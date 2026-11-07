#include <set>
#include <thread>
#include <unordered_set>

#include "ccxt_proxy.h"
#include "common_types/log_event_types.h"
#include "datetime_utils.h"
#include "disk_logging/log.h"
#include "live_infra/config.h"
#include "live_infra/constants.h"
#include "live_infra/live_trade_ticker.h"
#include "live_infra/state_updater.h"
#include "market_data/state_machine_dispatcher.h"
#include "market_making/dtypes/order.h"
#include "market_making/dtypes/order_fill_info.h"
#include "market_making/engine.h"
#include "market_making/prediction_market_utils.h"
#include "market_making/utils.h"
#include "sqlite_datasets.h"

namespace {

struct PolymarketPollContext {
    std::vector<PolymarketEventMeta> all_markets;
    std::set<std::string> engine_condition_ids;
    std::unordered_map<std::string, const PolymarketEventMeta*>
        loaded_meta_by_cid;
    long long now_ms;
};

bool is_stale_polymarket_meta(const PolymarketEventMeta& meta,
                              long long now_ms) {
    constexpr long long kRetentionMs = 7LL * 24 * 3600 * 1000;
    if (meta.event_end_time_ms <= 0) return false;
    if (now_ms - meta.event_end_time_ms <= kRetentionMs) return false;
    return meta.closed || !meta.active || !meta.accepting_orders ||
           meta.event_end_time_ms < now_ms;
}

double resolve_trade_settlement_price(const Core::Trade& trade,
                                      const Core::TradeTicker* ticker,
                                      const PolymarketEventMeta& meta) {
    if (!meta.outcome_prices.empty() && !trade.clob_token_id.empty()) {
        auto idx_it =
            ticker->clob_id_to_outcome_index.find(trade.clob_token_id);
        if (idx_it != ticker->clob_id_to_outcome_index.end() &&
            idx_it->second < (int)meta.outcome_prices.size()) {
            return meta.outcome_prices[idx_it->second];
        }
    }
    if (!meta.outcome_prices.empty()) {
        return meta.outcome_prices[0];
    }
    return 0.0;
}

void settle_polymarket_trade(Live::StateUpdater& updater,
                             Core::TradeTicker* ticker, Core::Trade& trade,
                             double settlement_price,
                             long long settlement_time_ms) {
    mpfr::mpreal remaining = trade.open_quantity - trade.closed_quantity;
    if (remaining <= 0) return;

    Core::Order settlement_order =
        Core::PolymarketOrderUtils::create_polymarket_settlement_order(
            trade, settlement_price, settlement_time_ms);

    ticker->add_synthethic_order_for_event_creation(settlement_order);

    Core::OrderFillInfo fill_info =
        Core::PolymarketOrderUtils::create_polymarke_synthethic_fill(
            settlement_order, settlement_price, settlement_time_ms);
    updater.on_order_fill(fill_info);
}

static std::chrono::steady_clock::time_point s_last_cleanup_time{};
static std::atomic<long long> s_last_orphan_cancel_time_ms{0};
static std::atomic<bool> s_orphan_cancel_running{false};

void cleanup_closed_polymarket_tickers(Live::StateUpdater& updater) {
    auto now = std::chrono::steady_clock::now();
    if (now - s_last_cleanup_time < std::chrono::minutes(5)) return;
    s_last_cleanup_time = now;

    auto& engine = Core::Engine::getInstance();
    int removed_count = 0;

    for (auto& strategy : engine.get_strategies()) {
        if (!strategy->is_trading_on_prediction_market) continue;

        std::vector<std::string> to_remove;
        {
            auto tickers_lock = strategy->lock_on_pred_market_strat();
            for (const auto& [symbol, ticker] : strategy->trade_tickers) {
                if (!ticker->is_closed.load()) continue;
                std::lock_guard<std::mutex> tl(ticker->trading_mutex);
                if (ticker->orders.empty() && ticker->active_trades.empty()) {
                    to_remove.push_back(symbol);
                }
            }
        }

        for (const auto& symbol : to_remove) {
            engine.remove_inventories_by_symbol(symbol, "polymarket", "spot");
            {
                auto tickers_lock = strategy->lock_on_pred_market_strat();
                auto ticker_it = strategy->trade_tickers.find(symbol);
                if (ticker_it != strategy->trade_tickers.end()) {
                    updater.unregister_polymarket_ticker(*ticker_it->second);
                }
            }
            strategy->remove_trade_ticker(symbol);
            removed_count++;
        }
    }

    if (removed_count > 0) {
        LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_POLYMARKET_POLL_RESULT,
                 "Removed " + std::to_string(removed_count) +
                     " closed polymarket tickers",
                 (nlohmann::json{{"phase", "closed_tickers_removed"},
                                 {"removed_count", removed_count}}));
    }
}

void handle_polymarket_market_created(Live::StateUpdater& updater,
                                      const PolymarketEventMeta& meta) {
    auto& engine = Core::Engine::getInstance();
    auto strategies = engine.get_strategies();

    for (auto& strategy : strategies) {
        std::unique_lock<std::shared_mutex> strategy_lock(
            strategy->trade_tickers_mutex);

        if (!strategy->is_trading_on_prediction_market) continue;

        if (!strategy->strategy_config.accepts_series_slug(meta.series_slug)) {
            continue;
        }

        if (strategy->trade_tickers.count(meta.condition_id)) continue;

        Live::OrderFrequencyLimits order_frequency_limits;
        order_frequency_limits.num_max_orders_10_sec =
            strategy->strategy_config.num_max_orders_10_sec;
        order_frequency_limits.num_max_orders_1_min =
            strategy->strategy_config.num_max_orders_1_min;
        order_frequency_limits.num_max_orders_1_hour =
            strategy->strategy_config.num_max_orders_1_hour;

        auto ticker_ptr = std::make_shared<Live::LiveTradeTicker>(
            strategy->strategy_config.exchanges_list, meta.condition_id,
            "USDC", "OUTCOME", strategy->strategy_config.id, updater,
            order_frequency_limits);
        ticker_ptr->configure_from_strategy(strategy->strategy_config);
        ticker_ptr->populate_polymarket_fields_from_meta(meta);
        ticker_ptr->series_slug = meta.series_slug;

        updater.register_polymarket_ticker(ticker_ptr);
        strategy->add_trade_ticker_lockless(meta.condition_id,
                                            std::move(ticker_ptr));
    }

    // Register event in engine-level polymarket event store
    auto pm_event = std::make_shared<Core::PolymarketActiveEvent>();
    pm_event->condition_id = meta.condition_id;
    pm_event->series_slug = meta.series_slug;
    pm_event->slug = meta.slug;
    pm_event->event_start_time_ms = meta.event_start_time_ms;
    pm_event->event_end_time_ms = meta.event_end_time_ms;
    pm_event->num_outcomes = meta.num_outcomes;
    pm_event->outcome_names = meta.outcome_names;
    pm_event->clob_token_ids = meta.clob_token_ids;
    engine.add_polymarket_event(pm_event);
}

void handle_polymarket_market_closed(Live::StateUpdater& updater,
                                     const PolymarketEventMeta& meta) {
    auto& engine = Core::Engine::getInstance();
    auto strategies = engine.get_strategies();
    const auto& symbol = meta.condition_id;

    double primary_price =
        meta.outcome_prices.empty() ? 0.0 : meta.outcome_prices[0];
    engine.update_price(symbol, "polymarket", "spot", primary_price);

    for (auto& strategy : strategies) {
        std::vector<std::string> resting_order_ids;
        std::vector<std::pair<Core::Trade, double>> settlements;
        std::optional<long long> account_id_opt;
        Core::TradeTicker* ticker = nullptr;

        {
            auto tickers_lock = strategy->lock_on_pred_market_strat();
            auto ticker_it = strategy->trade_tickers.find(symbol);
            if (ticker_it == strategy->trade_tickers.end()) continue;

            ticker = ticker_it->second.get();
            updater.unregister_polymarket_ticker(*ticker);
            std::lock_guard<std::mutex> ticker_lock(ticker->trading_mutex);

            ticker->is_closed.store(true);
            ticker->is_accepting_orders.store(false);
            ticker->is_active.store(false);

            for (const auto& order : ticker->orders) {
                if (!order.exchange_order_id.empty()) {
                    resting_order_ids.push_back(order.exchange_order_id);
                }
            }

            for (const auto& trade : ticker->active_trades) {
                double price =
                    resolve_trade_settlement_price(trade, ticker, meta);
                settlements.emplace_back(trade, price);
            }

            account_id_opt = strategy->get_exchange_and_market_type_account_id(
                "polymarket", "spot");

            if (!account_id_opt.has_value()) {
                LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_ACCOUNT_INIT_FAILED,
                          "No account id found for polymarket strategy " +
                              std::to_string(strategy->strategy_config.id),
                          (nlohmann::json{
                              {"strategy_id", strategy->strategy_config.id},
                              {"exchange_name", "polymarket"},
                              {"market_type", "spot"}}));
            }
        }

        if (!resting_order_ids.empty() && account_id_opt.has_value()) {
            updater.cancel_polymarket_orders_bulk(resting_order_ids,
                                                  account_id_opt.value());
        }

        long long settlement_time_ms = meta.event_end_time_ms;
        for (auto& [trade, price] : settlements) {
            settle_polymarket_trade(updater, ticker, trade, price,
                                    settlement_time_ms);
        }

        LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_POLYMARKET_TICKER_CLOSED,
                 "Closed live polymarket ticker: " + symbol,
                 (nlohmann::json{{"symbol", symbol},
                                 {"condition_id", meta.condition_id}}));
    }

    MarketData::MarketEvent resolved_event;
    resolved_event.type = MarketData::MarketEvent::POLYMARKET_MARKET_RESOLVED;
    resolved_event.polymarket_market_resolved =
        MarketData::PolymarketMarketResolvedEvent{
            meta.condition_id, DatetimeUtils::get_current_time_ms()};
    MarketData::StateMachineDispatcher::get_instance().enqueue(
        std::move(resolved_event));

    engine.remove_polymarket_event(symbol);
}

void poll_polymarket_events(Live::StateUpdater& updater,
                            std::chrono::steady_clock::time_point& last_poll,
                            const std::vector<std::string>& series_slugs) {
    auto now = std::chrono::steady_clock::now();
    if (now - last_poll < std::chrono::seconds(60)) {
        return;
    }
    last_poll = now;

    if (series_slugs.empty()) return;

    std::string base =
        Live::Config::getInstance().get_polymarket_series_base_path();

    PolymarketPollContext ctx;
    ctx.now_ms = DatetimeUtils::get_current_time_ms();
    long long window_start = ctx.now_ms - 30LL * 24 * 3600 * 1000;
    long long window_end = ctx.now_ms + 24LL * 3600 * 1000;

    for (const auto& slug : series_slugs) {
        auto markets = SqliteDatasets::load_polymarket_markets(
            base, slug, window_start, window_end);
        for (auto& m : markets) {
            m.series_slug = slug;
            ctx.all_markets.push_back(std::move(m));
        }
    }

    auto& engine = Core::Engine::getInstance();
    for (auto& strategy : engine.get_strategies()) {
        if (!strategy->is_trading_on_prediction_market) continue;

        auto tickers_lock = strategy->lock_on_pred_market_strat();
        for (const auto& [symbol, ticker] : strategy->trade_tickers) {
            if (!ticker->is_closed.load() &&
                ticker->is_prediction_market_active()) {
                ctx.engine_condition_ids.insert(symbol);
            }
        }
    }

    for (const auto& meta : ctx.all_markets) {
        ctx.loaded_meta_by_cid[meta.condition_id] = &meta;
    }

    updater.update_polymarket_event_metas(ctx.all_markets);

    int created_count = 0;
    for (const auto& meta : ctx.all_markets) {
        if (meta.accepting_orders_timestamp_ms > ctx.now_ms) continue;
        if (meta.event_end_time_ms < ctx.now_ms) continue;
        if (ctx.engine_condition_ids.count(meta.condition_id)) continue;
        handle_polymarket_market_created(updater, meta);
        created_count++;
    }

    if (created_count > 0) {
        LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_POLYMARKET_POLL_RESULT,
                 "Polymarket poll: created=" + std::to_string(created_count),
                 (nlohmann::json{{"phase", "tickers_created"},
                                 {"created_count", created_count}}));
    }

    std::vector<std::string> stream_condition_ids;
    for (auto& strategy : engine.get_strategies()) {
        if (!strategy->is_trading_on_prediction_market) continue;
        auto tickers_lock = strategy->lock_on_pred_market_strat();
        for (const auto& [symbol, ticker] : strategy->trade_tickers) {
            if (!ticker->is_closed.load()) {
                stream_condition_ids.push_back(symbol);
            }
        }
    }
    if (!stream_condition_ids.empty()) {
        CcxtProxy::instance().subscribe_polymarket_stream(stream_condition_ids);
    }
}

void subscribe_polymarket_public_stream(
    Live::StateUpdater& updater,
    std::chrono::steady_clock::time_point& last_sub_time) {
    auto now = std::chrono::steady_clock::now();
    if (now - last_sub_time < std::chrono::seconds(30)) return;
    last_sub_time = now;

    if (!updater.bars_realtime) return;

    auto& engine = Core::Engine::getInstance();
    std::vector<std::string> asset_ids;

    for (auto& strategy : engine.get_strategies()) {
        if (!strategy->is_trading_on_prediction_market) continue;
        auto tickers_lock = strategy->lock_on_pred_market_strat();
        for (const auto& [symbol, ticker] : strategy->trade_tickers) {
            if (ticker->is_closed.load()) continue;
            for (const auto& clob_id : ticker->clob_ids_by_index) {
                asset_ids.push_back(clob_id);
            }
        }
    }

    if (asset_ids.empty()) return;

    updater.bars_realtime->update_polymarket_subscriptions(asset_ids);
}

void cancel_orphaned_polymarket_orders(Live::StateUpdater& updater) {
    if (CcxtProxy::instance().is_polymarket_rate_limited()) return;

    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count();
    if (now_ms - s_last_orphan_cancel_time_ms.load(std::memory_order_relaxed) <
        5000)
        return;
    s_last_orphan_cancel_time_ms.store(now_ms, std::memory_order_relaxed);

    if (s_orphan_cancel_running.load()) return;
    s_orphan_cancel_running.store(true);

    std::vector<std::pair<long long, int>> account_ids;
    const auto& pm_accounts = Live::Config::getInstance().polymarket_accounts;
    for (const auto& [account_id, _] : pm_accounts) {
        account_ids.emplace_back(account_id, 0);
    }

    auto* updater_ptr = &updater;
    std::thread([updater_ptr, account_ids = std::move(account_ids)]() {
        try {
            auto& proxy = CcxtProxy::instance();
            auto& engine = Core::Engine::getInstance();

            for (const auto& [account_id, _] : account_ids) {
                // Fetch exchange orders first, then snapshot engine state
                auto exchange_orders =
                    proxy.get_polymarket_all_open_orders(account_id);

                long long current_time = engine.get_time();

                std::unordered_set<std::string> exchange_order_id_set;
                for (const auto& eo : exchange_orders) {
                    exchange_order_id_set.insert(eo.exchange_order_id);
                }

                // With optimistic order ID assignment (EIP-712 hash set
                // before HTTP), every polymarket order has an
                // exchange_order_id from the moment it's created. No more
                // "stuck orders" with empty IDs.
                std::unordered_set<std::string> engine_ids;
                std::vector<Core::Order> engine_orders;

                for (const auto& order : engine.get_orders()) {
                    if (!order.is_prediction_market_order) continue;
                    if (order.account_id != account_id) continue;
                    if (order.exchange_order_id.empty()) continue;

                    engine_ids.insert(order.exchange_order_id);
                    engine_orders.push_back(order);
                }

                // Direction 1: exchange orders not in engine -> cancel on
                // exchange
                {
                    std::vector<std::string> orphaned_ids;
                    for (const auto& eo : exchange_orders) {
                        if (engine_ids.count(eo.exchange_order_id) != 0)
                            continue;
                        orphaned_ids.push_back(eo.exchange_order_id);
                    }
                    if (!orphaned_ids.empty()) {
                        LOG_INFO(
                            Core::
                                TRADING_LOG_EVENT_TYPE_POLYMARKET_ORPHAN_CANCEL_FAILED,
                            "Canceling " + std::to_string(orphaned_ids.size()) +
                                " orphaned exchange orders for account_id=" +
                                std::to_string(account_id),
                            (nlohmann::json{
                                {"phase", "orphan_cancel"},
                                {"account_id", account_id},
                                {"orphaned_count", orphaned_ids.size()}}));
                        updater_ptr->cancel_polymarket_orders_bulk(orphaned_ids,
                                                                   account_id);
                    }
                }
            }
        } catch (const std::exception& e) {
            LOG_ERROR(
                Core::TRADING_LOG_EVENT_TYPE_POLYMARKET_ORPHAN_CANCEL_FAILED,
                std::string("cancel_orphaned_polymarket_orders exception: ") +
                    e.what(),
                (nlohmann::json{{"what", e.what()}}));
        } catch (...) {
            LOG_ERROR(
                Core::TRADING_LOG_EVENT_TYPE_POLYMARKET_ORPHAN_CANCEL_FAILED,
                "cancel_orphaned_polymarket_orders unknown exception",
                (nlohmann::json{{"what", "unknown"}}));
        }
        s_orphan_cancel_running.store(false);
    }).detach();
}

void update_polymarket_ticker_inventories(Live::StateUpdater& updater) {
    static std::chrono::steady_clock::time_point s_last_inventory_update{};
    auto now = std::chrono::steady_clock::now();
    if (now - s_last_inventory_update < std::chrono::seconds(5)) return;
    s_last_inventory_update = now;

    const auto& pm_accounts = Live::Config::getInstance().polymarket_accounts;
    if (pm_accounts.empty()) return;

    // Fetch inventory outside of any ticker locks (HTTP calls)
    // condition_id -> vector<(outcome_name, size)>
    std::unordered_map<std::string, std::vector<std::pair<std::string, double>>>
        inventory_by_condition;

    auto& proxy = CcxtProxy::instance();
    for (const auto& [account_id, _] : pm_accounts) {
        auto inv_state =
            proxy.get_inventory_state("polymarket", "spot", account_id);
        if (!inv_state.has_value()) continue;

        for (const auto& inv : inv_state.value().second) {
            const std::string& condition_id = inv.asset;
            const std::string& outcome_name = inv.quote_conversion_symbol;
            double size = inv.total.toDouble();
            inventory_by_condition[condition_id].emplace_back(outcome_name,
                                                              size);
        }
    }

    // Update tickers under shared lock (atomics only, no trading_mutex needed)
    auto& engine = Core::Engine::getInstance();
    for (auto& strategy : engine.get_strategies()) {
        if (!strategy->is_trading_on_prediction_market) continue;

        auto tickers_lock = strategy->lock_on_pred_market_strat();
        for (const auto& [symbol, ticker] : strategy->trade_tickers) {
            if (ticker->is_closed.load()) continue;
            if (!ticker->is_trading_on_prediction_market.load()) continue;

            auto it = inventory_by_condition.find(ticker->condition_id);
            int num_outcomes = ticker->num_outcomes.load();

            for (int i = 0; i < num_outcomes &&
                            i < Core::TradeTicker::MAX_POLYMARKET_OUTCOMES;
                 i++) {
                double size = 0.0;
                if (it != inventory_by_condition.end()) {
                    for (const auto& [name, sz] : it->second) {
                        if (i < (int)ticker->outcome_names_by_index.size() &&
                            name == ticker->outcome_names_by_index[i]) {
                            size = sz;
                            break;
                        }
                    }
                }
                ticker->update_outcome_exchange_inventory(i, size);
            }
        }
    }
}

}  // namespace

void Live::StateUpdater::verify_polymarket_order_states() {
    auto& proxy = CcxtProxy::instance();
    if (proxy.is_polymarket_rate_limited()) return;

    auto& engine = Core::Engine::getInstance();
    const auto& pm_accounts = Live::Config::getInstance().polymarket_accounts;

    for (const auto& [account_id, _] : pm_accounts) {
        // Step 1: bulk fetch open orders with full state
        auto open_orders = proxy.get_polymarket_all_open_orders(account_id);
        std::unordered_map<std::string, Core::OrderResNormalized>
            open_order_map;
        for (auto& oo : open_orders) {
            open_order_map.emplace(oo.exchange_order_id, std::move(oo));
        }

        // Step 2: collect engine polymarket orders for this account
        std::vector<Core::Order> engine_pm_orders;
        for (const auto& order : engine.get_orders()) {
            if (!order.is_prediction_market_order) continue;
            if (order.account_id != account_id) continue;
            if (order.exchange_order_id.empty()) continue;
            engine_pm_orders.push_back(order);
        }

        if (engine_pm_orders.empty()) continue;

        int live_count = 0;
        int query_count = 0;

        for (auto& order : engine_pm_orders) {
            // Step 3: if in open orders -> use bulk-fetched state
            auto it = open_order_map.find(order.exchange_order_id);
            if (it != open_order_map.end()) {
                live_count++;
                this->handle_order_state_check(order, it->second);
                continue;
            }

            // Step 4: not in open orders -> query individually
            query_count++;
            auto order_res = proxy.query_order(order);
            if (order_res.has_value()) {
                this->handle_order_state_check(order, order_res.value());
            } else {
                // Not found at all - grace period before removal
                // auto age_ms = DatetimeUtils::get_current_time_ms() -
                //               order.create_timestamp;
                // long long min_age =
                //     order.is_order_creation_acknowledged ? 5000 : 0;
                // if (age_ms >= min_age) {
                //     LOG_INFO(
                //         Core::
                //             TRADING_LOG_EVENT_TYPE_POLYMARKET_ORDER_STATE_VERIFY,
                //         "verify_polymarket_order_states: removing unfound "
                //         "order " +
                //             order.exchange_order_id,
                //         (nlohmann::json{
                //             {"phase", "unfound_order_removed"},
                //             {"exchange_order_id", order.exchange_order_id},
                //             {"age_ms", age_ms}}));
                //     engine.order_canceled_ack(order, true);
                // }
            }
        }
    }
}

void Live::StateUpdater::init_polymarket_tickers() {
    if (this->polymarket_series_slugs.empty()) return;

    std::string base =
        Live::Config::getInstance().get_polymarket_series_base_path();

    long long now_ms = DatetimeUtils::get_current_time_ms();
    long long window_start = now_ms - 30LL * 24 * 3600 * 1000;
    long long window_end = now_ms + 24LL * 3600 * 1000;

    std::vector<PolymarketEventMeta> all_markets;
    for (const auto& slug : this->polymarket_series_slugs) {
        auto markets = SqliteDatasets::load_polymarket_markets(
            base, slug, window_start, window_end);
        for (auto& m : markets) {
            m.series_slug = slug;
            all_markets.push_back(std::move(m));
        }
    }

    this->update_polymarket_event_metas(all_markets);

    auto& engine = Core::Engine::getInstance();
    int tickers_before = 0;
    for (auto& strategy : engine.get_strategies()) {
        if (!strategy->is_trading_on_prediction_market) continue;
        auto lock = strategy->lock_on_pred_market_strat();
        tickers_before += strategy->trade_tickers.size();
    }

    for (const auto& meta : all_markets) {
        handle_polymarket_market_created(*this, meta);
    }

    int tickers_after = 0;
    for (auto& strategy : engine.get_strategies()) {
        if (!strategy->is_trading_on_prediction_market) continue;
        auto lock = strategy->lock_on_pred_market_strat();
        tickers_after += strategy->trade_tickers.size();
    }

    LOG_INFO(
        Core::TRADING_LOG_EVENT_TYPE_POLYMARKET_INIT_COMPLETED,
        "init_polymarket_tickers: loaded " +
            std::to_string(all_markets.size()) + " events, created " +
            std::to_string(tickers_after - tickers_before) + " tickers",
        (nlohmann::json{{"events_loaded", all_markets.size()},
                        {"tickers_created", tickers_after - tickers_before}}));
}

void Live::StateUpdater::cancel_orders_if_near_shutdown() {
    auto elapsed = std::chrono::steady_clock::now() - this->program_start_time;
    auto remaining =
        std::chrono::minutes(Live::Constants::PROGRAM_MAX_RUNTIME_MINUTES) -
        elapsed;
    if (remaining > std::chrono::minutes(
                        Live::Constants::PRE_SHUTDOWN_CANCEL_BUFFER_MINUTES)) {
        return;
    }

    for (auto& strategy : Core::Engine::getInstance().get_strategies()) {
        if (!strategy->is_trading_on_prediction_market) continue;
        strategy->set_stop_making_markets(
            true, Core::StrategyPauseReason::PRE_SHUTDOWN_CANCEL_BUFFER);
    }
}

void Live::StateUpdater::schedule_polymarket_delayed_task(
    std::chrono::milliseconds delay, std::function<void()> task) {
    std::lock_guard<std::mutex> lock(polymarket_delayed_tasks_mutex_);
    polymarket_delayed_tasks_.push_back(
        {std::chrono::steady_clock::now() + delay, std::move(task)});
}

void Live::StateUpdater::process_polymarket_delayed_tasks() {
    std::vector<std::function<void()>> ready;
    {
        std::lock_guard<std::mutex> lock(polymarket_delayed_tasks_mutex_);
        auto now = std::chrono::steady_clock::now();
        auto it = polymarket_delayed_tasks_.begin();
        while (it != polymarket_delayed_tasks_.end()) {
            if (now >= it->due_time) {
                ready.push_back(std::move(it->task));
                it = polymarket_delayed_tasks_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& t : ready) t();
}

void Live::StateUpdater::dispatch_polymarket_cancel_resolution(
    const Core::Order& order) {
    {
        std::lock_guard<std::mutex> lock(
            polymarket_cancel_resolution_ids_mutex);
        if (!polymarket_cancel_resolution_ids.insert(order.exchange_order_id)
                 .second) {
            return;  // already dispatched
        }
    }

    Core::Order order_copy = order;
    schedule_polymarket_delayed_task(
        POLYMARKET_CANCEL_RESOLUTION_DELAY_MS, [this, order_copy]() mutable {
            misc_task_thread_pool->enqueue([this, order_copy]() mutable {
                try {
                    auto& proxy = CcxtProxy::instance();
                    auto& engine = Core::Engine::getInstance();

                    auto order_res = proxy.query_order(order_copy);
                    if (order_res.has_value()) {
                        this->handle_order_state_check(order_copy,
                                                       order_res.value());
                    } else {
                        LOG_INFO(
                            Core::
                                TRADING_LOG_EVENT_TYPE_ORDER_NOT_FOUND_ON_EXCHANGE,
                            "polymarket_cancel_resolution: order not found " +
                                order_copy.exchange_order_id + ", removing",
                            (nlohmann::json{{"exchange_order_id",
                                             order_copy.exchange_order_id}}));
                        engine.order_canceled_ack(order_copy, true);
                    }
                } catch (const std::exception& e) {
                    LOG_ERROR(
                        Core::
                            TRADING_LOG_EVENT_TYPE_POLYMARKET_ORPHAN_CANCEL_FAILED,
                        std::string(
                            "polymarket_cancel_resolution exception: ") +
                            e.what() +
                            " order_id=" + order_copy.exchange_order_id,
                        (nlohmann::json{
                            {"exchange_order_id", order_copy.exchange_order_id},
                            {"what", e.what()}}));
                } catch (...) {
                    LOG_ERROR(
                        Core::
                            TRADING_LOG_EVENT_TYPE_POLYMARKET_ORPHAN_CANCEL_FAILED,
                        "polymarket_cancel_resolution unknown exception for " +
                            order_copy.exchange_order_id,
                        (nlohmann::json{
                            {"exchange_order_id", order_copy.exchange_order_id},
                            {"what", "unknown"}}));
                }
            });
        });
}

void Live::StateUpdater::run_polymarket_tasks() {
    process_polymarket_delayed_tasks();

    // Clean dedup set every ~1 minute
    {
        auto now = std::chrono::steady_clock::now();
        if (now - last_cancel_resolution_ids_cleanup >
            std::chrono::seconds(60)) {
            last_cancel_resolution_ids_cleanup = now;
            std::lock_guard<std::mutex> lock(
                polymarket_cancel_resolution_ids_mutex);
            polymarket_cancel_resolution_ids.clear();
        }
    }

    cancel_orders_if_near_shutdown();
    cleanup_closed_polymarket_tickers(*this);
    poll_polymarket_events(*this, this->last_polymarket_events_poll_time,
                           this->polymarket_series_slugs);
    subscribe_polymarket_public_stream(*this,
                                       this->last_polymarket_public_sub_time_);
    run_prediction_market_mint_contracts();
    run_prediction_market_redeem_contracts();
    cancel_orphaned_polymarket_orders(*this);
    update_polymarket_ticker_inventories(*this);
}

void Live::StateUpdater::run_prediction_market_mint_contracts() {
    auto now = std::chrono::steady_clock::now();
    if (now - this->last_mint_contracts_poll_time < std::chrono::seconds(5)) {
        return;
    }
    if (this->mint_contracts_running.load()) return;

    this->last_mint_contracts_poll_time = now;
    this->mint_contracts_running.store(true);

    struct MintStrategyWork {
        int strategy_id;
        long long account_id;
        std::vector<Core::MintContractsRequest> mint_requests;
    };

    auto& engine = Core::Engine::getInstance();
    std::vector<MintStrategyWork> all_work;

    for (auto& strategy : engine.get_strategies()) {
        if (!strategy->is_trading_on_prediction_market) continue;
        if (strategy->is_disabled_now()) continue;

        auto mint_requests = strategy->get_mint_contracts(engine);
        if (mint_requests.empty()) continue;

        auto account_id_opt = strategy->get_exchange_and_market_type_account_id(
            "polymarket", "spot");

        MintStrategyWork work;
        work.strategy_id = strategy->strategy_config.id;
        work.account_id = account_id_opt.value_or(0);
        work.mint_requests = std::move(mint_requests);
        all_work.push_back(std::move(work));
    }

    if (all_work.empty()) {
        this->mint_contracts_running.store(false);
        return;
    }

    std::thread([this, all_work = std::move(all_work)]() {
        try {
            auto& proxy = CcxtProxy::instance();
            auto& engine = Core::Engine::getInstance();

            for (const auto& work : all_work) {
                LOG_INFO(
                    Core::
                        TRADING_LOG_EVENT_TYPE_POLYMARKET_SYNTHETIC_CONTRACT_MINT,
                    "Mint contracts callback returned " +
                        std::to_string(work.mint_requests.size()) +
                        " requests for strategy " +
                        std::to_string(work.strategy_id),
                    (nlohmann::json{
                        {"phase", "callback_result"},
                        {"strategy_id", work.strategy_id},
                        {"num_requests", work.mint_requests.size()}}));

                for (const auto& req : work.mint_requests) {
                    if (req.quantity <= 0) continue;

                    LOG_INFO(
                        Core::
                            TRADING_LOG_EVENT_TYPE_POLYMARKET_SYNTHETIC_CONTRACT_MINT,
                        "Minting " + std::to_string(req.quantity) +
                            " contracts for condition " + req.condition_id +
                            " (neg_risk=" + (req.neg_risk ? "true" : "false") +
                            ")",
                        (nlohmann::json{{"phase", "minting"},
                                        {"strategy_id", work.strategy_id},
                                        {"condition_id", req.condition_id},
                                        {"quantity", req.quantity},
                                        {"neg_risk", req.neg_risk}}));

                    auto result = proxy.mint_prediction_market_contracts(
                        req.condition_id, req.neg_risk, req.quantity,
                        work.account_id);

                    if (!result.success) {
                        LOG_ERROR(
                            Core::
                                TRADING_LOG_EVENT_TYPE_POLYMARKET_SYNTHETIC_CONTRACT_MINT_FAILED,
                            "Mint failed: " + result.error +
                                " condition=" + req.condition_id,
                            (nlohmann::json{{"strategy_id", work.strategy_id},
                                            {"condition_id", req.condition_id},
                                            {"quantity", req.quantity},
                                            {"error", result.error}}));
                        continue;
                    }

                    LOG_INFO(
                        Core::
                            TRADING_LOG_EVENT_TYPE_POLYMARKET_SYNTHETIC_CONTRACT_MINT,
                        "Mint success: tx=" + result.tx_hash +
                            " gas=" + std::to_string(result.gas_used) +
                            " condition=" + req.condition_id,
                        (nlohmann::json{{"phase", "success"},
                                        {"strategy_id", work.strategy_id},
                                        {"condition_id", req.condition_id},
                                        {"tx_hash", result.tx_hash},
                                        {"gas_used", result.gas_used}}));

                    // Post-mint: create synthetic fills on the main engine
                    std::vector<std::string> clob_ids;
                    int num_outcomes = 0;

                    for (auto& strategy : engine.get_strategies()) {
                        if (strategy->strategy_config.id != work.strategy_id)
                            continue;

                        auto tickers_lock =
                            strategy->lock_on_pred_market_strat();
                        auto ticker_it =
                            strategy->trade_tickers.find(req.condition_id);
                        if (ticker_it == strategy->trade_tickers.end()) {
                            LOG_ERROR(
                                Core::
                                    TRADING_LOG_EVENT_TYPE_POLYMARKET_MINT_TICKER_MISSING,
                                "No ticker for minted condition " +
                                    req.condition_id,
                                (nlohmann::json{
                                    {"strategy_id", work.strategy_id},
                                    {"condition_id", req.condition_id}}));
                            break;
                        }
                        auto* ticker = ticker_it->second.get();
                        std::lock_guard<std::mutex> tl(ticker->trading_mutex);
                        clob_ids = ticker->clob_ids_by_index;
                        num_outcomes = ticker->num_outcomes.load();
                        break;
                    }

                    if (clob_ids.empty()) continue;

                    long long timestamp = DatetimeUtils::get_current_time_ms();
                    double mint_price =
                        (num_outcomes > 0) ? (1.0 / num_outcomes) : 0.50;

                    for (const auto& clob_id : clob_ids) {
                        Core::Order mint_order = Core::PolymarketOrderUtils::
                            create_polymarket_mint_order(
                                req.condition_id, clob_id, work.strategy_id,
                                work.account_id, timestamp, mint_price,
                                req.quantity);

                        for (auto& strategy : engine.get_strategies()) {
                            if (strategy->strategy_config.id !=
                                work.strategy_id)
                                continue;
                            auto tickers_lock =
                                strategy->lock_on_pred_market_strat();
                            auto ticker_it =
                                strategy->trade_tickers.find(req.condition_id);
                            if (ticker_it != strategy->trade_tickers.end()) {
                                ticker_it->second
                                    ->add_synthethic_order_for_event_creation(
                                        mint_order);
                            }
                            break;
                        }

                        Core::OrderFillInfo fill = Core::PolymarketOrderUtils::
                            create_polymarke_synthethic_fill(
                                mint_order, mint_price, timestamp);
                        this->on_order_fill(fill);
                    }

                    LOG_INFO(
                        Core::
                            TRADING_LOG_EVENT_TYPE_POLYMARKET_SYNTHETIC_CONTRACT_MINT,
                        "Created synthetic mint trades for " +
                            req.condition_id + " (" +
                            std::to_string(clob_ids.size()) + " outcomes)",
                        (nlohmann::json{{"phase", "synthetic_fills_created"},
                                        {"condition_id", req.condition_id},
                                        {"num_outcomes", clob_ids.size()}}));
                }
            }
        } catch (const std::exception& e) {
            LOG_ERROR(
                Core::
                    TRADING_LOG_EVENT_TYPE_POLYMARKET_SYNTHETIC_CONTRACT_MINT_FAILED,
                std::string("mint_contracts exception: ") + e.what(),
                (nlohmann::json{{"what", e.what()}}));
        } catch (...) {
            LOG_ERROR(
                Core::
                    TRADING_LOG_EVENT_TYPE_POLYMARKET_SYNTHETIC_CONTRACT_MINT_FAILED,
                "mint_contracts unknown exception",
                (nlohmann::json{{"what", "unknown"}}));
        }
        this->mint_contracts_running.store(false);
    }).detach();
}

void Live::StateUpdater::update_polymarket_event_metas(
    const std::vector<PolymarketEventMeta>& metas) {
    auto& engine = Core::Engine::getInstance();
    std::unordered_set<std::string> active_condition_ids;
    for (auto& strategy : engine.get_strategies()) {
        if (!strategy->is_trading_on_prediction_market) continue;

        auto tickers_lock = strategy->lock_on_pred_market_strat();
        for (const auto& [symbol, ticker] : strategy->trade_tickers) {
            if (!ticker->is_closed.load()) {
                active_condition_ids.insert(symbol);
            }
        }
    }

    std::lock_guard<std::mutex> lock(polymarket_metas_mutex);
    for (const auto& m : metas) {
        polymarket_event_metas.insert_or_assign(m.condition_id, m);
    }

    long long now_ms = DatetimeUtils::get_current_time_ms();
    for (auto it = polymarket_event_metas.begin();
         it != polymarket_event_metas.end();) {
        if (active_condition_ids.count(it->first) == 0 &&
            is_stale_polymarket_meta(it->second, now_ms)) {
            it = polymarket_event_metas.erase(it);
            continue;
        }
        ++it;
    }
}

std::optional<PolymarketEventMeta>
Live::StateUpdater::get_polymarket_event_meta(const std::string& condition_id) {
    std::lock_guard<std::mutex> lock(polymarket_metas_mutex);
    auto it = polymarket_event_metas.find(condition_id);
    if (it == polymarket_event_metas.end()) return std::nullopt;
    return it->second;
}

std::vector<PolymarketEventMeta>
Live::StateUpdater::get_all_polymarket_event_metas() {
    std::lock_guard<std::mutex> lock(polymarket_metas_mutex);
    std::vector<PolymarketEventMeta> result;
    result.reserve(polymarket_event_metas.size());
    for (const auto& [_, meta] : polymarket_event_metas) {
        result.push_back(meta);
    }
    return result;
}

void Live::StateUpdater::run_prediction_market_redeem_contracts() {
    auto now = std::chrono::steady_clock::now();
    if (now - this->last_redeem_contracts_poll_time < std::chrono::seconds(5)) {
        return;
    }
    if (this->redeem_contracts_running.load()) return;

    this->last_redeem_contracts_poll_time = now;
    this->redeem_contracts_running.store(true);

    struct RedeemContractAttempt {
        std::string condition_id;
        std::string slug;
        bool neg_risk;
    };

    // Collect candidates and account IDs on the main thread
    std::vector<std::pair<std::vector<RedeemContractAttempt>, long long>>
        strategy_candidates;

    auto& engine = Core::Engine::getInstance();
    for (auto& strategy : engine.get_strategies()) {
        if (!strategy->is_trading_on_prediction_market) continue;

        std::vector<RedeemContractAttempt> candidates;
        {
            auto tickers_lock = strategy->lock_on_pred_market_strat();
            for (const auto& [symbol, ticker] : strategy->trade_tickers) {
                if (ticker->is_prediction_market_active()) continue;
                if (ticker->get_num_active_trades() == 0) continue;
                candidates.push_back({ticker->condition_id, ticker->slug,
                                      ticker->is_neg_risk.load()});
            }
        }

        if (candidates.empty()) continue;

        auto redeem_account_id_opt =
            strategy->get_exchange_and_market_type_account_id("polymarket",
                                                              "spot");
        strategy_candidates.emplace_back(std::move(candidates),
                                         redeem_account_id_opt.value_or(0));
    }

    if (strategy_candidates.empty()) {
        this->redeem_contracts_running.store(false);
        return;
    }

    std::thread([this, strategy_candidates = std::move(strategy_candidates)]() {
        try {
            auto& proxy = CcxtProxy::instance();

            for (const auto& [candidates, account_id] : strategy_candidates) {
                for (const auto& candidate : candidates) {
                    auto meta_opt =
                        this->get_polymarket_event_meta(candidate.condition_id);
                    if (!meta_opt.has_value()) continue;

                    const auto& meta = meta_opt.value();
                    if (meta.outcome_prices.empty()) continue;

                    bool has_resolved_outcome = false;
                    for (double p : meta.outcome_prices) {
                        if (p == 0.0 || p == 1.0) {
                            has_resolved_outcome = true;
                            break;
                        }
                    }
                    if (!has_resolved_outcome) continue;

                    LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_POLYMARKET_REDEEM,
                             "Attempting redeem for condition " +
                                 candidate.condition_id +
                                 " (slug: " + candidate.slug + ")",
                             (nlohmann::json{
                                 {"phase", "attempt"},
                                 {"condition_id", candidate.condition_id},
                                 {"slug", candidate.slug},
                                 {"account_id", account_id}}));

                    auto result = proxy.redeem_prediction_market_contracts(
                        candidate.condition_id, candidate.neg_risk,
                        candidate.slug, account_id);

                    if (result.success) {
                        if (result.status == "no_tokens") {
                            LOG_INFO(
                                Core::TRADING_LOG_EVENT_TYPE_POLYMARKET_REDEEM,
                                "No tokens to redeem for " +
                                    candidate.condition_id + ", settling",
                                (nlohmann::json{
                                    {"phase", "no_tokens"},
                                    {"condition_id", candidate.condition_id}}));
                        } else {
                            LOG_INFO(
                                Core::TRADING_LOG_EVENT_TYPE_POLYMARKET_REDEEM,
                                "Redeem success: tx=" + result.tx_hash +
                                    " usdc_gained=" +
                                    std::to_string(result.usdc_gained) +
                                    " condition=" + candidate.condition_id,
                                (nlohmann::json{
                                    {"phase", "success"},
                                    {"condition_id", candidate.condition_id},
                                    {"tx_hash", result.tx_hash},
                                    {"usdc_gained", result.usdc_gained}}));
                        }
                        handle_polymarket_market_closed(*this, meta);
                    } else {
                        LOG_ERROR(
                            Core::
                                TRADING_LOG_EVENT_TYPE_POLYMARKET_REDEEM_FAILED,
                            "Redeem failed for " + candidate.condition_id +
                                ": " + result.error,
                            (nlohmann::json{
                                {"condition_id", candidate.condition_id},
                                {"error", result.error}}));
                    }
                }
            }
        } catch (const std::exception& e) {
            LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_POLYMARKET_REDEEM_FAILED,
                      std::string("redeem_contracts exception: ") + e.what(),
                      (nlohmann::json{{"what", e.what()}}));
        } catch (...) {
            LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_POLYMARKET_REDEEM_FAILED,
                      "redeem_contracts unknown exception",
                      (nlohmann::json{{"what", "unknown"}}));
        }
        this->redeem_contracts_running.store(false);
    }).detach();
}