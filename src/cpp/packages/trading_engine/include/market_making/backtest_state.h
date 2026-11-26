#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <functional>
#include <limits>
#include <queue>

#include "market_data/bar.h"
#include "market_data/bar_close_policy.h"
#include "market_data/dataset_manager.h"
#include "market_data/ohlcv_bar_meta.h"
#include "market_data/state_machine_dispatcher.h"
#include "market_data/utils.h"
#include "market_making/dtypes/engine_state_snapshot.h"
#include "market_making/dtypes/inventory.h"
#include "market_making/dtypes/market_data_dataset.h"
#include "market_making/dtypes/market_event.h"
#include "market_making/dtypes/order.h"
#include "market_making/dtypes/order_fill_info.h"
#include "market_making/dtypes/quote_asset.h"
#include "market_making/dtypes/simulation_input.h"
#include "market_making/engine.h"
#include "market_making/utils.h"

namespace Core {

struct BacktestExchange {
    std::string exchange_name;
    std::string market_type;
    std::vector<QuoteAsset> quote_assets;
};

struct BacktestParams {
    std::vector<BacktestExchange> exchanges;
    long long simulation_start_timestamp_ms;
    long long simulation_end_timestamp_ms;

    double fee_rate_coeff = 0;
    double futures_leverage = 10;
};

struct BacktestOhlcvDataset {
    ::MarketData::OhlcvBarMeta ohlcv_bar_meta;
    std::string dataset_path;
    std::string symbol;
    std::string exchange_name;
    std::string market_type;
};

struct BacktestFundingRateDataset {
    std::string dataset_path;
    std::string symbol;
    std::string exchange_name;
    std::string market_type;
};

struct FillCheckWindow {
    int bars_buffered = 0;
    double window_open = 0;
    double window_high = 0;
    double window_low = 0;
    double window_close = 0;
    long long window_first_open_time = 0;
    long long window_last_event_time = 0;
    std::string exchange_name;
    std::string market_type;
    std::string symbol;
};

struct SymbolRestingBook {
    std::vector<Order> orders;
    double max_buy_limit = -std::numeric_limits<double>::infinity();
    double min_sell_limit = std::numeric_limits<double>::infinity();
    // Resting takers fill at any trade price and are excluded from the
    // bounds above, so the bounds-skip is only safe when this is zero.
    int taker_count = 0;
};

struct ScheduledBookRemoval {
    long long effective_time_ms = 0;
    std::string exchange_name;
    std::string market_type;
    std::string symbol;
    unsigned long long order_id = 0;
};

struct ScheduledBookRemovalCompare {
    bool operator()(const ScheduledBookRemoval& a,
                    const ScheduledBookRemoval& b) const {
        return a.effective_time_ms > b.effective_time_ms;
    }
};

enum class BacktestHotpathKind {
    ScheduledRemovals,
    EngineProcessMarketEvent,
    FillCheck,
    GeneratePredictions,
    PollStrategyStates,
    Reconcile,
    Count
};

struct BacktestHotpathBucket {
    uint64_t total_ns = 0;
    uint64_t calls = 0;
    uint64_t max_ns = 0;
};

class BacktestState {
   public:
    using PacedOhlcvObserver = std::function<void(const MarketEvent&)>;

    static BacktestState& getInstance();

    BacktestState(const BacktestState&) = delete;
    BacktestState& operator=(const BacktestState&) = delete;

    void read_ohlcv_klines();
    void read_funding_rates();
    void read_aggr_trades();
    void read_trades();
    void read_polymarket_events();
    void read_polymarket_trades();
    void read_predictions();
    // Reads each unique fold_schedule.json referenced by a loaded strategy's
    // strategy_config.model_schedule_path, and emits MODEL_UPDATE events into
    // the chunked market_events queue. One warm-up event at chunk_start (using
    // the latest entry whose timestamp <= chunk_start) plus one event per
    // entry strictly within (chunk_start, chunk_end). schedule_path on the
    // event is the join key — engine dispatches to every matching strategy.
    void read_model_fold_events();
    void apply_pending_predictions();
    void handle_polymarket_market_created(const MarketEvent& market_event);
    void handle_polymarket_market_closed(const MarketEvent& market_event);
    void settle_polymarket_trade(Core::TradeTicker* ticker, Core::Trade& trade,
                                 double settlement_price,
                                 long long settlement_time_ms);
    void mint_polymarket_contracts(Core::Strategy* strategy,
                                   Core::TradeTicker* ticker,
                                   const PolymarketEventMeta& meta,
                                   long long timestamp);
    void mint_polymarket_contracts_from_requests(
        Core::Strategy* strategy, Core::TradeTicker* ticker,
        const PolymarketEventMeta& meta, long long timestamp,
        const std::vector<MintContractsRequest>& requests);
    void init_state();
    void add_sim_params(const Core::SimulationInput& params);

    [[nodiscard]] bool is_sim_autofilling_orders() const {
        return this->is_autofilling_orders;
    }

    [[nodiscard]] long long get_latency_to_exchange() const {
        return this->sim_params.latency_to_exchange;
    }

    [[nodiscard]] bool is_acking_cancels_at_exchange_removal() const {
        return this->sim_params.ack_cancels_at_exchange_removal;
    }

    void defer_cancel_ack(const Order& order) {
        this->deferred_cancel_acks_.insert_or_assign(order.id, order);
    }

    void mark_order_for_removal(const std::string& exchange_name,
                                const std::string& market_type,
                                const std::string& symbol,
                                unsigned long long order_id,
                                long long removal_timestamp);

    void run_backtest();
    void prepare_paced_backtest(const PacedOhlcvObserver& observer);
    [[nodiscard]] bool advance_paced_backtest_through(
        long long target_time_ms, const PacedOhlcvObserver& observer);
    [[nodiscard]] double get_market_data_loading_progress() const {
        const std::size_t total = streaming_datasets_total_.load();
        if (total == 0) return 0.0;
        return static_cast<double>(streaming_datasets_loaded_.load()) /
               static_cast<double>(total);
    }
    void finish_paced_backtest();
    void prefeed_market_events(const MarketEvent& market_event);
    void add_order(const Order& order);
    void remove_order(const std::string& exchange_name,
                      const std::string& market_type, const std::string& symbol,
                      const unsigned long long order_id);
    std::string get_fee_rates_key(const std::string& exchange_name,
                                  const std::string& market_type,
                                  bool is_taker);
    std::atomic<bool> is_paused{false};

    void toggle_pause() {
        is_paused.store(!is_paused.load(std::memory_order_relaxed),
                        std::memory_order_relaxed);
    }

    void stop();
    void init_strategies();
    // All trades accumulated during the backtest (open + closed). Filled
    // via update_engine_state_snapshot as the engine emits new_trades.
    // Distinct from Engine::get_trades() which is active-only. Used for
    // e2e fixture capture — emitted to <output_dir>/trades.json when the
    // sim config sets output_dir.
    const std::vector<Trade>& get_all_backtest_trades() const {
        return this->trades;
    }
    void init_ohlcv_datasets();
    void init_funding_rate_datasets();
    void flush_fill_records() {
        if (fill_record_stream_.is_open()) fill_record_stream_.flush();
    }
    void init_exchanges();
    void backtest_update_inventory(const Order& order, const mpfr::mpreal& fill_price,
                                   bool is_taker_execution);
    void update_quote_asset(const std::string& exchange_name,
                            const std::string& market_type,
                            long long account_id, const std::string& asset,
                            const mpfr::mpreal& quote_asset_delta);
    void update_fees(const Order& order, const mpfr::mpreal& fill_price,
                     bool is_taker_execution);
    void update_pnl(const Order& order, const mpfr::mpreal& fill_price);
    void update_engine_state_snapshot(const EngineStateSnapshot& snapshot);
    // Writes a JSON summary (PnL, fees, win rate, max DD, Sharpe from
    // the daily NAV samples, trade counts). Opt-in: runner calls this
    // when the sim config sets output_dir.
    void dump_summary_json(const std::string& path) const;
    // Writes the daily NAV / exposure samples as a JSON array. Separate from
    // summary.json so summary stays skimmable.
    void dump_equity_curve_json(const std::string& path) const;
    // When set, dump_equity_curve_json is also called after every appended
    // daily sample so killed long runs leave a viewable curve. Atomic write.
    void set_equity_curve_dump_path(const std::string& path) {
        this->equity_curve_dump_path = path;
    }
    void loop_market_events();
    void read_market_events();
    void print_debug_state(long long event_time);
    mpfr::mpreal get_fees_coeff(const std::string& exchange_name,
                                const std::string& market_type, bool is_taker);

    [[gnu::always_inline]] inline OrderFillInfo create_order_fill_info(
        const Order& order, const mpfr::mpreal& price, long long timestamp) {
        OrderFillInfo order_fill_info;
        order_fill_info.symbol = order.symbol;
        order_fill_info.strategy_id = order.strategy_id;
        order_fill_info.exchange_name = order.exchange_name;
        order_fill_info.market_type = order.market_type;
        order_fill_info.order_fill_timestamp = timestamp;
        order_fill_info.order_id = order.id;
        order_fill_info.side = order.side;
        order_fill_info.sizing_coeff_delta = order.sizing_coeff_delta;
        order_fill_info.prediction = order.prediction;
        order_fill_info.price = price;
        order_fill_info.fill_amount = order.quantity;
        order_fill_info.trade_id = order.trade_id;

        mpfr::mpreal fee_rate = ::Core::Engine::getInstance().get_fee_rate(
            order.exchange_name, order.market_type,
            order.type == (order.is_taker_in_simulation
                               ? Core::OrderType::MARKET
                               : Core::OrderType::LIMIT));
        order_fill_info.fee = OrderResFees{
            .currency = "USDT",
            .amount = (order.quantity * order.price) * fee_rate,
        };
        order_fill_info.kv_cache = order.kv_cache;
        order_fill_info.hedge_for_trade_id = order.hedge_for_trade_id;
        order_fill_info.hedge_symbol = order.hedge_for_trade_id.has_value()
                                           ? order.route_order_to_alt_symbol
                                           : std::nullopt;
        order_fill_info.account_id = order.account_id;
        order_fill_info.clob_token_id = order.clob_id;
        order_fill_info.trade_creation_type =
            Core::trade_creation_type_for(order.type_internal);
        order_fill_info.is_polymarket_fill_confirmed = true;
        order_fill_info.is_maker = !order.is_taker_in_simulation;
        order_fill_info.is_order_cancelled_before_fill =
            (order.cancel_timestamp > 0);
        order_fill_info.markout_config = order.markout_config;
        return order_fill_info;
    }

    void on_order_fill(const Order& order, const mpfr::mpreal& price,
                       long long timestamp, bool is_taker_execution);

    struct FillCheckBar {
        double open_price = 0;
        double high_price = 0;
        double low_price = 0;
        double close_price = 0;
        long long kline_open_time = 0;
        long long event_time = 0;
    };

    // A per-order fill decision: present => fill at `price` (taker flag
    // routed through to on_order_fill); absent => no fill for this order.
    struct ResolvedFill {
        mpfr::mpreal price;
        bool is_taker;
    };

    // Shared fill-matching core for both the bar-based (crypto/equities)
    // and trade-based (polymarket) paths. Owns the O(1) bounds-skip, the
    // common order-validity guards, and the deferred on_order_fill
    // application; the per-order match logic is supplied by `decide`.
    //
    // skip_low/skip_high bound the price move (bar H/L; for a trade,
    // both = trade price). decision_ts gates valid-from / removed /
    // expires; create_cutoff_ts (+ strictness) gates the create guard —
    // both kept per-call to preserve each path's exact prior semantics.
    template <typename FillDecider>
    [[gnu::always_inline]] inline void apply_resting_fills(
        const MarketEvent& market_event, SymbolRestingBook& book,
        double skip_low, double skip_high, long long decision_ts,
        long long create_cutoff_ts, bool create_cutoff_strict,
        FillDecider&& decide) {
        // Bounds-skip: no resting order can fill when the price move can't
        // cross any limit. Takers fill at any price and are excluded from
        // the bounds, so only skip when none are resting.
        if (book.taker_count == 0 && skip_low > book.max_buy_limit &&
            skip_high < book.min_sell_limit) {
            ++this->hotpath_fill_bounds_skips_;
            return;
        }

        struct PendingFill {
            Order order;
            mpfr::mpreal price;
            bool is_taker;
        };
        std::vector<PendingFill> pending_fills;

        for (const auto& order : book.orders) {
            if (order.exchange_name != market_event.exchange_name ||
                order.market_type != market_event.market_type) {
                continue;
            }

            if (order.valid_in_backtest_from_timestamp > 0 &&
                decision_ts < order.valid_in_backtest_from_timestamp) {
                continue;
            }
            if (order.valid_in_backtest_from_timestamp == 0) {
                const bool created_after =
                    create_cutoff_strict
                        ? order.create_timestamp > create_cutoff_ts
                        : order.create_timestamp >= create_cutoff_ts;
                if (created_after) {
                    continue;
                }
            }

            if (order.removed_in_backtest_at_timestamp > 0 &&
                decision_ts >= order.removed_in_backtest_at_timestamp) {
                continue;
            }

            if (order.expires_on_ms > 0 && decision_ts >= order.expires_on_ms) {
                continue;
            }

            std::optional<ResolvedFill> rf = decide(order);
            if (rf.has_value()) {
                pending_fills.push_back({order, rf->price, rf->is_taker});
            }
        }

        // Apply after iteration — safe to mutate resting_books_ now. The
        // queue-notional erase is a no-op for paths that never populate it
        // (crypto/equities), and clears a filled polymarket order's queue
        // accumulator so a recycled id starts fresh.
        for (auto& fill : pending_fills) {
            this->order_queue_notional_accumulated.erase(fill.order.id);
            this->on_order_fill(fill.order, fill.price, decision_ts,
                                fill.is_taker);
        }
    }

    [[gnu::always_inline]] inline void check_order_fills(
        const MarketEvent& market_event, const FillCheckBar& bar) {
        if (!market_event.symbol.has_value() ||
            !market_event.exchange_name.has_value() ||
            !market_event.market_type.has_value()) {
            return;
        }

        const std::string& key = market_event.exchange_name.value() + ":" +
                                 market_event.market_type.value() + ":" +
                                 market_event.symbol.value();

        const auto book_it = this->resting_books_.find(key);
        if (book_it == this->resting_books_.end()) {
            return;
        }

        const bool use_trade_price = this->sim_params.use_trade_price_for_fills;
        this->apply_resting_fills(
            market_event, book_it->second, bar.low_price, bar.high_price,
            bar.event_time, bar.kline_open_time, /*create_cutoff_strict=*/true,
            [&](const Order& order) -> std::optional<ResolvedFill> {
                // A market order on a bar feed takes the first bar it is
                // live on at that bar's open.
                if (order.is_taker_in_simulation &&
                    !order.is_prediction_market_order) {
                    return ResolvedFill{bar.open_price, true};
                }
                const double limit_price = order.price.toDouble();
                if (order.side == Core::OrderSide::BUY) {
                    if (bar.low_price < limit_price) {
                        return ResolvedFill{
                            use_trade_price ? bar.close_price : order.price,
                            false};
                    }
                } else if (order.side == Core::OrderSide::SELL) {
                    if (bar.high_price > limit_price) {
                        return ResolvedFill{
                            use_trade_price ? bar.close_price : order.price,
                            false};
                    }
                }
                return std::nullopt;
            });
    }

    [[gnu::always_inline]] inline void run_fill_check_for_window(
        const FillCheckWindow& window) {
        MarketEvent meta;
        meta.symbol = window.symbol;
        meta.exchange_name = window.exchange_name;
        meta.market_type = window.market_type;
        meta.event_time = window.window_last_event_time;
        FillCheckBar bar{
            .open_price = window.window_open,
            .high_price = window.window_high,
            .low_price = window.window_low,
            .close_price = window.window_close,
            .kline_open_time = window.window_first_open_time,
            .event_time = window.window_last_event_time,
        };
        this->check_order_fills(meta, bar);
    }

    [[gnu::always_inline]] inline void accumulate_ohlcv_for_fill_check(
        const MarketEvent& market_event) {
        if (!market_event.symbol.has_value() ||
            !market_event.bar_flush_info.has_value()) {
            return;
        }

        const auto& tick = market_event.bar_flush_info->tick;
        const std::string& key = market_event.exchange_name.value() + ":" +
                                 market_event.market_type.value() + ":" +
                                 market_event.symbol.value();
        const int aggregation_ticks =
            this->sim_params.fill_bars_aggregation_ticks;

        if (aggregation_ticks <= 1) {
            FillCheckBar bar{
                .open_price = tick.open_price,
                .high_price = tick.high_price,
                .low_price = tick.low_price,
                .close_price = tick.close_price,
                .kline_open_time = tick.kline_open_time,
                .event_time = market_event.event_time,
            };
            this->check_order_fills(market_event, bar);
            return;
        }

        auto& window = this->fill_check_windows_[key];
        if (window.bars_buffered == 0) {
            window.window_open = tick.open_price;
            window.window_high = tick.high_price;
            window.window_low = tick.low_price;
            window.window_first_open_time = tick.kline_open_time;
            window.exchange_name = market_event.exchange_name.value();
            window.market_type = market_event.market_type.value();
            window.symbol = market_event.symbol.value();
        } else {
            window.window_high = std::max(window.window_high, tick.high_price);
            window.window_low = std::min(window.window_low, tick.low_price);
        }
        window.window_close = tick.close_price;
        window.window_last_event_time = market_event.event_time;
        window.bars_buffered++;

        if (window.bars_buffered >= aggregation_ticks) {
            this->run_fill_check_for_window(window);
            window = FillCheckWindow{};
        }
    }

    void flush_pending_fill_checks() {
        for (auto& [key, window] : this->fill_check_windows_) {
            (void)key;
            if (window.bars_buffered > 0) {
                this->run_fill_check_for_window(window);
                window = FillCheckWindow{};
            }
        }
    }

    [[gnu::always_inline]] inline void check_order_fills_from_trade(
        const MarketEvent& market_event) {
        if (!market_event.symbol.has_value() ||
            !market_event.polymarket_trade.has_value()) {
            return;
        }

        const std::string& key = market_event.exchange_name.value() + ":" +
                                 market_event.market_type.value() + ":" +
                                 market_event.symbol.value();

        const auto book_it = this->resting_books_.find(key);
        if (book_it == this->resting_books_.end()) {
            return;
        }

        const auto& trade = market_event.polymarket_trade.value();
        const double trade_price = trade.price;
        const bool use_trade_price = this->sim_params.use_trade_price_for_fills;
        // For a single trade the price move is a point: skip_low == skip_high.
        this->apply_resting_fills(
            market_event, book_it->second, trade_price, trade_price,
            market_event.event_time, market_event.event_time,
            /*create_cutoff_strict=*/false,
            [&](const Order& order) -> std::optional<ResolvedFill> {
                if (order.is_prediction_market_order &&
                    !order.clob_id.empty() &&
                    order.clob_id != trade.clob_token_id) {
                    return std::nullopt;
                }
                if (order.is_taker_in_simulation) {
                    return ResolvedFill{trade.price, true};
                }
                bool price_matches = false;
                if (order.side == Core::OrderSide::BUY) {
                    price_matches =
                        trade.side == "SELL" && trade.price < order.price;
                } else if (order.side == Core::OrderSide::SELL) {
                    price_matches =
                        trade.side == "BUY" && trade.price > order.price;
                }
                if (!price_matches) {
                    return std::nullopt;
                }
                const double queue_threshold =
                    this->sim_params.polymarket_queue_notional;
                if (queue_threshold > 0) {
                    this->order_queue_notional_accumulated[order.id] +=
                        trade.size * trade.price;
                    if (this->order_queue_notional_accumulated[order.id] <
                        queue_threshold) {
                        return std::nullopt;
                    }
                }
                return ResolvedFill{use_trade_price ? trade.price : order.price,
                                    false};
            });
    }

    [[gnu::always_inline]] inline void process_ohlcv_update(
        const MarketEvent& market_event) {
        ::MarketData::DatasetsManager::get_instance().add_ohlcv_tick(
            market_event.bar_flush_info.value());
        this->last_market_event_was_ohlcv_update = true;

        // Always update engine price from bar close (needed by strategies
        // that read engine.get_price() in backtest mode)
        ::Core::Engine::getInstance().update_price(
            market_event.symbol.value(), market_event.exchange_name.value(),
            market_event.market_type.value(),
            market_event.bar_flush_info->tick.close_price);

        // Bar SMs are NOT fed from here. Each strategy that uses a bar SM is
        // responsible for replaying bars from its own dataset_store into the
        // SM (see e.g. AlgorithmPanicClarity::compute_predictions). SMs make
        // their react_impl idempotent w.r.t. already-processed timestamps so
        // re-feeds and multi-strategy sharing are safe. This keeps the engine
        // out of the SM data-path entirely.
        //
        // Note: a previous version dispatched a synthetic PRICE_UPDATE
        // MarketEvent to Engine::process_market_event when autofilling.
        // That path eventually called Engine::update_price with the same
        // close_price already passed above — a redundant mutex round-trip
        // per OHLCV bar. Dropped: the direct update_price call above is the
        // single source of truth for the engine's price state.
    }

    [[gnu::always_inline]] inline void process_funding_payment(
        MarketEvent& market_event) {
        static int g_fund_dbg = 0;
        if (g_fund_dbg < 40) {
            ++g_fund_dbg;
            std::string accts;
            for (auto a : this->account_ids) accts += std::to_string(a) + ",";
            std::string invkeys;
            int shown = 0;
            for (const auto& kv : this->backtest_inventories) {
                if (shown++ < 6) invkeys += kv.first + " ";
            }
            std::cout << "[FUND-CALL] sym=" << market_event.symbol.value()
                      << " accts=[" << accts << "] n_inv="
                      << this->backtest_inventories.size()
                      << " invkeys{" << invkeys << "}" << std::endl;
        }
        for (const auto& account_id : this->account_ids) {
            const auto& key =
                ::Core::Utils::get_symbol_exchange_market_account_key(
                    market_event.symbol.value(),
                    market_event.exchange_name.value(),
                    market_event.market_type.value(), account_id);

            const auto& inventory = this->backtest_inventories.find(key);
            if (inventory == this->backtest_inventories.end()) {
                continue;
            }

            const auto& price = ::Core::Engine::getInstance().get_price(
                market_event.symbol.value(), market_event.exchange_name.value(),
                market_event.market_type.value());

            const auto& funding_amount =
                std::abs(market_event.funding_rate_paid.value() *
                         (inventory->second.total.toDouble() * price.value()));

            // [DIAG] stream funding accrual so we can see it working in backtest.
            std::cout << "[FUND-DBG] " << market_event.symbol.value()
                      << " acct=" << account_id
                      << " pos=" << inventory->second.total.toDouble()
                      << " px=" << price.value() << " notional="
                      << (inventory->second.total.toDouble() * price.value())
                      << " rate_hr=" << market_event.funding_rate_paid.value()
                      << " fund_amt=" << funding_amount << " "
                      << (inventory->second.total.toDouble() > 0 ? "LONG" : "SHORT")
                      << (market_event.funding_rate_paid.value() > 0 ? " rate+"
                                                                     : " rate-")
                      << std::endl;

            if (inventory->second.total.toDouble() > 0) {
                if (market_event.funding_rate_paid.value() > 0) {
                    const mpfr::mpreal funding_amount_usdt =
                        funding_amount * -1;
                    this->update_quote_asset(market_event.exchange_name.value(),
                                             market_event.market_type.value(),
                                             account_id, "USDT",
                                             funding_amount_usdt);
                    market_event.notional_funding_payment = funding_amount_usdt;
                } else {
                    this->update_quote_asset(market_event.exchange_name.value(),
                                             market_event.market_type.value(),
                                             account_id, "USDT",
                                             funding_amount);
                    market_event.notional_funding_payment = funding_amount;
                }
            } else {
                if (market_event.funding_rate_paid.value() > 0) {
                    this->update_quote_asset(market_event.exchange_name.value(),
                                             market_event.market_type.value(),
                                             account_id, "USDT",
                                             funding_amount);
                    market_event.notional_funding_payment = funding_amount;
                } else {
                    const mpfr::mpreal funding_amount_usdt =
                        funding_amount * -1;
                    this->update_quote_asset(market_event.exchange_name.value(),
                                             market_event.market_type.value(),
                                             account_id, "USDT",
                                             funding_amount_usdt);
                    market_event.notional_funding_payment = funding_amount_usdt;
                }
            }
            // The quote-asset credit above already moves NAV, but the separate
            // global funding counter (read by the dashboard panel + engine
            // snapshot's funding_collected) was never incremented in backtest —
            // leaving it stuck at 0. Reflect the accrued payment here.
            if (market_event.notional_funding_payment.has_value()) {
                ::Core::Engine::getInstance().add_funding_payment(market_event);
            }
        }
    }

    [[gnu::always_inline]] inline void process_market_event(
        MarketEvent& market_event,
        bool is_last_market_event_on_interval = false) {
        if (market_event.event_time < this->simulation_start_timestamp_ms ||
            market_event.event_time > this->simulation_end_timestamp_ms) {
            return;
        }

        ++this->hotpath_market_events_processed_;

        this->hotpath_timed(BacktestHotpathKind::ScheduledRemovals, [&] {
            this->process_scheduled_book_removals(market_event.event_time);
        });

        this->hotpath_timed(BacktestHotpathKind::EngineProcessMarketEvent, [&] {
            ::Core::Engine::getInstance().process_market_event(market_event);
        });

        // Hourly reconciliation: rebuild risk_engine state from ticker truth.
        // Bounds any aggregate-state drift bug to one hour worst-case.
        constexpr long long RECONCILE_INTERVAL_MS = 3600LL * 1000;
        const long long bucket =
            market_event.event_time / RECONCILE_INTERVAL_MS;
        if (bucket != this->last_reconcile_bucket) {
            this->last_reconcile_bucket = bucket;
            auto& engine = ::Core::Engine::getInstance();
            this->hotpath_timed(BacktestHotpathKind::Reconcile, [&] {
                engine.reconcile_risk_engine_with_tickers();
                engine.run_periodic_strategy_cleanup();
            });
        }

        if (MarketEventType::ORDER_BOOK_UPDATE == market_event.type) {
        } else if (MarketEventType::POLYMARKET_TRADE == market_event.type) {
            if (!this->is_sim_autofilling_orders()) {
                this->check_order_fills_from_trade(market_event);
            }
            // Update engine-level polymarket event last trade price
            if (market_event.polymarket_trade.has_value()) {
                const auto& pm_trade = market_event.polymarket_trade.value();
                ::Core::Engine::getInstance()
                    .update_polymarket_event_last_trade(
                        pm_trade.condition_id, pm_trade.outcome_index,
                        pm_trade.price, pm_trade.timestamp);

                // Route trade to SM dispatcher for backtest replay
                if (::MarketData::StateMachineDispatcher::get_instance()
                        .is_running()) {
                    ::MarketData::MarketEvent sm_event;
                    sm_event.type = ::MarketData::MarketEvent::TRADE;
                    sm_event.trade = ::MarketData::TradeEvent{
                        market_event.symbol.value_or(""),
                        market_event.exchange_name.value_or("polymarket"),
                        market_event.market_type.value_or("prediction"),
                        pm_trade.price,
                        pm_trade.size,
                        (pm_trade.side == "SELL"),
                        pm_trade.timestamp};
                    ::MarketData::StateMachineDispatcher::get_instance()
                        .enqueue(std::move(sm_event));
                }
            }
        } else if (MarketEventType::POLYMARKET_MARKET_CREATED ==
                   market_event.type) {
            this->handle_polymarket_market_created(market_event);
        } else if (MarketEventType::POLYMARKET_MARKET_CLOSED ==
                   market_event.type) {
            this->handle_polymarket_market_closed(market_event);
        } else if (MarketEventType::PREDICTION_UPDATE == market_event.type) {
            if (market_event.backtest_prediction.has_value()) {
                this->pending_predictions[market_event.backtest_prediction
                                              ->model_name] =
                    market_event.backtest_prediction.value();
            }
            this->apply_pending_predictions();
            this->hotpath_timed(BacktestHotpathKind::PollStrategyStates, [&] {
                ::Core::Engine::getInstance().poll_strategy_states();
            });
        }

        if (market_event.type == MarketEventType::FUNDING_PAYMENT) {
            this->process_funding_payment(market_event);
        }

        if (market_event.type == MarketEventType::OHLCV_UPDATE) {
            this->process_ohlcv_update(market_event);
            if (!this->is_sim_autofilling_orders()) {
                this->hotpath_timed(BacktestHotpathKind::FillCheck, [&] {
                    this->accumulate_ohlcv_for_fill_check(market_event);
                });
            }
            if (is_last_market_event_on_interval) {
                this->hotpath_timed(
                    BacktestHotpathKind::GeneratePredictions, [&] {
                        ::Core::Engine::getInstance().generate_predictions();
                    });
                this->hotpath_timed(
                    BacktestHotpathKind::PollStrategyStates, [&] {
                        ::Core::Engine::getInstance().poll_strategy_states();
                    });
            }
            this->last_market_event_was_ohlcv_update = true;

        } else {
            if (!this->sim_params.run_strategy_on_bar_arrival &&
                this->sim_params.market_data.predictions.empty() &&
                this->last_market_event_was_ohlcv_update) {
                this->hotpath_timed(
                    BacktestHotpathKind::GeneratePredictions, [&] {
                        ::Core::Engine::getInstance().generate_predictions();
                    });
                this->hotpath_timed(
                    BacktestHotpathKind::PollStrategyStates, [&] {
                        ::Core::Engine::getInstance().poll_strategy_states();
                    });
            }
            this->last_market_event_was_ohlcv_update = false;
        }

        if (market_event.event_time -
                this->last_strategy_state_polling_timestamp >
            this->latency_between_strategy_state_polling_ms) {
            this->last_strategy_state_polling_timestamp =
                market_event.event_time;
            ::Core::Engine::getInstance().update_exposures_notional();

            if (market_event.event_time -
                    this->last_engine_state_snapshot_timestamp >
                this->interval_between_engine_state_snapshots_ms) {
                this->last_engine_state_snapshot_timestamp =
                    market_event.event_time;
                ::Core::Engine::getInstance().emit_engine_state_snapshot();

                this->print_debug_state(market_event.event_time);
            }
        }

        if (market_event.event_time - this->last_rm_unused_klines_timestamp >
            this->interval_between_rm_unused_klines_ms) {
            this->last_rm_unused_klines_timestamp = market_event.event_time;
            std::cout << "Freeing up memory on unused klines" << std::endl;
            for (auto& dataset : this->ohlcv_datasets) {
                const std::string& ohlcv_identifier =
                    ::MarketData::get_derived_bars_base_dir(
                        dataset.symbol, dataset.exchange_name,
                        dataset.market_type,
                        dataset.ohlcv_bar_meta.bar_close_policy, true);

                ::MarketData::DatasetsManager::get_instance().rm_unused_klines(
                    ohlcv_identifier,
                    this->sim_params.num_required_klines_on_datasets);
            }
        }

        this->last_market_event_time_ms = market_event.event_time;
    }

   private:
    struct CompactOhlcvBar {
        long long open_time_ms = 0;
        long long close_time_ms = 0;
        double open = 0.0;
        double high = 0.0;
        double low = 0.0;
        double close = 0.0;
        double volume = 0.0;
        double quote_asset_volume = 0.0;
        long number_of_trades = 0;
        double taker_buy_base_volume = 0.0;
        double taker_buy_quote_volume = 0.0;
        bool is_closed = false;
        bool is_read_from_slow_storage = false;
        bool is_read_from_fast_storage = false;
        long long last_trade_timestamp_ms = 0;
    };

    BacktestState();
    ~BacktestState();

    void bounds_on_add(SymbolRestingBook& book, const Order& order);
    void bounds_on_remove(SymbolRestingBook& book, const Order& removed);
    void recompute_side_bounds(SymbolRestingBook& book, Core::OrderSide side);
    void enqueue_scheduled_book_removal(long long effective_time_ms,
                                        const std::string& exchange_name,
                                        const std::string& market_type,
                                        const std::string& symbol,
                                        unsigned long long order_id);
    void cancel_scheduled_book_removal(unsigned long long order_id);
    [[nodiscard]] bool is_active_scheduled_book_removal(
        const ScheduledBookRemoval& entry) const;
    void process_scheduled_book_removals(long long event_time);
    void print_hotpath_profile(long long event_time, bool final_report = false);
    void hotpath_record_ns(BacktestHotpathKind kind, uint64_t ns);
    [[nodiscard]] bool ensure_paced_market_event_available();
    void observe_paced_ohlcv(const MarketEvent& event,
                             const PacedOhlcvObserver& observer) const;
    void initialize_streaming_ohlcv_cache();
    void read_streaming_ohlcv_klines();

    template <typename Fn>
    void hotpath_timed(BacktestHotpathKind kind, Fn&& fn) {
        const auto t0 = std::chrono::steady_clock::now();
        fn();
        const auto elapsed_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - t0)
                .count();
        this->hotpath_record_ns(kind, static_cast<uint64_t>(elapsed_ns));
    }

    static BacktestState* instance;
    std::vector<long long> account_ids;
    std::unordered_map<std::string, Inventory> backtest_inventories;
    std::unordered_map<std::string, QuoteAsset> backtest_quote_assets;
    std::unordered_map<std::string, mpfr::mpreal>
        fees_coeff_by_exchange_and_market;
    std::unordered_map<std::string, SymbolRestingBook> resting_books_;
    bool exchange_latency_enabled_ = false;
    std::unordered_map<unsigned long long, ScheduledBookRemoval>
        active_book_removals_;
    std::priority_queue<ScheduledBookRemoval, std::vector<ScheduledBookRemoval>,
                        ScheduledBookRemovalCompare>
        scheduled_book_removals_;
    std::unordered_map<unsigned long long, Order> deferred_cancel_acks_;
    std::unordered_map<std::string, FillCheckWindow> fill_check_windows_;
    // condition_id -> (outcome_index -> clob_token_id)
    std::unordered_map<std::string, std::vector<std::string>>
        condition_id_to_clob_ids;
    std::vector<EngineStateSnapshot> engine_state_snapshots;
    std::vector<Trade> trades;
    long long last_market_event_time_ms = 0;
    long long last_reconcile_bucket = -1;
    long long latency_between_strategy_state_polling_ms = 1000;
    long long latency_between_checking_order_fills_ms = 0;
    long long interval_between_engine_state_snapshots_ms = 60000 * 60 * 2;
    long long interval_between_rm_unused_klines_ms = 60000 * 60 * 24 * 7;
    long long last_strategy_state_polling_timestamp = 0;
    long long last_checking_order_fills_timestamp = 0;
    long long last_engine_state_snapshot_timestamp = 0;
    long long last_rm_unused_klines_timestamp = 0;
    long long simulation_start_timestamp_ms = 0;
    long long simulation_end_timestamp_ms = 0;

    long long read_market_events_chunk_start_timestamp_ms = 0;
    long long read_market_events_chunk_end_timestamp_ms = 0;
    long long read_market_events_chunk_size = 864000000ll * 2ll;
    long long price_data_end_timestamp_ms = 0;

    long long filled_exit_orders = 0;
    long long sent_exit_orders = 0;

    long long filled_entry_orders = 0;
    long long sent_entry_orders = 0;

    mpfr::mpreal total_fees = 0;
    mpfr::mpreal total_maker_rebates = 0;
    std::unordered_map<std::string, double> maker_rebate_rates;
    mpfr::mpreal total_pnl = 0;
    mpfr::mpreal total_volume = 0;
    mpfr::mpreal total_trades = 0;
    mpfr::mpreal fees_coeff = 0;

    mpfr::mpreal peak_balance = 0;
    mpfr::mpreal max_drawdown = 0;
    long long max_drawdown_ts_ms = 0;
    // Snapshot-frequency exposure peaks (long/NAV, short/NAV, gross/NAV)
    // with the timestamp at which each peak was reached. Updated in
    // update_engine_state_snapshot.
    double peak_long_exposure_pct = 0.0;
    long long peak_long_exposure_ts_ms = 0;
    double peak_short_exposure_pct = 0.0;
    long long peak_short_exposure_ts_ms = 0;
    double peak_gross_exposure_pct = 0.0;
    long long peak_gross_exposure_ts_ms = 0;
    // Daily NAV / exposure samples for end-of-run Sharpe + risk overlays.
    // Appended in update_engine_state_snapshot when ≥ 24h has elapsed since
    // the last sample.
    struct DailyStateSample {
        long long ts_ms;
        double nav;
        double long_exposure;
        double short_exposure;
    };
    std::vector<DailyStateSample> daily_state_samples;
    long long last_daily_state_sample_ms = 0;
    static constexpr long long DAILY_STATE_SAMPLE_INTERVAL_MS = 3600000;
    // When set (via the sim config's output_dir → main.cpp), the curve
    // is flushed to disk every N appended samples so a killed long-running
    // backtest still leaves a viewable curve. Atomic via write-tmp + rename.
    std::string equity_curve_dump_path;
    static constexpr int EQUITY_CURVE_FLUSH_EVERY_N_SAMPLES = 1;
    // Every fill (organic / mint / settlement) appended as JSONL to
    // <results_dir>/fills.jsonl — default on whenever equity_curve_dump_path
    // is set. The per-fill record is what makes the backtest diagnosable
    // (resolved-PnL, markout, per-condition attribution). Lazily opened in
    // on_order_fill.
    std::ofstream fill_record_stream_;
    long long winning_trades = 0;
    long long losing_trades = 0;
    mpfr::mpreal total_winning_pnl = 0;
    mpfr::mpreal total_losing_pnl = 0;
    SimulationInput sim_params;
    std::vector<BacktestOhlcvDataset> ohlcv_datasets;
    std::vector<BacktestFundingRateDataset> funding_rate_datasets;
    std::vector<std::vector<CompactOhlcvBar>> streaming_ohlcv_cache_;
    bool streaming_ohlcv_cache_initialized_ = false;
    long long streaming_ohlcv_cache_end_ms_ = 0;
    std::atomic<std::size_t> streaming_datasets_loaded_{0};
    std::atomic<std::size_t> streaming_datasets_total_{0};
    std::vector<Core::MarketEvent> market_events;
    std::size_t paced_market_event_index_ = 0;
    long long paced_market_events_processed_ = 0;
    bool paced_backtest_prepared_ = false;
    bool paced_backtest_finished_ = false;

    bool last_market_event_was_ohlcv_update = false;
    int ohlcv_sampling_counter_ = 0;
    bool is_autofilling_orders = false;
    std::unordered_map<std::string, BacktestPredictionEvent>
        pending_predictions;
    std::unordered_map<unsigned long long, double>
        order_queue_notional_accumulated;

    std::array<BacktestHotpathBucket,
               static_cast<size_t>(BacktestHotpathKind::Count)>
        hotpath_{};
    std::array<BacktestHotpathBucket,
               static_cast<size_t>(BacktestHotpathKind::Count)>
        hotpath_at_last_report_{};
    uint64_t hotpath_market_events_processed_ = 0;
    uint64_t hotpath_market_events_at_last_report_ = 0;
    uint64_t hotpath_fill_bounds_skips_ = 0;
    uint64_t hotpath_fill_bounds_skips_at_last_report_ = 0;
    long long hotpath_last_report_event_time_ms_ = 0;
};

}  // namespace Core
