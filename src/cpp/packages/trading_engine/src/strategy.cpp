#include "market_making/strategy.h"
#include "market_making/discretionary_accounts.h"

#include <optional>
#include <shared_mutex>

#include "common_types/log_event_types.h"
#include "datetime_utils.h"
#include "market_data/dataset_manager.h"
#include "market_data/ohlcv_dataset.h"
#include "market_data/state_machine_dispatcher.h"
#include "market_making/constants.h"
#include "market_making/engine.h"
#include "market_making/ticker.h"
#include "market_making/utils.h"
#include "market_making/strategy_registry.h"
#include "market_making/strategy_commands.h"

Core::Strategy::Strategy(const StrategyConfig &strategy_config)
    : strategy_config(strategy_config),
      is_disabled(strategy_config.is_disabled),
      is_in_reduce_only(strategy_config.is_in_reduce_only),
      is_trading_on_prediction_market(
          strategy_config.is_trading_on_prediction_market),
      is_market_making_strategy(strategy_config.is_market_making_strategy),
      account_mode(strategy_config.account_mode) {
    std::cerr << "[STRATEGY-CTOR] id=" << this->strategy_config.id << " name='"
              << this->strategy_config.strategy_name
              << "' is_disabled=" << this->strategy_config.is_disabled << "\n";
    if (const auto binder =
            StrategyRegistry::find(this->strategy_config.strategy_name)) {
        binder(*this);
    } else if (!this->strategy_config.is_disabled) {
        throw std::runtime_error("Strategy not found: " +
                                 this->strategy_config.strategy_name);
    }

    for (const auto &exchange_and_market :
         this->strategy_config.exchanges_list) {
        this->exchange_market_map[Utils::get_exchange_market_key(
            exchange_and_market.exchange_name,
            exchange_and_market.market_type)] = exchange_and_market;
    }
    this->virtual_balance.store(this->strategy_config.virtual_balance);
    this->is_using_hedges = this->strategy_config.uses_hedges;
    this->allow_co_strategy_compounding =
        this->strategy_config.allow_co_strategy_compounding;
    this->needs_to_run_realtime = this->strategy_config.needs_to_run_realtime;
    this->owns_event_state_machine =
        registers_own_state_machine(this->strategy_config.strategy_name);
}

bool Core::Strategy::registers_own_state_machine(
    const std::string &strategy_name) {
    return StrategyRegistry::owns_state_machine(strategy_name);
}

void Core::Strategy::adopt_runtime_state(
    Core::Strategy &old, const std::vector<std::string> &shared_symbols) {
    for (const auto &symbol : shared_symbols) {
        const auto it = old.trade_tickers.find(symbol);
        if (it != old.trade_tickers.end()) {
            this->add_trade_ticker_lockless(symbol, it->second);
        }
    }
    this->active_bundle = std::atomic_load(&old.active_bundle);
    this->dataset_pointer_to_last_kline_close_time_ms =
        old.dataset_pointer_to_last_kline_close_time_ms;
    this->ticker_dataset_to_last_kline_close_time_ms =
        old.ticker_dataset_to_last_kline_close_time_ms;
    this->last_reacted_to_market_state_by_polling =
        old.last_reacted_to_market_state_by_polling;
    this->last_received_update.store(old.last_received_update.load());
    this->last_generated_strategy_decisions.store(
        old.last_generated_strategy_decisions.load());
    {
        std::lock_guard<std::mutex> lock(old.strategy_mutex);
        this->navs_by_account_id = old.navs_by_account_id;
    }
    {
        std::lock_guard<std::mutex> lock(old.recent_orders_mutex);
        this->recent_enter_order_times = old.recent_enter_order_times;
        this->recent_exit_order_times = old.recent_exit_order_times;
    }
    this->adopt_moving_state(old);
}

void Core::Strategy::adopt_moving_state(const Core::Strategy &old) {
    this->virtual_balance.store(old.virtual_balance.load());
    // A carry-over, not a pause transition, so no STRATEGY_PAUSED event.
    this->stop_making_markets.store(old.stop_making_markets.load());
}

Core::Strategy::~Strategy() {
    if (this->strategy_event_state_machine && this->owns_event_state_machine) {
        ::MarketData::StateMachineDispatcher::get_instance().unregister_machine(
            this->strategy_event_state_machine);
    }
}

std::optional<std::shared_lock<std::shared_mutex>>
Core::Strategy::lock_on_pred_market_strat() const {
    if (!this->is_trading_on_prediction_market) {
        return std::nullopt;
    }
    return std::shared_lock<std::shared_mutex>(this->trade_tickers_mutex);
}

void Core::Strategy::update_dataset_store() {
    std::lock_guard<std::mutex> lock(this->strategy_mutex);
    if (std::chrono::steady_clock::now() -
            this->last_dataset_store_update_time <
        std::chrono::seconds(60)) {
        return;
    }

    this->last_dataset_store_update_time = std::chrono::steady_clock::now();
    auto dataset_store =
        ::MarketData::DatasetsManager::get_instance().copy_store();
    this->dataset_store = dataset_store;
}

void Core::Strategy::mark_received_update() {
    this->last_received_update = std::chrono::steady_clock::now();
}

std::chrono::steady_clock::time_point Core::Strategy::get_last_received_update()
    const {
    return this->last_received_update.load();
}

std::chrono::seconds Core::Strategy::get_seconds_since_last_received_update()
    const {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - this->last_received_update.load());
}

void Core::Strategy::on_create_order(Engine &engine, Order &order) {
    auto tickers_lock = this->lock_on_pred_market_strat();
    const auto ticker_it = this->trade_tickers.find(order.symbol);
    if (ticker_it == this->trade_tickers.end()) {
        throw std::runtime_error(
            "on_create_order: Ticker not found: " + order.symbol +
            " Strategy ID: " + std::to_string(this->strategy_config.id));
    }
    ticker_it->second->on_create_order(engine, order);
}

void Core::Strategy::on_cancel_order(Core::Engine& engine, Core::Order& order) {
    auto tickers_lock = this->lock_on_pred_market_strat();
    const auto ticker = this->trade_tickers.find(order.symbol);
    if (ticker == this->trade_tickers.end()) {
        throw std::runtime_error("on_cancel_order: Ticker not found");
    }
    ticker->second->on_cancel_order(engine, order);
}

void Core::Strategy::prune_old_order_times(
    std::deque<std::chrono::steady_clock::time_point> &times,
    std::chrono::seconds max_age) const {
    const auto cutoff = std::chrono::steady_clock::now() - max_age;
    while (!times.empty() && times.front() < cutoff) {
        times.pop_front();
    }
}

void Core::Strategy::record_enter_order_sent() {
    this->period_orders_placed.fetch_add(1, std::memory_order_relaxed);
    this->last_order_ts_ms.store(DatetimeUtils::get_current_time_ms());
    std::lock_guard<std::mutex> lock(this->recent_orders_mutex);
    this->recent_enter_order_times.push_back(std::chrono::steady_clock::now());
}

void Core::Strategy::record_exit_order_sent() {
    this->period_orders_placed.fetch_add(1, std::memory_order_relaxed);
    this->last_order_ts_ms.store(DatetimeUtils::get_current_time_ms());
    std::lock_guard<std::mutex> lock(this->recent_orders_mutex);
    this->recent_exit_order_times.push_back(std::chrono::steady_clock::now());
}

void Core::Strategy::prune_recent_order_times() {
    std::lock_guard<std::mutex> lock(this->recent_orders_mutex);
    const auto max_age = std::chrono::seconds(60);
    this->prune_old_order_times(this->recent_enter_order_times, max_age);
    this->prune_old_order_times(this->recent_exit_order_times, max_age);
}

int Core::Strategy::get_recent_enter_orders_count(
    std::chrono::seconds window) const {
    std::lock_guard<std::mutex> lock(this->recent_orders_mutex);
    const auto cutoff = std::chrono::steady_clock::now() - window;
    int count = 0;
    for (auto it = this->recent_enter_order_times.rbegin();
         it != this->recent_enter_order_times.rend(); ++it) {
        if (*it >= cutoff) {
            ++count;
        } else {
            break;
        }
    }
    return count;
}

int Core::Strategy::get_recent_exit_orders_count(
    std::chrono::seconds window) const {
    std::lock_guard<std::mutex> lock(this->recent_orders_mutex);
    const auto cutoff = std::chrono::steady_clock::now() - window;
    int count = 0;
    for (auto it = this->recent_exit_order_times.rbegin();
         it != this->recent_exit_order_times.rend(); ++it) {
        if (*it >= cutoff) {
            ++count;
        } else {
            break;
        }
    }
    return count;
}

int Core::Strategy::get_recent_enter_orders_count_10s() const {
    return this->get_recent_enter_orders_count(std::chrono::seconds(10));
}

int Core::Strategy::get_recent_exit_orders_count_10s() const {
    return this->get_recent_exit_orders_count(std::chrono::seconds(10));
}

std::optional<long long>
Core::Strategy::get_exchange_and_market_type_account_id(
    const std::string &exchange_name, const std::string &market_type) {
    const std::string &key =
        Utils::get_exchange_market_key(exchange_name, market_type);
    const auto it = this->exchange_market_map.find(key);
    if (it == this->exchange_market_map.end()) {
        return std::nullopt;
    }
    return it->second.account_id;
}

double Core::Strategy::get_strategy_trade_size(Engine &engine,
                                               const std::string &exchange_name,
                                               const std::string &market_type) {
    const auto account_id = this->get_exchange_and_market_type_account_id(
        exchange_name, market_type);
    if (!account_id.has_value()) {
        return 0;
    }
    const auto nav_account = this->get_nav_by_account_id(account_id.value());
    const auto strategy_virtual_balance = this->get_virtual_balance();

    if (nav_account == 0) {
        return 0;
    }

    double quote_sizing_notional = nav_account.toDouble();

    if (!this->allow_co_strategy_compounding &&
        strategy_virtual_balance < quote_sizing_notional) {
        quote_sizing_notional = strategy_virtual_balance;
    }

    if (this->strategy_config.is_using_percentage_max_position) {
        quote_sizing_notional *= this->strategy_config.percentage_max_position;
    }
    if (this->strategy_config.is_using_nominal_max_position) {
        quote_sizing_notional = std::min(
            quote_sizing_notional, this->strategy_config.nominal_max_position);
    }
    return quote_sizing_notional;
}

double Core::Strategy::get_virtual_balance() {
    return this->virtual_balance.load();
}

void Core::Strategy::update_virtual_balance(const Core::Trade &trade) {
    double pnl = trade.pnl.toDouble();
    if (this->strategy_config.discretionary_multi_account && this->strategy_config.is_paper_trading && !Core::Engine::getInstance().is_backtest) {
        Core::DiscretionaryAccounts::instance().settle(trade.id, trade.account_id, pnl, trade.signal_id);
    }
    this->virtual_balance.fetch_add(pnl);
}

void Core::Strategy::add_trade_ticker(
    const std::string &symbol, std::shared_ptr<TradeTicker> trade_ticker) {
    std::unique_lock<std::shared_mutex> lock(this->trade_tickers_mutex);
    this->add_trade_ticker_lockless(symbol, std::move(trade_ticker));
}

void Core::Strategy::add_trade_ticker_lockless(
    const std::string &symbol, std::shared_ptr<TradeTicker> trade_ticker) {
    this->trade_tickers[symbol] = std::move(trade_ticker);
    this->trade_tickers_symbols_set.insert(symbol);
    this->trade_tickers_symbols.clear();
    for (const auto &ticker : this->trade_tickers) {
        this->trade_tickers_symbols.push_back(ticker.first);
    }
}

void Core::Strategy::remove_trade_ticker(const std::string &symbol) {
    std::unique_lock<std::shared_mutex> lock(this->trade_tickers_mutex);
    this->remove_trade_ticker_lockless(symbol);
}

void Core::Strategy::remove_trade_ticker_lockless(const std::string &symbol) {
    this->trade_tickers.erase(symbol);
    this->trade_tickers_symbols_set.erase(symbol);
    this->trade_tickers_symbols.clear();
    for (const auto &ticker : this->trade_tickers) {
        this->trade_tickers_symbols.push_back(ticker.first);
    }
}

void Core::Strategy::set_stop_making_markets(bool should_stop,
                                             const char *reason,
                                             nlohmann::json extra_metadata) {
    bool expected = !should_stop;
    if (!this->stop_making_markets.compare_exchange_strong(expected,
                                                           should_stop)) {
        return;
    }

    auto &engine = Core::Engine::getInstance();
    if (!engine.isInitialized()) return;

    nlohmann::json metadata = extra_metadata.is_object()
                                  ? std::move(extra_metadata)
                                  : nlohmann::json::object();
    metadata["reason"] = reason ? reason : "UNKNOWN";
    metadata["strategy_id"] = this->strategy_config.id;
    metadata["strategy_name"] = this->strategy_config.strategy_name;
    metadata["symbols"] = this->trade_tickers_symbols;
    metadata["transitioned_at_ms"] =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();

    const char *event_type =
        should_stop ? Core::TRADING_LOG_EVENT_TYPE_STRATEGY_PAUSED
                    : Core::TRADING_LOG_EVENT_TYPE_STRATEGY_RESUMED;
    std::string level =
        should_stop ? Core::LOG_LEVEL_WARNING : Core::LOG_LEVEL_INFO;
    const char *message = should_stop ? "strategy paused" : "strategy resumed";

    engine.create_system_log("", level, message, event_type, metadata,
                             this->strategy_config.id);
}

void Core::Strategy::process_strategy_decisions(
    Engine &engine, const std::vector<StrategyDecision> &strategy_decisions) {
    if (strategy_decisions.size() == 0) {
        return;
    }

    if (engine.is_backtest) {
        this->post_decisions_backtest(engine, strategy_decisions);
        return;
    }

    if (!this->is_trading_on_prediction_market) {
        this->process_crypto_and_stock_market_decisions(engine,
                                                        strategy_decisions);
    } else {
        this->process_prediction_market_decisions(engine, strategy_decisions);
    }
}

static const int POLYMARKET_CANCEL_ORDER_BATCH_SIZE = 100;
static const int POLYMARKET_CREATE_ORDER_BATCH_SIZE = 15;

void Core::Strategy::process_prediction_market_decisions(
    Engine &engine, const std::vector<StrategyDecision> &strategy_decisions) {
    std::vector<Order> all_orders;
    std::vector<Order> all_cancelled_orders;

    for (const auto &strategy_decision : strategy_decisions) {
        all_orders.insert(all_orders.end(), strategy_decision.orders.begin(),
                          strategy_decision.orders.end());
        all_cancelled_orders.insert(all_cancelled_orders.end(),
                                    strategy_decision.cancelled_orders.begin(),
                                    strategy_decision.cancelled_orders.end());
    }

    if (!all_cancelled_orders.empty()) {
        std::unordered_map<std::string, std::vector<Order *>> cancels_by_symbol;
        for (auto &order : all_cancelled_orders) {
            cancels_by_symbol[order.symbol].push_back(&order);
        }
        for (auto &[symbol, cancel_ptrs] : cancels_by_symbol) {
            auto ticker_it = this->trade_tickers.find(symbol);
            if (ticker_it == this->trade_tickers.end()) continue;
            auto ticker = ticker_it->second;

            for (size_t i = 0; i < cancel_ptrs.size();
                 i += POLYMARKET_CANCEL_ORDER_BATCH_SIZE) {
                size_t end =
                    std::min(i + (size_t)POLYMARKET_CANCEL_ORDER_BATCH_SIZE,
                             cancel_ptrs.size());

                auto owned_orders = std::make_shared<std::vector<Order>>();
                owned_orders->reserve(end - i);
                for (size_t j = i; j < end; j++) {
                    owned_orders->push_back(*cancel_ptrs[j]);
                }

                engine.enqueue_cancel_order_task(
                    [ticker, owned_orders, &engine]() {
                        std::vector<Order *> ptrs;
                        ptrs.reserve(owned_orders->size());
                        for (auto &order : *owned_orders) {
                            ptrs.push_back(&order);
                        }
                        ticker->on_cancel_orders_bulk(engine, ptrs);
                    });
            }
        }
    }

    if (!all_orders.empty()) {
        std::unordered_map<std::string, std::vector<Order *>> orders_by_symbol;
        for (auto &order : all_orders) {
            orders_by_symbol[order.symbol].push_back(&order);
        }

        for (auto &[symbol, order_ptrs] : orders_by_symbol) {
            auto ticker_it = this->trade_tickers.find(symbol);
            if (ticker_it == this->trade_tickers.end()) continue;
            auto ticker = ticker_it->second;

            for (size_t i = 0; i < order_ptrs.size();
                 i += POLYMARKET_CREATE_ORDER_BATCH_SIZE) {
                size_t end =
                    std::min(i + (size_t)POLYMARKET_CREATE_ORDER_BATCH_SIZE,
                             order_ptrs.size());

                auto owned_orders = std::make_shared<std::vector<Order>>();
                owned_orders->reserve(end - i);
                for (size_t j = i; j < end; j++) {
                    owned_orders->push_back(*order_ptrs[j]);
                }

                engine.enqueue_order_task([ticker, owned_orders, &engine]() {
                    std::vector<Order *> ptrs;
                    ptrs.reserve(owned_orders->size());
                    for (auto &order : *owned_orders) {
                        ptrs.push_back(&order);
                    }
                    ticker->on_create_orders_bulk(engine, ptrs);
                });
            }
        }
    }
}

void Core::Strategy::post_decisions_backtest(
    Engine &engine, const std::vector<StrategyDecision> &strategy_decisions) {
    for (const auto &strategy_decision : strategy_decisions) {
        for (auto order : strategy_decision.orders) {
            this->on_create_order(engine, order);
        }
        for (auto order : strategy_decision.cancelled_orders) {
            this->on_cancel_order(engine, order);
        }
    }
}

void Core::Strategy::process_crypto_and_stock_market_decisions(
    Engine &engine, const std::vector<StrategyDecision> &strategy_decisions) {
    for (const auto &strategy_decision : strategy_decisions) {
        for (auto order : strategy_decision.orders) {
            engine.enqueue_order_task([this, &engine, order]() mutable {
                this->on_create_order(engine, order);
            });
        }
        for (auto order : strategy_decision.cancelled_orders) {
            engine.enqueue_cancel_order_task([this, &engine, order]() mutable {
                this->on_cancel_order(engine, order);
            });
        }
    }
}

void Core::Strategy::react_using_polling(Engine &engine) {
    try {
        if (this->is_disabled) {
            return;
        }
        auto tickers_lock = this->lock_on_pred_market_strat();
        const auto &strategy_decisions =
            this->strategy_callback(engine, *this, std::nullopt);
        this->process_strategy_decisions(engine, strategy_decisions);
        this->last_generated_strategy_decisions.store(
            std::chrono::steady_clock::now());
        this->mark_strategy_decisions_generation_end();
    } catch (const std::exception &e) {
        std::string strategy_name = this->strategy_config.strategy_name;
        std::string error_message = e.what();
        std::cout << "Error in react_using_polling: " << error_message
                  << " for strategy: " << strategy_name << std::endl;
        abort();
    }
}

std::vector<Core::Trade> Core::Strategy::get_trades() {
    auto tickers_lock = this->lock_on_pred_market_strat();
    std::vector<Core::Trade> trades;
    for (const auto &trade_ticker : this->trade_tickers) {
        auto trades_from_ticker = trade_ticker.second->get_trades();
        trades.insert(trades.end(), trades_from_ticker.begin(),
                      trades_from_ticker.end());
    }
    return trades;
}

std::vector<Core::Order> Core::Strategy::get_orders() {
    auto tickers_lock = this->lock_on_pred_market_strat();
    std::vector<Core::Order> orders;
    for (const auto &trade_ticker : this->trade_tickers) {
        const auto orders_from_ticker = trade_ticker.second->get_orders();
        orders.insert(orders.end(), orders_from_ticker.begin(),
                      orders_from_ticker.end());
    }
    return orders;
}

Core::Strategy::RecoveredIds Core::Strategy::recover_state_from_disk(
    Engine &engine, const std::vector<Trade> &trades,
    const std::vector<Order> &orders) {
    auto tickers_lock = this->lock_on_pred_market_strat();
    RecoveredIds recovered;
    for (const auto &trade : trades) {
        if (trade.strategy_id != this->strategy_config.id) {
            continue;
        }
        for (const auto &ticker : this->trade_tickers) {
            const bool directional_match =
                trade.type_internal ==
                    Core::TradeTypeInternal::DIRECTIONAL_TRADE &&
                ticker.second->symbol == trade.symbol;
            const bool hedge_match =
                trade.type_internal == Core::TradeTypeInternal::HEDGE &&
                trade.hedge_for_symbol.has_value() &&
                ticker.second->symbol == trade.hedge_for_symbol.value();
            if (directional_match || hedge_match) {
                ticker.second->recover_trade_from_disk(engine, trade);
                recovered.trades.insert(trade.id);
            }
        }
        if (!recovered.trades.contains(trade.id)) {
            engine.create_system_log(
                trade.symbol, LOG_LEVEL_ERROR,
                "recover_state_from_disk: no matching ticker for trade " +
                    std::to_string(trade.id) + " on " + trade.symbol +
                    " (strategy " + std::to_string(trade.strategy_id) +
                    "); kept in the state file",
                TRADING_LOG_EVENT_TYPE_TRADING_STATE_RECOVER_FAILED,
                trade.to_json(), trade.strategy_id, trade.exchange_name,
                trade.market_type);
        }
    }
    for (const auto &order : orders) {
        if (order.strategy_id != this->strategy_config.id) {
            continue;
        }
        for (const auto &ticker : this->trade_tickers) {
            if (ticker.second->symbol == order.symbol) {
                ticker.second->recover_order_from_disk(engine, order);
                recovered.orders.insert(order.id);
            }
        }
        if (!recovered.orders.contains(order.id)) {
            engine.create_system_log(
                order.symbol, LOG_LEVEL_ERROR,
                "recover_state_from_disk: no matching ticker for order " +
                    std::to_string(order.id) + " on " + order.symbol +
                    " (strategy " + std::to_string(order.strategy_id) +
                    "); kept in the state file",
                TRADING_LOG_EVENT_TYPE_TRADING_STATE_RECOVER_FAILED,
                order.to_json(), order.strategy_id, order.exchange_name,
                order.market_type);
        }
    }
    return recovered;
}

std::optional<::MarketData::Store> Core::Strategy::get_dataset_store() {
    std::lock_guard<std::mutex> lock(this->strategy_mutex);
    return this->dataset_store;
}

void Core::Strategy::generate_predictions(Engine &engine) {
    try {
        this->mark_prediction_start();
        auto tickers_lock = this->lock_on_pred_market_strat();
        this->compute_predictions_callback(engine, *this);
        this->mark_prediction_end();
    } catch (const std::exception &e) {
        std::string strategy_name = this->strategy_config.strategy_name;
        std::string error_message = e.what();
        std::cerr << "FATAL: generate_predictions failed for " << strategy_name
                  << ": " << error_message << std::endl;
        abort();
    }
}

bool Core::Strategy::is_ready_to_run_new_prediction(
    const std::vector<std::shared_ptr<::MarketData::OhlcvDataset>>
        &dependant_datasets) {
    for (const auto &dataset : dependant_datasets) {
        const auto latest_kline_close_time_ms =
            dataset->get_latest_bar_close_time_ms();

        if (!latest_kline_close_time_ms.has_value()) {
            return false;
        }

        if (this->dataset_pointer_to_last_kline_close_time_ms.find(dataset) ==
            this->dataset_pointer_to_last_kline_close_time_ms.end()) {
            return true;
        }
        if (this->dataset_pointer_to_last_kline_close_time_ms[dataset] !=
            latest_kline_close_time_ms.value()) {
            return true;
        }
    }
    return false;
}

void Core::Strategy::update_last_prediction_ts(
    const std::vector<std::shared_ptr<::MarketData::OhlcvDataset>>
        &dependant_datasets) {
    for (const auto &dataset : dependant_datasets) {
        const auto latest_kline_close_time_ms =
            dataset->get_latest_bar_close_time_ms();
        if (!latest_kline_close_time_ms.has_value()) {
            continue;
        }
        this->dataset_pointer_to_last_kline_close_time_ms[dataset] =
            latest_kline_close_time_ms.value();
    }
}

bool Core::Strategy::is_ready_to_run_new_prediction(
    const std::string &ticker_symbol,
    const std::vector<std::shared_ptr<::MarketData::OhlcvDataset>>
        &dependant_datasets) {
    auto &ticker_map =
        this->ticker_dataset_to_last_kline_close_time_ms[ticker_symbol];
    for (const auto &dataset : dependant_datasets) {
        const auto latest_kline_close_time_ms =
            dataset->get_latest_bar_close_time_ms();

        if (!latest_kline_close_time_ms.has_value()) {
            return false;
        }

        if (ticker_map.find(dataset) == ticker_map.end()) {
            return true;
        }
        if (ticker_map[dataset] != latest_kline_close_time_ms.value()) {
            return true;
        }
    }
    return false;
}

void Core::Strategy::update_last_prediction_ts(
    const std::string &ticker_symbol,
    const std::vector<std::shared_ptr<::MarketData::OhlcvDataset>>
        &dependant_datasets) {
    auto &ticker_map =
        this->ticker_dataset_to_last_kline_close_time_ms[ticker_symbol];
    for (const auto &dataset : dependant_datasets) {
        const auto latest_kline_close_time_ms =
            dataset->get_latest_bar_close_time_ms();
        if (!latest_kline_close_time_ms.has_value()) {
            continue;
        }
        ticker_map[dataset] = latest_kline_close_time_ms.value();
    }
}

bool Core::Strategy::can_start_new_prediction() const {
    return !this->is_prediction_thread_running.load();
}

void Core::Strategy::mark_prediction_start() {
    this->is_prediction_thread_running.store(true);
}

void Core::Strategy::mark_prediction_end() {
    this->last_prediction_ts_ms.store(DatetimeUtils::get_current_time_ms());
    this->is_prediction_thread_running.store(false);
}

std::chrono::steady_clock::time_point
Core::Strategy::get_last_generated_strategy_decisions() const {
    return this->last_generated_strategy_decisions.load();
}

std::chrono::seconds
Core::Strategy::get_seconds_since_last_generated_strategy_decisions() const {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() -
        this->last_generated_strategy_decisions.load());
}

bool Core::Strategy::can_generate_new_strategy_decisions(
    bool needs_to_run_realtime) const {
    if (!needs_to_run_realtime) {
        if (this->get_seconds_since_last_received_update() >
            std::chrono::seconds(15)) {
            if (this->get_seconds_since_last_generated_strategy_decisions() <
                std::chrono::seconds(2)) {
                return false;
            }
        }
    }
    return !this->is_generating_strategy_decisions.load();
}

void Core::Strategy::mark_strategy_decisions_generation_start() {
    this->is_generating_strategy_decisions.store(true);
}

void Core::Strategy::mark_strategy_decisions_generation_end() {
    this->is_generating_strategy_decisions.store(false);
}

void Core::Strategy::sync_persistent_state_to_disk() {
    auto tickers_lock = this->lock_on_pred_market_strat();
    for (const auto &ticker : this->trade_tickers) {
        ticker.second->sync_persistent_state_to_disk();
    }
}

void Core::Strategy::update_navs_by_account_id(
    const std::unordered_map<long long, mpfr::mpreal> &navs) {
    std::lock_guard<std::mutex> lock(this->strategy_mutex);
    for (const auto &nav : navs) {
        this->navs_by_account_id[nav.first] = nav.second;
    }
}

mpfr::mpreal Core::Strategy::get_nav_by_account_id(long long account_id) {
    std::lock_guard<std::mutex> lock(this->strategy_mutex);
    if (this->navs_by_account_id.find(account_id) !=
        this->navs_by_account_id.end()) {
        return this->navs_by_account_id[account_id];
    }
    return 0;
}

int Core::Strategy::get_num_active_trades_excluding_symbol(
    const std::string &symbol) {
    auto tickers_lock = this->lock_on_pred_market_strat();
    int num_active_trades = 0;
    for (const auto &ticker : this->trade_tickers) {
        if (ticker.second->symbol == symbol) {
            continue;
        }
        num_active_trades += ticker.second->get_num_active_trades();
    }
    return num_active_trades;
}

int Core::Strategy::get_num_active_enter_orders_excluding_symbol(
    const std::string &symbol) {
    auto tickers_lock = this->lock_on_pred_market_strat();
    int num_active_enter_orders = 0;
    for (const auto &ticker : this->trade_tickers) {
        if (ticker.second->symbol == symbol) {
            continue;
        }
        std::lock_guard<std::mutex> lock(ticker.second->trading_mutex);
        num_active_enter_orders += ticker.second->get_num_enter_orders();
    }
    return num_active_enter_orders;
}

std::vector<Core::MintContractsRequest> Core::Strategy::get_mint_contracts(
    Engine &engine) {
    if (this->mint_contracts_callback == nullptr) {
        return {};
    }
    auto tickers_lock = this->lock_on_pred_market_strat();
    return this->mint_contracts_callback(engine, *this);
}

void Core::Strategy::force_close_trade(Engine &engine, const Core::Trade &trade,
                                       ForceCloseReason reason,
                                       const nlohmann::json &context) {
    auto tickers_lock = this->lock_on_pred_market_strat();
    for (const auto &ticker : this->trade_tickers) {
        if (ticker.second->symbol == trade.symbol) {
            ticker.second->force_close_trade(engine, trade.id, reason, context);
        }
    }
}

void Core::Strategy::watch_state_machine(const std::string &name) {
    // Idempotent: if already watching, no-op. Supports strategies that call
    // watch_state_machine from a hot callback rather than an init hook.
    for (const auto &w : this->watched_state_machines) {
        if (w.sm && w.sm->get_name() == name) return;
    }
    auto sm =
        ::MarketData::StateMachineDispatcher::get_instance().get_machine(name);
    if (!sm) {
        // Disabled strategies never execute; their watched-SM list only feeds
        // the health snapshot and is meaningless when nothing runs. Refusing
        // to load a disabled strategy because its observability dependency is
        // missing turns a startup-time landmine on for any global SM kill.
        // Active strategies still fail-fast — that path stays load-bearing.
        if (this->strategy_config.is_disabled) {
            std::cout << "[watch_state_machine] disabled strategy "
                      << this->strategy_config.strategy_name
                      << " references missing SM '" << name
                      << "'; skipping (no functional impact)" << std::endl;
            return;
        }
        throw std::runtime_error("Strategy " +
                                 this->strategy_config.strategy_name +
                                 " watches unknown state machine: " + name);
    }
    this->watched_state_machines.push_back({std::move(sm)});
}
