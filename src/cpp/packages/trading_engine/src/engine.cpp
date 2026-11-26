#include "market_making/engine.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <map>
#include <shared_mutex>
#include <unordered_set>

#include "chrono_utils.h"
#include "common_types/constants.h"
#include "common_types/log_event_types.h"
#include "datetime_utils.h"
#include "digital_option_calibration_map.h"
#include "env_utils.h"
#include "linux_process.h"
#include "market_data/state_machine_dispatcher.h"
#include "market_data/utils.h"
#include "market_making/dtypes/order.h"
#include "market_making/dtypes/symbol_precision.h"
#include "market_making/fold_schedule_loader.h"
#include "market_making/ticker.h"
#include "market_making/utils.h"
#include "onnx_model.h"
#include "str_math.h"

Core::Engine& Core::Engine::getInstance() {
    static Engine instance;
    return instance;
}

namespace {

Core::EngineSignalOutcome apply_remove_trade_signal(
    Core::Engine& engine, const Core::EngineSignal& signal) {
    Core::ManuallyRemovedTrade removal;
    try {
        removal.db_id = signal.id;
        removal.symbol = signal.payload.at("symbol").get<std::string>();
        removal.strategy_id = signal.payload.at("strategy_id").get<int>();
        removal.trade_id =
            std::stoull(signal.payload.at("trade_id").get<std::string>());
    } catch (const std::exception& e) {
        return {Core::EngineSignalStatus::FAILED,
                std::string("bad payload: ") + e.what()};
    }
    if (engine.manually_remove_trade(removal)) {
        return {Core::EngineSignalStatus::APPLIED, ""};
    }
    // Every engine sees the removal; only the one holding the trade applies it.
    return {Core::EngineSignalStatus::SKIPPED, "trade not held by this engine"};
}

// Only strategies that make markets read the flag.
Core::EngineSignalOutcome apply_stop_making_markets_signal(
    Core::Engine& engine, const Core::EngineSignal& signal) {
    const int strategy_id = signal.payload.value("strategy_id", -1);
    const bool prediction_market_only =
        signal.payload.value("prediction_market_only", false);
    const char* reason = prediction_market_only
                             ? Core::StrategyPauseReason::POLYMARKET_SIGNAL_STOP
                             : Core::StrategyPauseReason::OPERATOR_REQUESTED;
    int stopped = 0;
    for (auto& strategy : engine.get_strategies()) {
        if (strategy_id >= 0 && strategy->strategy_config.id != strategy_id) {
            continue;
        }
        if (prediction_market_only &&
            !strategy->is_trading_on_prediction_market) {
            continue;
        }
        strategy->set_stop_making_markets(
            true, reason, nlohmann::json{{"signal_id", signal.id}});
        ++stopped;
    }
    if (stopped == 0) {
        return {Core::EngineSignalStatus::SKIPPED,
                "no matching strategy in this engine"};
    }
    return {Core::EngineSignalStatus::APPLIED, ""};
}

}  // namespace

void Core::Engine::initialize(
    const TradingLogCallback& trading_log_callback,
    const EngineStateSnapshotCallback& engine_state_snapshot_callback,
    const EngineConfig& config) {
    if (initialized_.load()) {
        throw std::runtime_error("Engine already initialized");
    }

    running.store(true);
    trading_log_callback_ = trading_log_callback;
    engine_state_snapshot_callback_ = engine_state_snapshot_callback;
    fees_by_exchange_and_market = config.fees_by_exchange_and_market;

    is_backtest = config.is_backtest;
    is_order_creation_disabled_ = config.is_order_creation_disabled;
    engine_instance_ = config.engine_instance;
    observability_callback_ = config.observability_callback;
    last_startup_ts_ms_ =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    if (!is_backtest) {
        order_thread_pool = std::make_unique<ThreadPool>(200);
        cancel_order_thread_pool = std::make_unique<ThreadPool>(100);
        strategy_thread_pool =
            std::make_unique<ThreadPool>(std::thread::hardware_concurrency());
        generate_predictions_thread_pool =
            std::make_unique<ThreadPool>(std::thread::hardware_concurrency());
        // Every slow write the periodic loop starts lands here, so it must
        // never round down to zero workers or queue one DB write behind another.
        periodic_tasks_thread_pool = std::make_unique<ThreadPool>(
            std::max(2u, std::thread::hardware_concurrency() / 4));

        // Periodic-loop spawns are individually gated by env vars that
        // the test harness sets when it wants deterministic control
        // via /control/engine/triggers/*. Production never sets these
        // — the getenv check fires exactly three times per process
        // startup, never again, so the cost to live is zero once past
        // init.
        if (!EnvUtils::env_is_one("DISABLE_RUN_PERIODIC_CALLS_LOOP")) {
            run_periodic_calls_thread =
                std::thread(&Engine::run_periodic_calls_loop, this);
        }
        if (!EnvUtils::env_is_one("DISABLE_GENERATE_PREDICTIONS_LOOP")) {
            generate_predictions_thread =
                std::thread(&Engine::generate_predictions_loop, this);
        }
        if (!EnvUtils::env_is_one("DISABLE_GENERATE_STRATEGY_DECISIONS_LOOP")) {
            generate_strategy_decisions_thread =
                std::thread(&Engine::generate_strategy_decisions_loop, this);
        }
    }

    num_market_events_processed = 0;
    global_long_exposure = 0;
    global_short_exposure = 0;
    global_nav = 0;
    emit_trading_logs = config.emit_trading_logs;
    is_strategy_development = config.is_strategy_development;
    num_backpressure_events_ = 0;
    num_dropped_trade_events_ = 0;
    num_dropped_orderbook_events_ = 0;

    last_update_exposures_notional_time =
        std::chrono::steady_clock::now() - std::chrono::seconds(60);
    last_poll_market_data_streams_time =
        std::chrono::steady_clock::now() - std::chrono::seconds(60);
    last_flush_trading_logs_time =
        std::chrono::steady_clock::now() - std::chrono::seconds(60);
    last_log_throughput_stats_time =
        std::chrono::steady_clock::now() - std::chrono::seconds(60);
    last_periodic_calls_time =
        std::chrono::steady_clock::now() - std::chrono::seconds(60);

    initialized_.store(true);

    if (config.emit_logs_in_real_time) {
        if (!config.publish_trading_log_callback) {
            throw std::runtime_error(
                "publish_trading_log_callback is required when "
                "emit_logs_in_real_time is true");
        }
        this->emit_logs_in_real_time = true;
        this->publish_trading_log_callback =
            config.publish_trading_log_callback.value();
    }

    if (config.emit_realtime_trading_state) {
        if (!config.publish_realtime_trading_state_callback) {
            throw std::runtime_error(
                "publish_realtime_trading_state_callback is required when "
                "emit_realtime_trading_state is true");
        }
        this->emit_realtime_trading_state = true;
        this->publish_realtime_trading_state_callback =
            config.publish_realtime_trading_state_callback.value();
        this->seconds_between_realtime_trading_state_updates =
            config.seconds_between_realtime_trading_state_updates;
        this->last_realtime_trading_state_update_time =
            std::chrono::steady_clock::now();
        if (this->seconds_between_realtime_trading_state_updates <= 0) {
            throw std::runtime_error(
                "seconds_between_realtime_trading_state_updates must be "
                "greater than 0");
        }
    }
    if (config.enable_engine_signals) {
        if (!config.fetch_engine_signals_callback ||
            !config.ack_engine_signal_callback) {
            throw std::runtime_error(
                "fetch_engine_signals_callback and ack_engine_signal_callback "
                "are required when enable_engine_signals is true");
        }
        this->enable_engine_signals = true;
        this->fetch_engine_signals_callback =
            config.fetch_engine_signals_callback.value();
        this->ack_engine_signal_callback =
            config.ack_engine_signal_callback.value();
    }
    this->register_engine_signal_handler(
        ENGINE_SIGNAL_REMOVE_TRADE, [this](const EngineSignal& signal) {
            return apply_remove_trade_signal(*this, signal);
        });
    this->register_engine_signal_handler(
        ENGINE_SIGNAL_STOP_MAKING_MARKETS, [this](const EngineSignal& signal) {
            return apply_stop_making_markets_signal(*this, signal);
        });
    if (config.enable_realtime_subscriptions) {
        if (!config.fetch_realtime_subscriptions_callback) {
            throw std::runtime_error(
                "fetch_realtime_subscriptions_callback is required when "
                "enable_realtime_subscriptions is true");
        }
        this->enable_realtime_subscriptions = true;
        this->fetch_realtime_subscriptions_callback =
            config.fetch_realtime_subscriptions_callback.value();
        this->seconds_between_subscription_polls =
            config.seconds_between_subscription_polls;
    }
    if (config.enable_strategy_telemetry) {
        if (!config.publish_strategy_telemetry_callback) {
            throw std::runtime_error(
                "publish_strategy_telemetry_callback is required when "
                "enable_strategy_telemetry is true");
        }
        this->enable_strategy_telemetry_ = true;
        this->publish_strategy_telemetry_callback_ =
            config.publish_strategy_telemetry_callback.value();
    }
    this->program_start_time_ms = DatetimeUtils::get_current_time_ms();
}

Core::Engine::~Engine() {
    if (!running.load()) {
        return;  // Already stopped
    }
    gracefully_stop();
}

void Core::Engine::reset() {
    // Test-harness hook: clears transient engine state so a subsequent
    // test starts from a clean slate without respawning the binary.
    // Declared at engine.h:275; gated behind the
    // /control/engine/reset endpoint (mutations-required).
    //
    // WHAT THIS CLEARS
    //   - Pending-emit queues (new_orders / new_trades /
    //     new_orders_cancels / new_order_fills) — the per-emit staging
    //     that emit_engine_state_snapshot() drains.
    //   - BBA + price caches and the raw orderbooks map.
    //   - Market event deque, markout queues.
    //   - Misc tracked orders, out-of-sync orderbook set, cached
    //     orderbook updates, active orderbook subscriptions.
    //   - Counters (market events processed, backpressure, dropped
    //     trade/orderbook events) — reset to 0.
    //   - Exposure atomics (long/short/margin_notional/funding).
    //
    // WHAT THIS LEAVES ALONE
    //   - Strategies vector + their TradeTickers. A Strategy-level
    //     reset would walk each ticker's orders/trades; out of scope
    //     for this Phase — tests that care either use per-test
    //     Scenario (fresh engine) or inject orders via the control
    //     plane after reset.
    //   - Loaded ONNX models, exchange_symbol_infos, fees, calibration
    //     maps — all init-derived config.
    //   - Risk engine, inventories, quote_assets — account state.
    //   - Observability / trading-log callbacks and threads.
    //   - is_state_ready — stays true so subsequent tests don't wait
    //     for state-ready again.

    {
        std::lock_guard<std::mutex> l(new_orders_mutex);
        new_orders.clear();
    }
    {
        std::lock_guard<std::mutex> l(new_trades_mutex);
        new_trades.clear();
    }
    {
        std::lock_guard<std::mutex> l(new_orders_cancels_mutex);
        new_orders_cancels.clear();
    }
    {
        std::lock_guard<std::mutex> l(new_order_fills_mutex);
        new_order_fills.clear();
    }
    {
        std::unique_lock<std::shared_mutex> l(bba_mutex_);
        bba_states_.clear();
    }
    {
        std::unique_lock<std::shared_mutex> l(price_mutex);
        prices.clear();
    }
    {
        std::lock_guard<std::mutex> l(engine_state_mutex);
        tracked_misc_orders.clear();
        order_books.clear();
        market_events.clear();
    }
    {
        std::lock_guard<std::mutex> l(order_books_out_of_sync_mutex);
        order_books_out_of_sync.clear();
    }
    {
        std::lock_guard<std::mutex> l(cached_orderbook_updates_mutex);
        cached_orderbook_updates.clear();
    }
    {
        std::lock_guard<std::mutex> l(markout_mutex_);
        pending_markouts_.clear();
    }
    {
        std::lock_guard<std::mutex> l(completed_markouts_mutex_);
        completed_markouts_.clear();
    }

    num_market_events_processed.store(0);
    num_backpressure_events_ = 0;
    num_dropped_trade_events_ = 0;
    num_dropped_orderbook_events_ = 0;

    global_long_exposure.store(0.0);
    global_short_exposure.store(0.0);
    global_margin_notional.store(0.0);
    global_funding_payments.store(0.0);

    active_orderbook_subscriptions.clear();
}

void Core::Engine::initialize_symbol_precision_access() {
    SymbolPrecisionAccess::getInstance().initialize(this);
}

void Core::Engine::add_strategy(std::unique_ptr<Core::Strategy> strategy) {
    Core::Strategy& added = *strategy;
    {
        std::unique_lock lock(this->strategies_mutex);
        this->strategies.push_back(std::move(strategy));
    }
    this->load_models();
    this->seed_active_bundle_from_schedule(added);
}

std::unique_lock<std::shared_mutex>
Core::Engine::try_lock_strategies_exclusive() {
    // The rwlock prefers readers, so a blocking lock could wait behind a
    // steady stream of short reads; the caller's loop must not stall.
    for (int attempt = 0; attempt < 200; ++attempt) {
        std::unique_lock lock(this->strategies_mutex, std::try_to_lock);
        if (lock.owns_lock()) return lock;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return {};
}

Core::Engine::SyncResult Core::Engine::add_strategy_while_trading(
    std::unique_ptr<Core::Strategy>& strategy) {
    if (!strategy->strategy_config.strategy_models.empty() ||
        !strategy->strategy_config.model_schedule_path.empty()) {
        return SyncResult::RESTART_REQUIRED;
    }
    auto lock = this->try_lock_strategies_exclusive();
    if (!lock.owns_lock()) return SyncResult::BUSY;
    this->restore_held_state(*strategy, false);
    this->strategies.push_back(std::move(strategy));
    return SyncResult::APPLIED;
}

bool Core::Engine::quiesce_strategy(Core::Strategy& strategy,
                                    std::chrono::milliseconds timeout) {
    const bool was_disabled = strategy.is_disabled_now();
    strategy.set_disabled(true);
    // A dispatch loop that read the flag before it flipped holds the shared
    // lock until it has marked its pass running, so once the exclusive lock
    // is acquired the in-flight flags are complete.
    bool settled = this->try_lock_strategies_exclusive().owns_lock();
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (settled && !strategy.is_idle()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            settled = false;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!settled) strategy.set_disabled(was_disabled);
    return settled;
}

Core::Engine::SyncResult Core::Engine::replace_strategy(
    std::unique_ptr<Core::Strategy>& next) {
    const int id = next->strategy_config.id;
    auto lock = this->try_lock_strategies_exclusive();
    if (!lock.owns_lock()) return SyncResult::BUSY;
    for (auto& slot : this->strategies) {
        if (slot->strategy_config.id != id) continue;
        next->adopt_moving_state(*slot);
        this->restore_held_state(*next, false);
        this->retired_strategies.push_back(std::move(slot));
        slot = std::move(next);
        return SyncResult::APPLIED;
    }
    return SyncResult::NOT_FOUND;
}

Core::Engine::SyncResult Core::Engine::remove_strategy(int strategy_id) {
    auto lock = this->try_lock_strategies_exclusive();
    if (!lock.owns_lock()) return SyncResult::BUSY;
    for (auto it = this->strategies.begin(); it != this->strategies.end();
         ++it) {
        if ((*it)->strategy_config.id != strategy_id) continue;
        this->retired_strategies.push_back(std::move(*it));
        this->strategies.erase(it);
        return SyncResult::APPLIED;
    }
    return SyncResult::NOT_FOUND;
}

void Core::Engine::seed_active_bundle_from_schedule(Core::Strategy& strategy) {
    // Seeds `strategy.active_bundle` from its fold_schedule.json at
    // registration time. In live, get_time() returns wall-clock — picks the
    // entry that would have been active by now. In backtest, get_time() is 0
    // (no events processed yet) — no entry matches and the backtest reader's
    // per-chunk warm-up event handles seeding instead. Single seeding path, two
    // modes.
    const std::string& strategy_name = strategy.strategy_config.strategy_name;
    const auto& schedule_path = strategy.strategy_config.model_schedule_path;
    const int strategy_id = strategy.strategy_config.id;
    if (schedule_path.empty()) {
        std::cout << "[ModelBundle] strategy=" << strategy_name
                  << " SKIPPED (no model_schedule_path configured)" << "\n";
        this->create_system_log("", Core::LOG_LEVEL_INFO,
                                "no model_schedule_path configured",
                                Core::TRADING_LOG_EVENT_TYPE_MODEL_BUNDLE_SEED,
                                nlohmann::json{{"strategy", strategy_name}},
                                strategy_id, "", "", false);
        return;
    }
    auto entries = Core::FoldScheduleLoader::load(schedule_path);
    if (entries.empty()) {
        std::cout << "[ModelBundle] strategy=" << strategy_name
                  << " LOAD FAILED or empty schedule: " << schedule_path
                  << "\n";
        this->create_system_log(
            "", Core::LOG_LEVEL_ERROR, "fold_schedule load failed or empty",
            Core::TRADING_LOG_EVENT_TYPE_MODEL_BUNDLE_SEED,
            nlohmann::json{{"strategy", strategy_name},
                           {"schedule_path", schedule_path}},
            strategy_id, "", "", false);
        return;
    }
    const long long now_ms = this->get_time();
    const Core::FoldScheduleLoader::LoadedEntry* active = nullptr;
    for (const auto& le : entries) {
        if (le.timestamp <= now_ms &&
            (active == nullptr || le.timestamp > active->timestamp)) {
            active = &le;
        }
    }
    if (active == nullptr) {
        // For live this is a real WARN (no fold rolled out yet). For backtest
        // it's expected — get_time() returns 0 pre-replay; the chunk reader
        // emits the warm-up event later.
        const char* stdout_tag =
            this->is_backtest ? "(backtest — chunk reader will seed)" : "WARN";
        std::cout << "[ModelBundle] strategy=" << strategy_name
                  << " no active fold at now_ms=" << now_ms << " " << stdout_tag
                  << " entries_parsed=" << entries.size()
                  << " path=" << schedule_path << "\n";
        const std::string& level =
            this->is_backtest ? Core::LOG_LEVEL_INFO : Core::LOG_LEVEL_WARNING;
        this->create_system_log(
            "", level, "no active fold at engine.get_time()",
            Core::TRADING_LOG_EVENT_TYPE_MODEL_BUNDLE_SEED,
            nlohmann::json{{"strategy", strategy_name},
                           {"schedule_path", schedule_path},
                           {"now_ms", now_ms},
                           {"entries_parsed", entries.size()},
                           {"is_backtest", this->is_backtest}},
            strategy_id, "", "", false);
        return;
    }
    Core::MarketEvent ev;
    ev.type = MarketEventType::MODEL_UPDATE;
    ev.event_time = now_ms;
    ev.model_update = active->event;
    std::cout << "[ModelBundle] strategy=" << strategy_name
              << " SEEDING from fold_ts=" << active->timestamp
              << " members=" << active->event.members.size()
              << " path=" << schedule_path << "\n";
    this->create_system_log(
        "", Core::LOG_LEVEL_INFO, "seeding active_bundle",
        Core::TRADING_LOG_EVENT_TYPE_MODEL_BUNDLE_SEED,
        nlohmann::json{{"strategy", strategy_name},
                       {"schedule_path", schedule_path},
                       {"fold_ts", active->timestamp},
                       {"members", active->event.members.size()},
                       {"now_ms", now_ms}},
        strategy_id, "", "", false);
    this->dispatch_model_update(ev);
}

std::optional<double> Core::Engine::get_price(const std::string& symbol,
                                              const std::string& exchange_name,
                                              const std::string& market_type) {
    const auto& key = Utils::get_symbol_exchange_market_key(
        symbol, exchange_name, market_type);
    std::shared_lock<std::shared_mutex> lock(this->price_mutex);
    auto it = this->prices.find(key);
    return it != this->prices.end()
               ? std::make_optional(it->second.load(std::memory_order_relaxed))
               : std::nullopt;
}

void Core::Engine::update_quote_asset(const std::string& exchange_name,
                                      const std::string& market_type,
                                      const std::string& asset,
                                      long long account_id,
                                      const Core::QuoteAsset& quote_asset) {
    std::unique_lock<std::shared_mutex> lock(this->inventory_mutex);
    const auto key = Utils::get_symbol_exchange_market_account_key(
        asset, exchange_name, market_type, account_id);
    this->quote_assets[key] = quote_asset;
}

void Core::Engine::update_inventory_and_quote_assets(
    const std::string& exchange_name, const std::string& market_type,
    long long account_id, const std::vector<Core::Inventory>& inventories,
    const std::vector<Core::QuoteAsset>& quote_assets) {
    std::unique_lock<std::shared_mutex> lock(this->inventory_mutex);

    // Binance USDS-M futures hedge mode emits one inventory row per side
    // (LONG / SHORT) for the same symbol. Merge per-key so the stored
    // Inventory carries both long_total + short_total. Net `total` is
    // recomputed as (long_total - short_total). Reset side totals on the
    // first row touched per key so a side that's gone to zero in this
    // poll (e.g. all shorts closed) doesn't leak a stale value from the
    // previous poll.
    // Every caller passes the account's full snapshot, and venues omit flat
    // symbols, so a symbol missing from it holds nothing.
    const std::string account_suffix = ":" + exchange_name + ":" +
                                       market_type + ":" +
                                       std::to_string(account_id);
    for (auto& [key, slot] : this->inventories) {
        if (key.size() > account_suffix.size() &&
            key.compare(key.size() - account_suffix.size(),
                        account_suffix.size(), account_suffix) == 0) {
            slot.long_total = mpfr::mpreal(0);
            slot.short_total = mpfr::mpreal(0);
            slot.total = mpfr::mpreal(0);
            slot.used_margin = std::nullopt;
            slot.maint_margin = std::nullopt;
        }
    }
    std::unordered_set<std::string> touched_keys;
    for (const auto& inventory : inventories) {
        const auto key = Utils::get_symbol_exchange_market_account_key(
            inventory.quote_conversion_symbol, exchange_name, market_type,
            account_id);
        if (touched_keys.insert(key).second) {
            auto reset = inventory;
            reset.long_total = mpfr::mpreal(0);
            reset.short_total = mpfr::mpreal(0);
            reset.total = mpfr::mpreal(0);
            reset.used_margin = std::nullopt;
            reset.maint_margin = std::nullopt;
            this->inventories[key] = reset;
        }
        auto& slot = this->inventories[key];
        slot.long_total += inventory.long_total;
        slot.short_total += inventory.short_total;
        slot.total = slot.long_total - slot.short_total;
        // Margin: Binance reports per-side margin independently; sum so
        // the aggregated entry reflects the combined obligation.
        if (inventory.used_margin.has_value()) {
            slot.used_margin = slot.used_margin.value_or(mpfr::mpreal(0)) +
                               inventory.used_margin.value();
        }
        if (inventory.maint_margin.has_value()) {
            slot.maint_margin = slot.maint_margin.value_or(mpfr::mpreal(0)) +
                                inventory.maint_margin.value();
        }
    }
    for (const auto& quote_asset : quote_assets) {
        const auto key = Utils::get_symbol_exchange_market_account_key(
            quote_asset.asset, exchange_name, market_type, account_id);
        this->quote_assets[key] = quote_asset;
    }
}

std::optional<Core::Inventory> Core::Engine::get_inventory(
    const std::string& exchange_name, const std::string& market_type,
    const std::string& symbol, long long account_id) {
    std::shared_lock<std::shared_mutex> lock(this->inventory_mutex);
    const auto key = Utils::get_symbol_exchange_market_account_key(
        symbol, exchange_name, market_type, account_id);
    auto it = this->inventories.find(key);
    if (it == this->inventories.end()) {
        return std::nullopt;
    }
    return it->second;
}

void Core::Engine::update_price(const std::string& symbol,
                                const std::string& exchange_name,
                                const std::string& market_type, double price) {
    const auto& key = Utils::get_symbol_exchange_market_key(
        symbol, exchange_name, market_type);

    {
        std::shared_lock<std::shared_mutex> lock(this->price_mutex);
        auto it = this->prices.find(key);
        if (it != this->prices.end()) {
            it->second.store(price, std::memory_order_release);
            return;
        }
    }

    std::unique_lock<std::shared_mutex> lock(this->price_mutex);
    auto& ref = this->prices[key];
    ref.store(price, std::memory_order_release);
}

std::optional<double> Core::Engine::get_borrowable_inventory(
    const std::string& asset, const std::string& exchange_name,
    const std::string& market_type, long long account_id) {
    const auto key = Utils::get_symbol_exchange_market_account_key(
        asset, exchange_name, market_type, account_id);

    uint64_t seq_before =
        this->borrowable_inventories_seq.load(std::memory_order_acquire);

    if (seq_before & 1) {
        return std::nullopt;
    }

    auto it = this->borrowable_inventories.find(key);
    std::optional<double> result;
    if (it != this->borrowable_inventories.end()) {
        result = it->second;
    }

    std::atomic_thread_fence(std::memory_order_acquire);

    uint64_t seq_after =
        this->borrowable_inventories_seq.load(std::memory_order_relaxed);

    if (seq_before != seq_after) {
        return std::nullopt;
    }

    return result;
}

void Core::Engine::update_borrowable_inventories(
    const std::unordered_map<std::string, double>& updates) {
    this->borrowable_inventories_seq.fetch_add(1, std::memory_order_release);

    std::atomic_thread_fence(std::memory_order_release);

    for (const auto& [key, value] : updates) {
        this->borrowable_inventories[key] = value;
    }

    this->borrowable_inventories_seq.fetch_add(1, std::memory_order_release);
}

void Core::Engine::update_price_market_event(
    const Core::MarketEvent& market_event) {
    const auto& price = market_event.price;

    if (!price.has_value()) {
        return;
    }

    const auto& exchange_name = market_event.exchange_name;
    const auto& symbol = market_event.symbol;
    const auto& market_type = market_event.market_type;

    if (!exchange_name.has_value() || !symbol.has_value() ||
        !market_type.has_value()) {
        return;
    }

    this->update_price(symbol.value(), exchange_name.value(),
                       market_type.value(), price.value());
}

void Core::Engine::trade_market_event(const Core::MarketEvent& market_event) {
    if (!market_event.price.has_value() ||
        !market_event.exchange_name.has_value() ||
        !market_event.symbol.has_value() ||
        !market_event.market_type.has_value()) {
        return;
    }
    const auto price = market_event.price.value();
    const auto exchange_name = market_event.exchange_name.value();
    const auto symbol = market_event.symbol.value();
    const auto market_type = market_event.market_type.value();
    this->update_price(symbol, exchange_name, market_type, price);
}

void Core::Engine::generate_predictions() {
    if (!this->is_state_ready.load()) {
        return;
    }

    if (this->is_backtest) {
        for (auto& strategy : this->strategies_view()) {
            if (strategy->is_disabled_now()) {
                continue;
            }
            strategy->generate_predictions(*this);
        }
        return;
    }

    for (auto& strategy : this->strategies_view()) {
        if (strategy->is_disabled_now()) {
            continue;
        }
        if (strategy->can_start_new_prediction()) {
            strategy->mark_prediction_start();
            this->generate_predictions_thread_pool->enqueue(
                [strategy = strategy.get(), this]() {
                    strategy->generate_predictions(*this);
                });
        }
    }
}

void Core::Engine::update_inventory(const std::string& exchange_name,
                                    const std::string& market_type,
                                    const std::string& symbol,
                                    long long account_id,
                                    const Core::Inventory& inventory) {
    std::unique_lock<std::shared_mutex> lock(this->inventory_mutex);
    const auto key = Utils::get_symbol_exchange_market_account_key(
        symbol, exchange_name, market_type, account_id);
    this->inventories[key] = inventory;
}

void Core::Engine::remove_inventories_by_symbol(
    const std::string& symbol, const std::string& exchange_name,
    const std::string& market_type) {
    std::unique_lock<std::shared_mutex> lock(this->inventory_mutex);
    for (auto it = this->inventories.begin(); it != this->inventories.end();) {
        if (it->second.asset == symbol &&
            it->second.exchange_name == exchange_name &&
            it->second.market_type == market_type) {
            it = this->inventories.erase(it);
        } else {
            ++it;
        }
    }
}

void Core::Engine::flush_trading_logs(bool force_flush) {
    if (!this->is_state_ready.load() && !force_flush) {
        return;
    }

    auto now = std::chrono::steady_clock::now();
    if (!force_flush &&
        now - this->last_flush_trading_logs_time < std::chrono::seconds(5)) {
        return;
    }
    this->last_flush_trading_logs_time = now;

    std::vector<TradingLog> logs_copy;
    logs_copy.reserve(1000);  // Pre-allocate space for efficiency

    // Try to dequeue all available logs
    TradingLog log;
    while (this->trading_logs.try_dequeue(log)) {
        logs_copy.push_back(std::move(log));
    }

    if (logs_copy.empty()) {
        return;
    }

    if (force_flush) {
        this->trading_log_callback_(logs_copy);
    } else {
        this->periodic_tasks_thread_pool->enqueue(
            [this, logs = std::move(logs_copy)]() {
                this->trading_log_callback_(logs);
            });
    }
}

void Core::Engine::emit_strategy_telemetry(StrategyTelemetryEvent&& event) {
    if (!enable_strategy_telemetry_) return;

    auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(strategy_telemetry_emit_mutex_);
        auto& last = strategy_telemetry_last_emit_[event.strategy_id];
        if (now - last < std::chrono::milliseconds(10)) {
            return;
        }
        last = now;
    }
    strategy_telemetry_queue_.enqueue(std::move(event));
}

void Core::Engine::drain_strategy_telemetry() {
    if (!enable_strategy_telemetry_) return;

    strategy_telemetry_drain_job_.try_dispatch(
        *periodic_tasks_thread_pool, [this]() {
            StrategyTelemetryEvent event;
            std::unordered_map<int, StrategyTelemetryEvent> latest;
            while (strategy_telemetry_queue_.try_dequeue(event)) {
                latest[event.strategy_id] = std::move(event);
            }
            if (latest.empty()) return;

            nlohmann::json j = nlohmann::json::array();
            for (auto& [id, e] : latest) {
                j.push_back(e.to_json());
            }
            publish_strategy_telemetry_callback_(j.dump());
            auto ptr = std::make_shared<
                std::unordered_map<int, StrategyTelemetryEvent>>(
                std::move(latest));
            {
                std::lock_guard<std::mutex> lock(
                    latest_strategy_telemetry_mutex_);
                latest_strategy_telemetry_ = std::move(ptr);
            }
        });
}

std::shared_ptr<std::unordered_map<int, Core::StrategyTelemetryEvent>>
Core::Engine::get_latest_strategy_telemetry() {
    std::lock_guard<std::mutex> lock(latest_strategy_telemetry_mutex_);
    return latest_strategy_telemetry_;
}

void Core::Engine::poll_strategy_states() {
    if (!this->is_state_ready.load()) {
        return;
    }

    if (this->is_backtest) {
        for (auto& strategy : this->strategies_view()) {
            if (strategy->is_disabled_now()) {
                continue;
            }
            strategy->react_using_polling(*this);
            if (this->is_backtest && strategy->is_using_hedges) {
                strategy->react_using_polling(*this);
            }
        }
        return;
    }

    for (auto& strategy : this->strategies_view()) {
        if (strategy->is_disabled_now()) {
            continue;
        }
        if (strategy->can_generate_new_strategy_decisions(
                strategy->needs_to_run_realtime)) {
            strategy->mark_strategy_decisions_generation_start();
            this->strategy_thread_pool->enqueue(
                [strategy = strategy.get(), this]() {
                    strategy->react_using_polling(*this);
                });
        }
    }
}

void Core::Engine::run_periodic_calls() {
    if (!this->is_state_ready.load() || !this->initialized_.load()) {
        return;
    }

    auto now = std::chrono::steady_clock::now();
    auto since_last = std::chrono::duration_cast<std::chrono::microseconds>(
                          now - this->last_periodic_calls_time.load())
                          .count();
    if (since_last < 25) {
        return;
    }

    this->last_periodic_calls_time.store(now);
    this->update_exposures_notional();
    this->risk_engine.update_exposures_on_open_trades();
    if (!this->is_backtest) {
        this->emit_engine_state_snapshot(false);
        this->emit_observability_tick(false);
    }
    this->flush_trading_logs();
    this->log_throughput_stats();
    this->check_trades_for_less_than_min_notional();
    this->prune_ticker_order_times();
    this->run_periodic_strategy_cleanup();
    this->recalculate_ticker_order_book_state();
    this->process_engine_signals();
    this->poll_realtime_subscriptions();
    if (!this->is_backtest &&
        now - this->last_sweep_pulled_quotes_time.load() >=
            std::chrono::milliseconds(500)) {
        this->last_sweep_pulled_quotes_time.store(now);
        this->sweep_pulled_quotes();
    }
    if (!this->is_backtest) {
        this->process_markouts();
        this->drain_strategy_telemetry();
    }

    if (this->emit_realtime_trading_state) {
        auto now = std::chrono::steady_clock::now();
        if (now - this->last_realtime_trading_state_update_time.load() >
            std::chrono::seconds(
                this->seconds_between_realtime_trading_state_updates)) {
            this->last_realtime_trading_state_update_time.store(now);
            this->periodic_tasks_thread_pool->enqueue([this]() {
                this->publish_realtime_trading_state_callback(*this);
            });
        }
    }
}

void Core::Engine::add_funding_payment(const Core::MarketEvent& market_event) {
    std::lock_guard<std::mutex> lock(this->engine_state_mutex);
    if (market_event.notional_funding_payment.has_value()) {
        this->global_funding_payments +=
            market_event.notional_funding_payment.value().toDouble();
    }
}

void Core::Engine::process_market_event(const Core::MarketEvent& market_event) {
    if (market_event.type == MarketEventType::ORDER_BOOK_UPDATE) {
    } else if (market_event.type == MarketEventType::AGGR_TRADE) {
        this->trade_market_event(market_event);
    } else if (market_event.type == MarketEventType::SIZING_COEFF_UPDATE) {
    } else if (market_event.type == MarketEventType::PREDICTION_UPDATE) {
    } else if (market_event.type == MarketEventType::PRICE_UPDATE) {
        this->update_price_market_event(market_event);
    } else if (market_event.type == MarketEventType::BULK_PRICE_UPDATE) {
    } else if (market_event.type == MarketEventType::CUSTOM) {
    } else if (market_event.type == MarketEventType::FUNDING_PAYMENT) {
        this->add_funding_payment(market_event);
    } else if (market_event.type == MarketEventType::POLYMARKET_TRADE) {
        this->trade_market_event(market_event);
    } else if (market_event.type == MarketEventType::MODEL_UPDATE) {
        this->dispatch_model_update(market_event);
    }
    this->latest_market_event_time_ms.store(market_event.event_time);
    this->num_market_events_processed.store(
        this->num_market_events_processed.load() + 1);
}

void Core::Engine::dispatch_model_update(
    const Core::MarketEvent& market_event) {
    if (!market_event.model_update.has_value()) {
        return;
    }
    const auto& ev = market_event.model_update.value();
    if (ev.schedule_path.empty()) {
        return;
    }
    // Single bundle shared across all matching strategies — copy-on-write via
    // shared_ptr; engine never owns ONNX state.
    auto bundle = std::make_shared<Core::ModelBundle>();
    bundle->members = ev.members;
    bundle->metadata = ev.metadata;
    int matched = 0;
    std::vector<std::string> matched_names;
    for (auto& strategy : this->strategies_view()) {
        if (strategy->strategy_config.model_schedule_path ==
            ev.schedule_path) {
            std::atomic_store(&strategy->active_bundle, bundle);
            ++matched;
            matched_names.push_back(strategy->strategy_config.strategy_name);
        }
    }
    nlohmann::json md{
        {"schedule_path", ev.schedule_path},
        {"event_time_ms", market_event.event_time},
        {"members", ev.members.size()},
        {"strategies_matched", matched},
        {"strategy_names", matched_names},
    };
    if (matched == 0) {
        std::cout << "[ModelBundle] DISPATCH WARN: 0 strategies matched "
                  << ev.schedule_path
                  << " (event_time=" << market_event.event_time << ")" << "\n";
        this->create_system_log(
            "", Core::LOG_LEVEL_WARNING, "MODEL_UPDATE matched 0 strategies",
            Core::TRADING_LOG_EVENT_TYPE_MODEL_BUNDLE_UPDATE, md, 0, "", "",
            false);
    } else {
        std::cout << "[ModelBundle] DISPATCH ok: matched=" << matched
                  << " members=" << ev.members.size()
                  << " ts=" << market_event.event_time
                  << " path=" << ev.schedule_path << "\n";
        this->create_system_log(
            "", Core::LOG_LEVEL_INFO, "MODEL_UPDATE dispatched",
            Core::TRADING_LOG_EVENT_TYPE_MODEL_BUNDLE_UPDATE, md, 0, "", "",
            false);
    }
}

std::unordered_map<std::string, Core::SymbolExchangeInfo>
Core::Engine::get_exchange_symbol_infos() const {
    std::shared_lock<std::shared_mutex> lock(this->exchange_symbol_infos_mutex);
    return this->exchange_symbol_infos;
}

std::optional<int> Core::Engine::get_minutes_until_market_close() const {
    auto time = this->get_time();
    auto time_ms_until_market_close =
        MarketData::get_time_ms_until_market_close(time);
    if (!time_ms_until_market_close.has_value()) {
        return 0;
    }
    return std::round(time_ms_until_market_close.value() /
                      ((long long)60000.0));
}

long long Core::Engine::get_time() const {
    if (this->is_backtest) {
        return this->latest_market_event_time_ms.load();
    } else {
        return DatetimeUtils::get_current_time_ms();
    }
}

void Core::Engine::run_periodic_calls_loop() {
    while (running.load()) {
        this->run_periodic_calls();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void Core::Engine::generate_predictions_loop() {
    while (running.load()) {
        this->generate_predictions();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void Core::Engine::generate_strategy_decisions_loop() {
    while (running.load()) {
        this->poll_strategy_states();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void Core::Engine::log_throughput_stats() {
    auto now = std::chrono::steady_clock::now();
    auto duration = now - this->last_log_throughput_stats_time.load();
    auto seconds =
        std::chrono::duration_cast<std::chrono::seconds>(duration).count();

    if (seconds >= 60 * 5) {  // Use direct seconds comparison
        double messages_per_second =
            static_cast<double>(this->num_market_events_processed) / seconds;

        nlohmann::json metadata;
        metadata["messages_per_second"] = messages_per_second;
        metadata["queue_size"] = this->market_events.size();
        metadata["total_messages"] = this->num_market_events_processed.load();
        metadata["seconds_since_last_log"] = seconds;

        this->create_system_log(
            "", LOG_LEVEL_INFO,
            "Throughput: " + std::to_string(messages_per_second) + " msgs/sec",
            TRADING_LOG_EVENT_TYPE_SYSTEM_PERFORMANCE, metadata, -1, "", "");

        this->last_log_throughput_stats_time = now;
        this->num_market_events_processed.store(0);
    }
}

void Core::Engine::gracefully_stop() {
    this->running.store(false);

    nlohmann::json metadata;
    metadata["total_market_events"] = this->num_market_events_processed.load();
    metadata["reason"] = "gracefully_stop_called";
    this->create_system_log("", LOG_LEVEL_INFO, "Engine shutdown (graceful)",
                            TRADING_LOG_EVENT_TYPE_ENGINE_SHUTDOWN_GRACEFUL,
                            metadata, -1, "", "");

    // Final observability tick with is_running=false + shutdown reason so
    // engine_health reflects a clean exit rather than a stale heartbeat.
    if (this->observability_callback_.has_value() &&
        this->is_state_ready.load() && !this->is_backtest) {
        this->emit_observability_tick(/*is_shutdown=*/true);
    }

    // Force one last flush so the shutdown log + observability rows reach
    // the DB before the periodic thread is joined.
    this->flush_trading_logs(/*force_flush=*/true);
}

void Core::Engine::order_created_ack(Core::Order& order) {
    // Record order_sent_to_ack latency if we have a valid send timestamp.
    if (order.create_timestamp > 0) {
        const int64_t ack_ts_ms = DatetimeUtils::get_current_time_ms();
        const int64_t elapsed_ms = ack_ts_ms - order.create_timestamp;
        if (elapsed_ms >= 0) {
            this->latency_order_sent_to_ack_.record(elapsed_ms * 1'000'000LL);
        }
    }

    for (const auto& strategy : this->strategies_view()) {
        if (strategy->strategy_config.id != order.strategy_id) {
            continue;
        }

        strategy->mark_received_update();

        auto tickers_lock = strategy->lock_on_pred_market_strat();
        for (auto& ticker : strategy->trade_tickers) {
            if (ticker.first == order.symbol) {
                ticker.second->ack_create_order(*this, order);
                break;
            }
        }
    }
}

void Core::Engine::order_filled_ack(Core::OrderFillInfo& order_fill_info) {
    // Record fill_to_ack latency if we have a valid fill timestamp.
    if (order_fill_info.order_fill_timestamp > 0) {
        const int64_t ack_ts_ms = DatetimeUtils::get_current_time_ms();
        const int64_t elapsed_ms =
            ack_ts_ms - order_fill_info.order_fill_timestamp;
        if (elapsed_ms >= 0) {
            this->latency_fill_to_ack_.record(elapsed_ms * 1'000'000LL);
        }
    }
    for (const auto& strategy : this->strategies_view()) {
        if (strategy->strategy_config.id != order_fill_info.strategy_id) {
            continue;
        }

        strategy->mark_received_update();

        {
            auto tickers_lock = strategy->lock_on_pred_market_strat();
            for (auto& ticker : strategy->trade_tickers) {
                if (ticker.first == order_fill_info.symbol) {
                    ticker.second->on_order_filled_ack(*this, order_fill_info);
                }
            }
        }

        if (strategy->on_fill_callback) {
            strategy->on_fill_callback(*this, *strategy, order_fill_info);
        }

        if (!this->is_backtest) {
            if (!order_fill_info.markout_config.has_value()) {
                order_fill_info.markout_config = MarkoutConfig{};
            }
            if (order_fill_info.markout_config->duration_ms > 0) {
                this->register_markout(order_fill_info);
            }
        }
    }
}

std::vector<Core::Order> Core::Engine::get_orders(const std::string& symbol) {
    std::vector<Core::Order> ret;
    for (const auto& strategy : this->strategies_view()) {
        auto tickers_lock = strategy->lock_on_pred_market_strat();
        for (const auto& ticker : strategy->trade_tickers) {
            if (ticker.first == symbol) {
                const auto orders = ticker.second->get_orders();
                ret.insert(ret.end(), orders.begin(), orders.end());
            }
        }
    }
    return ret;
}

std::vector<Core::Order> Core::Engine::get_orders(
    const std::string& symbol, const std::string& exchange_name,
    const std::string& market_type) {
    std::vector<Core::Order> ret;
    for (const auto& strategy : this->strategies_view()) {
        auto tickers_lock = strategy->lock_on_pred_market_strat();
        for (const auto& ticker : strategy->trade_tickers) {
            if (ticker.first == symbol) {
                const auto orders = ticker.second->get_orders();
                for (const auto& order : orders) {
                    if (order.exchange_name == exchange_name &&
                        order.market_type == market_type) {
                        ret.push_back(order);
                    }
                }
            }
        }
    }
    return ret;
}

void Core::Engine::update_exposures_notional() {
    if (!this->is_state_ready.load()) {
        return;
    }

    auto now = std::chrono::steady_clock::now();
    if (!this->is_backtest) {
        if (now - this->last_update_exposures_notional_time.load() >
            std::chrono::seconds(60)) {
            this->last_update_exposures_notional_time.store(now);
        } else {
            return;
        }
    }

    std::unordered_map<long long, mpfr::mpreal> account_navs;
    std::unordered_map<std::string, Inventory> inventories_copy;
    std::unordered_map<std::string, QuoteAsset> quote_assets_copy;
    {
        std::shared_lock<std::shared_mutex> lock(this->inventory_mutex);
        inventories_copy = this->inventories;
        quote_assets_copy = this->quote_assets;
    }
    mpfr::mpreal total_long_exposure = MPREAL_ZERO;
    mpfr::mpreal total_short_exposure = MPREAL_ZERO;
    mpfr::mpreal quote_assets_notional = MPREAL_ZERO;

    mpfr::mpreal total_long_in_swaps = MPREAL_ZERO;
    mpfr::mpreal total_short_in_swaps = MPREAL_ZERO;

    mpfr::mpreal total_long_in_spot = MPREAL_ZERO;
    mpfr::mpreal total_short_in_spot = MPREAL_ZERO;

    mpfr::mpreal margin_notional = MPREAL_ZERO;

    for (const auto& exchange_key : quote_assets_copy) {
        const auto& quote_asset = exchange_key.second;
        quote_assets_notional += quote_asset.quantity * quote_asset.price;
        if (account_navs.find(quote_asset.account_id) != account_navs.end()) {
            account_navs[quote_asset.account_id] +=
                quote_asset.quantity * quote_asset.price;
        } else {
            account_navs[quote_asset.account_id] =
                quote_asset.quantity * quote_asset.price;
        }
    }

    for (const auto& [exchange_market_key, inventory] : inventories_copy) {
        const auto& price =
            this->get_price(inventory.quote_conversion_symbol,
                            inventory.exchange_name, inventory.market_type);

        if (price.has_value()) {
            if (inventory.total > MPREAL_ZERO) {
                total_long_exposure += inventory.total * price.value();

                if (inventory.market_type == "swap") {
                    total_long_in_swaps += inventory.total * price.value();
                }

                if (inventory.market_type == "spot") {
                    total_long_in_spot += inventory.total * price.value();
                }

                if (inventory.used_margin.has_value()) {
                    margin_notional += inventory.used_margin.value();
                }
            } else {
                total_short_exposure += (inventory.total * -1) * price.value();

                if (inventory.market_type == "swap") {
                    total_short_in_swaps +=
                        (inventory.total * -1) * price.value();
                }

                if (inventory.market_type == "spot") {
                    total_short_in_spot +=
                        (inventory.total * -1) * price.value();
                }

                if (inventory.used_margin.has_value()) {
                    margin_notional += inventory.used_margin.value();
                }
            }
        } else {
            if (inventory.quote_conversion_symbol == "USDUSDT") {
                continue;
            }
            this->create_system_log(
                inventory.quote_conversion_symbol, LOG_LEVEL_INFO,
                "update_exposures_notional: Price not found for symbol: " +
                    inventory.quote_conversion_symbol,
                TRADING_LOG_EVENT_TYPE_SYMBOL_PRICE_MISSING, nlohmann::json(),
                -1, inventory.exchange_name, inventory.market_type);
        }
    }

    mpfr::mpreal unrealized_pnl = MPREAL_ZERO;

    if (this->is_backtest) {
        for (const auto& strategy : this->strategies_view()) {
            auto tickers_lock = strategy->lock_on_pred_market_strat();
            for (const auto& ticker : strategy->trade_tickers) {
                for (const auto& trade : ticker.second->active_trades) {
                    // Skip unrealized PnL for prediction market trades —
                    // the engine tracks one price per symbol but polymarket
                    // has per-outcome prices, so unrealized PnL would be wrong.
                    if (trade.exchange_name == "polymarket") {
                        continue;
                    }
                    const auto& price = this->get_price(
                        trade.symbol, trade.exchange_name, trade.market_type);
                    if (!price.has_value()) {
                        continue;
                    }
                    const auto entry_price = trade.open_fills.empty()
                        ? trade.open_price : trade.get_average_open_price();
                    if (trade.type == TradeType::LONG) {
                        unrealized_pnl +=
                            (trade.open_quantity - trade.closed_quantity) *
                            (price.value() - entry_price);

                        if (account_navs.find(trade.account_id) !=
                            account_navs.end()) {
                            account_navs[trade.account_id] +=
                                (trade.open_quantity - trade.closed_quantity) *
                                (price.value() - entry_price);
                        }
                    } else {
                        unrealized_pnl +=
                            (trade.open_quantity - trade.closed_quantity) *
                            (entry_price - price.value());
                        if (account_navs.find(trade.account_id) !=
                            account_navs.end()) {
                            account_navs[trade.account_id] +=
                                (trade.open_quantity - trade.closed_quantity) *
                                (entry_price - price.value());
                        }
                    }
                }
            }
        }
    }

    mpfr::mpreal nav = quote_assets_notional + total_long_in_spot -
                       total_short_in_spot + unrealized_pnl;

    if (is_backtest) {
        nav = quote_assets_notional + unrealized_pnl;
    }

    {
        std::lock_guard<std::mutex> lock(this->engine_state_mutex);
        this->global_long_exposure = total_long_exposure.toDouble();
        this->global_short_exposure = total_short_exposure.toDouble();
        this->global_nav = nav.toDouble();
        this->global_margin_notional = margin_notional.toDouble();
        this->navs_by_account_id = account_navs;
    }

    this->risk_engine.update_navs_by_account_id(account_navs);

    for (const auto& strategy : this->strategies_view()) {
        strategy->update_navs_by_account_id(account_navs);
    }
}

std::optional<mpfr::mpreal> Core::Engine::get_nav_by_account_id(
    long long account_id) {
    std::lock_guard<std::mutex> lock(this->engine_state_mutex);
    if (this->navs_by_account_id.find(account_id) !=
        this->navs_by_account_id.end()) {
        return this->navs_by_account_id[account_id];
    }
    return std::nullopt;
}

void Core::Engine::create_system_log(
    const std::string& symbol, const std::string& log_level,
    const std::string& message, const std::string& event_type,
    const nlohmann::json& metadata, const int strategy_id,
    const std::string& exchange_name, const std::string& market_type,
    bool is_trading_activity_log) {
    if (!this->emit_trading_logs || this->is_backtest) {
        return;
    }

    auto log = TradingLog::create(symbol, strategy_id, log_level, message,
                                  event_type, metadata, "", exchange_name,
                                  market_type, is_trading_activity_log);

    // Track event_link_id range for snapshot archival. Low-watermark is set
    // once per interval (on first log after a snapshot); high-watermark
    // advances on every log. CAS-free — racing writers harmlessly overwrite.
    const auto link_id = static_cast<int64_t>(log.event_link_id);
    int64_t expected_lo = -1;
    this->snapshot_event_link_id_lo_.compare_exchange_strong(
        expected_lo, link_id, std::memory_order_relaxed);
    this->snapshot_event_link_id_hi_.store(link_id, std::memory_order_relaxed);

    this->trading_logs.enqueue(log);
}

double Core::Engine::get_nav() { return this->global_nav.load(); }

bool Core::Engine::is_order_creation_disabled() const {
    return is_order_creation_disabled_;
}

std::optional<Core::Trade> Core::Engine::find_trade(
    int strategy_id, const std::string& symbol, unsigned long long trade_id) {
    // never called live
    for (const auto& strategy : this->strategies_view()) {
        if (strategy->strategy_config.id != strategy_id) {
            continue;
        }
        auto tickers_lock = strategy->lock_on_pred_market_strat();
        for (const auto& ticker : strategy->trade_tickers) {
            if (ticker.first == symbol) {
                for (const auto& trade : ticker.second->active_trades) {
                    if (trade.strategy_id == strategy_id &&
                        trade.id == trade_id) {
                        return trade;
                    }
                }
            }
        }
    }
    return std::nullopt;
}

Core::EngineStateSnapshot Core::Engine::build_engine_state_snapshot_core(
    bool reset_funding) {
    EngineStateSnapshot snapshot;

    {
        std::lock_guard<std::mutex> lock(this->engine_state_mutex);
        snapshot.nav = this->global_nav.load();
        snapshot.margin_notional = this->global_margin_notional.load();
        snapshot.long_exposure = this->global_long_exposure.load();
        snapshot.short_exposure = this->global_short_exposure.load();
        snapshot.funding_collected = this->global_funding_payments.load();
        if (reset_funding) {
            this->global_funding_payments = 0;
        }
    }
    {
        std::shared_lock<std::shared_mutex> lock(this->inventory_mutex);
        snapshot.quote_assets = this->quote_assets;
        snapshot.inventories = this->inventories;
    }
    {
        std::unordered_map<std::string, double> prices_snapshot;
        {
            std::shared_lock<std::shared_mutex> lock(this->price_mutex);
            for (const auto& [symbol, price] : this->prices) {
                prices_snapshot[symbol] = price.load(std::memory_order_relaxed);
            }
        }
        snapshot.prices = std::move(prices_snapshot);
    }

    for (const auto& [key, order_book] : this->order_books) {
        auto bid_ask = order_book.get_best_bid_ask_with_qty();
        if (bid_ask.has_value()) {
            TopOfBook tob;
            tob.best_bid = bid_ask->bid_price;
            tob.best_ask = bid_ask->ask_price;
            tob.best_bid_qty = bid_ask->bid_qty;
            tob.best_ask_qty = bid_ask->ask_qty;
            snapshot.top_of_book[key] = tob;
        }

        if (this->active_orderbook_subscriptions.find(key) !=
            this->active_orderbook_subscriptions.end()) {
            if (order_book.is_in_sync) {
                // Propagate the source's ring window_bits so the snapshot
                // map slot is constructed with a matching ring size
                // (ring copy-assign requires same-sized source/dest).
                auto [it, _] = snapshot.order_books.try_emplace(
                    key, order_book.ring.window_bits);
                it->second.copy_from(order_book);
            }
        }
    }

    snapshot.active_trades = this->get_trades();
    snapshot.active_orders = this->get_orders();

    for (const auto& strategy : this->strategies_view()) {
        auto tickers_lock = strategy->lock_on_pred_market_strat();
        for (const auto& [symbol, ticker] : strategy->trade_tickers) {
            auto signal = ticker->get_last_actionable_signal();
            if (signal.has_value()) {
                auto sig = signal.value();
                sig.symbol = symbol;
                snapshot.actionable_signals.push_back(sig);
            }
        }
    }

    for (const auto& strategy : this->strategies_view()) {
        auto tickers_lock = strategy->lock_on_pred_market_strat();
        for (const auto& [symbol, ticker] : strategy->trade_tickers) {
            auto signal = ticker->get_last_signal();
            if (signal.has_value()) {
                auto sig = signal.value();
                sig.symbol = symbol;
                snapshot.all_signals.push_back(sig);
            }
        }
    }

    return snapshot;
}

void Core::Engine::emit_engine_state_snapshot(bool bypass_guard) {
    if (!this->is_state_ready.load()) {
        return;
    }

    if (!this->is_backtest && !bypass_guard) {
        auto now = std::chrono::steady_clock::now();
        if (now - this->last_engine_state_snapshot_time >
            std::chrono::seconds(60 * 5)) {
            this->last_engine_state_snapshot_time = now;
        } else {
            return;
        }
    }

    auto snapshot = this->build_engine_state_snapshot_core(true);

    {
        std::lock_guard<std::mutex> lock(this->new_trades_mutex);
        snapshot.new_trades = this->new_trades;
        this->new_trades.clear();
    }
    if (!this->skip_historical_order_accumulation_on_backtest) {
        {
            std::lock_guard<std::mutex> lock(this->new_orders_mutex);
            snapshot.new_orders = this->new_orders;
            this->new_orders.clear();
        }
        {
            std::lock_guard<std::mutex> lock(this->new_orders_cancels_mutex);
            snapshot.new_orders_cancels = this->new_orders_cancels;
            this->new_orders_cancels.clear();
        }
        {
            std::lock_guard<std::mutex> lock(this->new_order_fills_mutex);
            snapshot.new_order_fills = this->new_order_fills;
            this->new_order_fills.clear();
        }
    } else {
        {
            std::lock_guard<std::mutex> lock(this->new_orders_mutex);
            this->new_orders.clear();
        }
        {
            std::lock_guard<std::mutex> lock(this->new_orders_cancels_mutex);
            this->new_orders_cancels.clear();
        }
        {
            std::lock_guard<std::mutex> lock(this->new_order_fills_mutex);
            this->new_order_fills.clear();
        }
    }

    {
        std::lock_guard<std::mutex> lock(this->completed_markouts_mutex_);
        snapshot.completed_markouts = std::move(this->completed_markouts_);
        this->completed_markouts_.clear();
    }

    snapshot.timestamp_ms = this->get_time();

    // Observability: engine identity + event_link_id range since last
    // snapshot. Read-and-reset the lo/hi watermarks so the next interval
    // starts fresh. See EngineStateSnapshot.event_link_id_{lo,hi} comments.
    snapshot.engine_instance = this->engine_instance_;
    snapshot.event_link_id_lo = this->snapshot_event_link_id_lo_.exchange(
        -1, std::memory_order_acq_rel);
    snapshot.event_link_id_hi =
        this->snapshot_event_link_id_hi_.load(std::memory_order_relaxed);

    if (!this->is_backtest) {
        this->periodic_tasks_thread_pool->enqueue(
            [this, snapshot = std::move(snapshot)]() {
                this->engine_state_snapshot_callback_(snapshot);
            });
    } else {
        this->engine_state_snapshot_callback_(snapshot);
    }
}

mpfr::mpreal Core::Engine::get_fee_rate(const std::string& exchange,
                                        const std::string& market_type,
                                        bool is_maker) {
    const std::string& key =
        Utils::get_exchange_market_key(exchange, market_type);
    auto it = this->fees_by_exchange_and_market.find(key);
    if (it != this->fees_by_exchange_and_market.end()) {
        return is_maker ? it->second.maker_fee_rate : it->second.taker_fee_rate;
    }
    return MPREAL_ZERO;
}

void Core::Engine::add_trade_to_logs(const Core::Trade& trade) {
    std::lock_guard<std::mutex> lock(this->new_trades_mutex);
    const auto& key = Utils::get_symbol_exchange_market_key(
        trade.symbol, trade.exchange_name, trade.market_type);
    this->new_trades[key].push_back(trade);
}

void Core::Engine::add_order_to_logs(const Core::Order& order) {
    std::lock_guard<std::mutex> lock(this->new_orders_mutex);
    const auto& key = Utils::get_symbol_exchange_market_key(
        order.symbol, order.exchange_name, order.market_type);
    this->new_orders[key].push_back(order);
}

void Core::Engine::add_order_fill_to_logs(
    const Core::OrderFillInfo& order_fill_info) {
    std::lock_guard<std::mutex> lock(this->new_order_fills_mutex);
    const auto& key = Utils::get_symbol_exchange_market_key(
        order_fill_info.symbol, order_fill_info.exchange_name,
        order_fill_info.market_type);
    this->new_order_fills[key].push_back(order_fill_info);
}

void Core::Engine::add_order_cancel_to_logs(const Core::Order& order) {
    std::lock_guard<std::mutex> lock(this->new_orders_cancels_mutex);
    const auto& key = Utils::get_symbol_exchange_market_key(
        order.symbol, order.exchange_name, order.market_type);
    this->new_orders_cancels[key].push_back(order);
}

void Core::Engine::load_models() {
    auto try_load_digital_option_calibration_map =
        [&](int strategy_id, const std::string& model_path) {
            auto parent = std::filesystem::path(model_path).parent_path();
            auto cal_path = parent / "calibration_map.json";
            if (std::filesystem::exists(cal_path)) {
                auto cal_key = Utils::get_model_key(strategy_id, model_path);
                if (this->digital_option_calibration_maps_.find(cal_key) ==
                    this->digital_option_calibration_maps_.end()) {
                    this->digital_option_calibration_maps_[cal_key] =
                        DigitalOptionCalibrationMap::load(cal_path.string());
                }
            }
        };

    for (const auto& strategy : this->strategies_view()) {
        for (const auto& model_meta :
             strategy->strategy_config.strategy_models) {
            const auto model_key = Utils::get_model_key(
                strategy->strategy_config.id, model_meta.path_to_model);
            if (!model_meta.path_to_model.empty() &&
                this->model_path_to_model_map.find(model_key) ==
                    this->model_path_to_model_map.end()) {
                this->model_path_to_model_map[model_key] =
                    std::make_unique<OnnxModel>(
                        model_meta.path_to_model.c_str());
                try_load_digital_option_calibration_map(
                    strategy->strategy_config.id, model_meta.path_to_model);
            }
        }
    }
}

void Core::Engine::state_is_ready() {
    this->is_state_ready.store(true);
    this->create_system_log("", LOG_LEVEL_INFO, "Engine is ready to trade",
                            TRADING_LOG_EVENT_TYPE_SYSTEM_STARTUP,
                            nlohmann::json(), -1, "", "");
}

void Core::Engine::stop_trading() { this->is_state_ready.store(false); }

std::vector<Core::Trade> Core::Engine::get_trades() {
    std::vector<Core::Trade> ret;
    for (const auto& strategy : this->strategies_view()) {
        const auto trades = strategy->get_trades();
        ret.insert(ret.end(), trades.begin(), trades.end());
    }
    return ret;
}

std::vector<Core::Order> Core::Engine::get_orders() {
    std::vector<Core::Order> ret;
    for (const auto& strategy : this->strategies_view()) {
        const auto orders = strategy->get_orders();
        ret.insert(ret.end(), orders.begin(), orders.end());
    }
    return ret;
}

std::vector<Core::Order> Core::Engine::get_removed_orders() {
    std::vector<Core::Order> ret;
    for (const auto& strategy : this->strategies_view()) {
        auto tickers_lock = strategy->lock_on_pred_market_strat();
        for (const auto& ticker : strategy->trade_tickers) {
            const auto orders = ticker.second->get_removed_orders();
            ret.insert(ret.end(), orders.begin(), orders.end());
        }
    }
    return ret;
}

Core::EngineStateSnapshot Core::Engine::build_realtime_snapshot() {
    if (!this->is_state_ready.load()) {
        return Core::EngineStateSnapshot{};
    }
    auto snapshot = this->build_engine_state_snapshot_core(false);
    snapshot.timestamp_ms = DatetimeUtils::get_current_time_ms();
    snapshot.program_start_time_ms = this->program_start_time_ms;
    return snapshot;
}

void Core::Engine::recover_state_from_disk(
    const std::vector<Core::Trade>& trades,
    const std::vector<Core::Order>& orders) {
    nlohmann::json metadata = nlohmann::json::object();

    metadata["trades"] = nlohmann::json::array();
    for (const auto& trade : trades) {
        metadata["trades"].push_back(trade.to_json());
    }
    metadata["orders"] = nlohmann::json::array();
    for (const auto& order : orders) {
        metadata["orders"].push_back(order.to_json());
    }

    this->create_system_log("", LOG_LEVEL_INFO, "Recovered state from disk",
                            TRADING_LOG_EVENT_TYPE_TRADING_STATE_SYNC, metadata,
                            -1, "", "");
    {
        std::lock_guard<std::mutex> lock(this->held_state_mutex_);
        this->held_trades_ = trades;
        this->held_orders_ = orders;
    }
    std::unordered_set<int> loaded_ids;
    for (const auto& strategy : this->strategies_view()) {
        loaded_ids.insert(strategy->strategy_config.id);
        this->restore_held_state(*strategy, true);
    }
    std::map<int, std::pair<int, int>> unloaded;
    {
        std::lock_guard<std::mutex> lock(this->held_state_mutex_);
        for (const auto& trade : this->held_trades_) {
            if (!loaded_ids.contains(trade.strategy_id)) {
                unloaded[trade.strategy_id].first++;
            }
        }
        for (const auto& order : this->held_orders_) {
            if (!loaded_ids.contains(order.strategy_id)) {
                unloaded[order.strategy_id].second++;
            }
        }
    }
    for (const auto& [strategy_id, counts] : unloaded) {
        this->create_system_log(
            "", LOG_LEVEL_ERROR,
            "Strategy " + std::to_string(strategy_id) +
                " is not loaded; its trades and orders are kept in the state "
                "file",
            TRADING_LOG_EVENT_TYPE_HUMAN_INTERVENTION_REQUIRED,
            {{"held_trades", counts.first}, {"held_orders", counts.second}},
            strategy_id, "", "");
    }
    this->risk_engine.add_trades(trades);
    this->risk_engine.add_orders(orders);
}

std::vector<Core::Trade> Core::Engine::get_held_trades() {
    std::lock_guard<std::mutex> lock(this->held_state_mutex_);
    return this->held_trades_;
}

std::vector<Core::Order> Core::Engine::get_held_orders() {
    std::lock_guard<std::mutex> lock(this->held_state_mutex_);
    return this->held_orders_;
}

void Core::Engine::restore_held_state(Core::Strategy& strategy,
                                      bool at_startup) {
    const int id = strategy.strategy_config.id;
    std::lock_guard<std::mutex> lock(this->held_state_mutex_);
    const auto count_held = [&] {
        std::pair<int, int> held{0, 0};
        for (const auto& t : this->held_trades_) held.first += t.strategy_id == id;
        for (const auto& o : this->held_orders_) held.second += o.strategy_id == id;
        return held;
    };
    const auto before = count_held();
    if (!at_startup && before.first == 0 && before.second == 0) return;

    const auto recovered =
        strategy.recover_state_from_disk(*this, this->held_trades_,
                                         this->held_orders_);
    std::erase_if(this->held_trades_, [&](const Core::Trade& t) {
        return t.strategy_id == id && recovered.trades.contains(t.id);
    });
    std::erase_if(this->held_orders_, [&](const Core::Order& o) {
        return o.strategy_id == id && recovered.orders.contains(o.id);
    });
    const auto after = count_held();
    const nlohmann::json counts = {
        {"trades_on_disk", before.first},
        {"trades_restored", before.first - after.first},
        {"orders_on_disk", before.second},
        {"orders_restored", before.second - after.second}};

    if (after.first == 0 && after.second == 0) {
        this->held_paused_strategy_ids_.erase(id);
        this->create_system_log("", LOG_LEVEL_INFO,
                                "Restored strategy state from disk",
                                TRADING_LOG_EVENT_TYPE_TRADING_STATE_SYNC,
                                counts, id, "", "");
        return;
    }
    strategy.set_disabled(true);
    this->held_paused_strategy_ids_.insert(id);
    this->create_system_log(
        "", LOG_LEVEL_ERROR,
        "Strategy paused: " + std::to_string(after.first) + " trades and " +
            std::to_string(after.second) +
            " orders from disk have no ticker; they are kept in the state "
            "file until restored or manually removed",
        TRADING_LOG_EVENT_TYPE_HUMAN_INTERVENTION_REQUIRED, counts, id, "",
        "");
}

bool Core::Engine::remove_held_trade(
    const Core::ManuallyRemovedTrade& removal) {
    bool resume = false;
    {
        std::lock_guard<std::mutex> lock(this->held_state_mutex_);
        const auto removed = std::erase_if(
            this->held_trades_, [&](const Core::Trade& t) {
                return t.id == removal.trade_id &&
                       t.strategy_id == removal.strategy_id;
            });
        if (removed == 0) return false;
        const bool still_held =
            std::any_of(this->held_trades_.begin(), this->held_trades_.end(),
                        [&](const Core::Trade& t) {
                            return t.strategy_id == removal.strategy_id;
                        }) ||
            std::any_of(this->held_orders_.begin(), this->held_orders_.end(),
                        [&](const Core::Order& o) {
                            return o.strategy_id == removal.strategy_id;
                        });
        resume = !still_held &&
                 this->held_paused_strategy_ids_.erase(removal.strategy_id) > 0;
    }
    this->remove_trade_from_risk_engine(removal.trade_id);
    this->create_system_log(
        removal.symbol, LOG_LEVEL_WARNING, "Held trade manually removed",
        TRADING_LOG_EVENT_TYPE_TRADE_MANUALLY_REMOVED,
        {{"trade_id", removal.trade_id},
         {"db_id", removal.db_id},
         {"outcome", "removed_from_held_state"}},
        removal.strategy_id, "", "");
    if (!resume) return true;
    for (const auto& strategy : this->strategies_view()) {
        if (strategy->strategy_config.id != removal.strategy_id) continue;
        strategy->set_disabled(strategy->strategy_config.is_disabled);
        this->create_system_log(
            "", LOG_LEVEL_INFO, "No held state left; strategy resumed",
            TRADING_LOG_EVENT_TYPE_STRATEGY_RESUMED, nlohmann::json::object(),
            removal.strategy_id, "", "");
    }
    return true;
}

void Core::Engine::order_canceled_ack(Core::Order& order,
                                      bool creation_failed) {
    for (const auto& strategy : this->strategies_view()) {
        if (strategy->strategy_config.id != order.strategy_id) {
            continue;
        }

        auto tickers_lock = strategy->lock_on_pred_market_strat();
        for (auto& ticker : strategy->trade_tickers) {
            if (ticker.first == order.symbol) {
                ticker.second->on_order_cancel_success(*this, order,
                                                       creation_failed);
            }
        }
    }
}

void Core::Engine::inventory_mismatch_remove_trade(const Core::Trade& trade) {
    this->create_system_log(
        trade.symbol, LOG_LEVEL_ERROR,
        "Inventory mismatch, removing trade: " + trade.symbol,
        TRADING_LOG_EVENT_TYPE_TRADE_REMOVED_INVENTORY_MISMATCH,
        nlohmann::json(), trade.strategy_id, trade.exchange_name,
        trade.market_type);
    for (const auto& strategy : this->strategies_view()) {
        if (strategy->strategy_config.id != trade.strategy_id) {
            continue;
        }
        auto tickers_lock = strategy->lock_on_pred_market_strat();
        for (auto& ticker : strategy->trade_tickers) {
            if (ticker.first == trade.symbol) {
                this->create_system_log(
                    trade.symbol, LOG_LEVEL_ERROR,
                    "Inventory mismatch, removing trade: " + trade.symbol,
                    TRADING_LOG_EVENT_TYPE_TRADE_REMOVED_INVENTORY_MISMATCH,
                    nlohmann::json(), trade.strategy_id, trade.exchange_name,
                    trade.market_type);
                ticker.second->inventory_mismatch_remove_trade(*this, trade);
            }
        }
    }
}

bool Core::Engine::manually_remove_trade(
    const Core::ManuallyRemovedTrade& removal) {
    nlohmann::json metadata;
    metadata["trade_id"] = removal.trade_id;
    metadata["db_id"] = removal.db_id;
    metadata["removed_by"] = "operator_pending_trade_removals";
    if (this->remove_held_trade(removal)) return true;

    for (const auto& strategy : this->strategies_view()) {
        if (strategy->strategy_config.id != removal.strategy_id) {
            continue;
        }

        auto tickers_lock = strategy->lock_on_pred_market_strat();
        for (auto& ticker : strategy->trade_tickers) {
            if (ticker.first != removal.symbol) {
                continue;
            }

            bool removed =
                ticker.second->manually_remove_trade_by_id(removal.trade_id);
            if (removed) {
                this->remove_trade_from_risk_engine(removal.trade_id);
                metadata["outcome"] = "removed";
                this->create_system_log(
                    removal.symbol, LOG_LEVEL_WARNING, "Trade manually removed",
                    TRADING_LOG_EVENT_TYPE_TRADE_MANUALLY_REMOVED, metadata,
                    removal.strategy_id, "", "");
            } else {
                metadata["outcome"] = "not_found_in_ticker";
                this->create_system_log(
                    removal.symbol, LOG_LEVEL_ERROR,
                    "Manual trade removal: trade not found in ticker",
                    TRADING_LOG_EVENT_TYPE_TRADE_MANUALLY_REMOVED, metadata,
                    removal.strategy_id, "", "");
            }
            return removed;
        }
    }

    metadata["outcome"] = "no_matching_strategy_or_ticker";
    this->create_system_log(removal.symbol, LOG_LEVEL_ERROR,
                            "Manual trade removal: no matching strategy/ticker",
                            TRADING_LOG_EVENT_TYPE_TRADE_MANUALLY_REMOVED,
                            metadata, removal.strategy_id, "", "");
    return false;
}

int Core::Engine::sweep_pulled_quotes() {
    int resting = 0;
    for (auto& strategy : this->strategies_view()) {
        if (!strategy->quotes_pulled.load()) continue;
        std::vector<StrategyDecision> decisions;
        {
            std::shared_lock<std::shared_mutex> tickers_lock(
                strategy->trade_tickers_mutex);
            for (auto& [symbol, ticker] : strategy->trade_tickers) {
                StrategyDecision decision;
                decision.symbol = symbol;
                std::lock_guard<std::mutex> lock(ticker->trading_mutex);
                for (auto& order : ticker->get_active_orders_mut_ref()) {
                    if (!Strategy::is_quote(order.type,
                                            order.is_trade_close_order)) {
                        continue;
                    }
                    ++resting;
                    if (!ticker->is_cancel_already_sent(order)) {
                        ticker->mark_order_to_be_cancelled(*this, order,
                                                           decision);
                    }
                }
                if (!decision.cancelled_orders.empty()) {
                    decisions.push_back(std::move(decision));
                }
            }
        }
        strategy->process_strategy_decisions(*this, decisions);
    }
    return resting;
}

void Core::Engine::register_engine_signal_handler(
    const std::string& signal_type, EngineSignalHandler handler) {
    std::lock_guard<std::mutex> lock(this->engine_signal_handlers_mutex);
    this->engine_signal_handlers[signal_type] = std::move(handler);
}

std::vector<std::string> Core::Engine::engine_signal_types() {
    std::lock_guard<std::mutex> lock(this->engine_signal_handlers_mutex);
    std::vector<std::string> types;
    types.reserve(this->engine_signal_handlers.size());
    for (const auto& [signal_type, handler] : this->engine_signal_handlers) {
        (void)handler;
        types.push_back(signal_type);
    }
    std::sort(types.begin(), types.end());
    return types;
}

void Core::Engine::process_engine_signals() {
    if (!this->enable_engine_signals) {
        return;
    }

    auto now = std::chrono::steady_clock::now();
    if (now - this->last_process_engine_signals_time.load() <
        std::chrono::milliseconds(500)) {
        return;
    }
    this->last_process_engine_signals_time.store(now);
    if (this->engine_signals_in_flight.exchange(true)) {
        return;
    }

    this->periodic_tasks_thread_pool->enqueue([this]() {
        struct ClearInFlight {
            std::atomic<bool>& flag;
            ~ClearInFlight() { flag.store(false); }
        } clear{this->engine_signals_in_flight};

        std::vector<EngineSignal> signals;
        try {
            signals =
                this->fetch_engine_signals_callback(this->engine_signal_types());
        } catch (const std::exception& e) {
            this->create_system_log(
                "", LOG_LEVEL_ERROR,
                "Failed to fetch engine signals: " + std::string(e.what()),
                TRADING_LOG_EVENT_TYPE_ENGINE_SIGNALS_FETCH_FAILED,
                nlohmann::json(), -1, "", "");
            return;
        }

        const auto outcomes = this->apply_engine_signals(signals);
        for (std::size_t i = 0; i < signals.size(); ++i) {
            try {
                this->ack_engine_signal_callback(signals[i], outcomes[i]);
            } catch (const std::exception& e) {
                this->create_system_log(
                    "", LOG_LEVEL_ERROR,
                    "Failed to acknowledge engine signal: " +
                        std::string(e.what()),
                    TRADING_LOG_EVENT_TYPE_ENGINE_SIGNAL_ACK_FAILED,
                    nlohmann::json{
                        {"signal_id", signals[i].id},
                        {"signal_type", signals[i].signal_type},
                        {"status", engine_signal_status_name(outcomes[i].status)}},
                    -1, "", "");
            }
        }
    });
}

std::vector<Core::EngineSignalOutcome> Core::Engine::apply_engine_signals(
    const std::vector<EngineSignal>& signals) {
    std::vector<EngineSignalOutcome> outcomes;
    outcomes.reserve(signals.size());
    for (const auto& signal : signals) {
        EngineSignalHandler handler;
        {
            std::lock_guard<std::mutex> lock(this->engine_signal_handlers_mutex);
            const auto it = this->engine_signal_handlers.find(signal.signal_type);
            if (it != this->engine_signal_handlers.end()) handler = it->second;
        }
        if (!handler) {
            outcomes.push_back(
                {EngineSignalStatus::SKIPPED, "no handler for " + signal.signal_type});
            continue;
        }
        try {
            outcomes.push_back(handler(signal));
        } catch (const std::exception& e) {
            outcomes.push_back({EngineSignalStatus::FAILED, e.what()});
        }
    }
    return outcomes;
}

void Core::Engine::poll_realtime_subscriptions() {
    if (!this->enable_realtime_subscriptions) {
        return;
    }

    auto now = std::chrono::steady_clock::now();
    if (now - this->last_poll_realtime_subscriptions_time.load() <
        std::chrono::seconds(this->seconds_between_subscription_polls)) {
        return;
    }
    this->last_poll_realtime_subscriptions_time.store(now);

    this->periodic_tasks_thread_pool->enqueue([this]() {
        std::vector<RealtimeDataSubscription> subscriptions;
        try {
            subscriptions = this->fetch_realtime_subscriptions_callback();
        } catch (const std::exception& e) {
            this->create_system_log(
                "", LOG_LEVEL_ERROR,
                "Failed to fetch realtime subscriptions: " +
                    std::string(e.what()),
                TRADING_LOG_EVENT_TYPE_REALTIME_SUBSCRIPTIONS_FETCH_FAILED,
                nlohmann::json(), -1, "", "");
            return;
        }

        std::unordered_set<std::string> new_subscriptions;
        for (const auto& sub : subscriptions) {
            if (!sub.is_active || sub.data_type != "orderbook") {
                continue;
            }
            const std::string key = Utils::get_symbol_exchange_market_key(
                sub.symbol, sub.exchange_name, sub.market_type);
            new_subscriptions.insert(key);

            if (this->active_orderbook_subscriptions.find(key) ==
                this->active_orderbook_subscriptions.end()) {
                this->init_orderbook(sub.exchange_name, sub.market_type,
                                     sub.symbol);
                this->active_orderbook_subscriptions.insert(key);
                this->create_system_log(
                    sub.symbol, LOG_LEVEL_INFO,
                    "Added orderbook subscription: " + key,
                    TRADING_LOG_EVENT_TYPE_ORDERBOOK_SUBSCRIPTION_CHANGED,
                    nlohmann::json(), -1, sub.exchange_name, sub.market_type);
            }
        }

        std::vector<std::string> to_remove;
        for (const auto& key : this->active_orderbook_subscriptions) {
            if (new_subscriptions.find(key) == new_subscriptions.end()) {
                to_remove.push_back(key);
            }
        }
        for (const auto& key : to_remove) {
            this->active_orderbook_subscriptions.erase(key);
            this->create_system_log(
                "", LOG_LEVEL_INFO, "Removed orderbook subscription: " + key,
                TRADING_LOG_EVENT_TYPE_ORDERBOOK_SUBSCRIPTION_CHANGED,
                nlohmann::json(), -1, "", "");
        }
    });
}

std::unordered_map<std::string, std::vector<Core::Trade>>
Core::Engine::get_new_trades() {
    std::lock_guard<std::mutex> lock(this->new_trades_mutex);
    return this->new_trades;
}

Core::StrategiesView Core::Engine::get_strategies() {
    return this->strategies_view();
}

std::unordered_map<std::string, std::vector<Core::Order>>
Core::Engine::get_new_orders() {
    std::lock_guard<std::mutex> lock(this->new_orders_mutex);
    return this->new_orders;
}

std::unordered_map<std::string, std::vector<Core::Order>>
Core::Engine::get_new_orders_cancels() {
    std::lock_guard<std::mutex> lock(this->new_orders_cancels_mutex);
    return this->new_orders_cancels;
}

std::unordered_map<std::string, std::vector<Core::OrderFillInfo>>
Core::Engine::get_new_order_fills() {
    std::lock_guard<std::mutex> lock(this->new_order_fills_mutex);
    return this->new_order_fills;
}

std::unordered_map<std::string, std::vector<Core::Order>>
Core::Engine::get_tracked_misc_orders() {
    std::lock_guard<std::mutex> lock(this->engine_state_mutex);
    return this->tracked_misc_orders;
}

void Core::Engine::set_tracked_misc_orders(
    const std::unordered_map<std::string, std::vector<Core::Order>>&
        tracked_misc_orders) {
    std::lock_guard<std::mutex> lock(this->engine_state_mutex);
    this->tracked_misc_orders = tracked_misc_orders;
}

void Core::Engine::set_unsynced_analytics_data(
    std::unordered_map<std::string, std::vector<Core::Order>> new_orders,
    std::unordered_map<std::string, std::vector<Core::Trade>> new_trades,
    std::unordered_map<std::string, std::vector<Core::Order>>
        new_orders_cancels,
    std::unordered_map<std::string, std::vector<Core::OrderFillInfo>>
        new_order_fills) {
    {
        std::lock_guard<std::mutex> lock(this->new_orders_mutex);
        this->new_orders = std::move(new_orders);
    }
    {
        std::lock_guard<std::mutex> lock(this->new_trades_mutex);
        this->new_trades = std::move(new_trades);
    }
    {
        std::lock_guard<std::mutex> lock(this->new_orders_cancels_mutex);
        this->new_orders_cancels = std::move(new_orders_cancels);
    }
    {
        std::lock_guard<std::mutex> lock(this->new_order_fills_mutex);
        this->new_order_fills = std::move(new_order_fills);
    }
}

long long Core::Engine::get_latest_fully_completed_kline_open_time_ms(
    long long interval_ms) const {
    const auto now_ms = this->get_time();
    return now_ms - (now_ms % interval_ms) -
           (now_ms % interval_ms == 0 ? interval_ms : 0) - interval_ms;
}

long long Core::Engine::get_latest_fully_completed_kline_close_time_ms(
    long long interval_ms) const {
    const auto now_ms = this->get_time();
    return now_ms - (now_ms % interval_ms);
}

long long Core::Engine::get_time_to_next_kline_close(
    long long interval_ms) const {
    const auto time = this->get_time();
    const auto latest_fully_completed_kline_ms =
        this->get_latest_fully_completed_kline_open_time_ms(interval_ms);
    return latest_fully_completed_kline_ms + (interval_ms * 2) - time;
}

void Core::Engine::add_symbol_exchange_info(
    const std::string& exchange_name, const std::string& market_type,
    const std::vector<Core::SymbolExchangeInfo>& exchange_market_symbols) {
    std::lock_guard<std::shared_mutex> lock(this->exchange_symbol_infos_mutex);
    for (auto& symbol : exchange_market_symbols) {
        const auto& key = Utils::get_symbol_exchange_market_key(
            symbol.symbol, exchange_name, market_type);
        this->exchange_symbol_infos[key] = symbol;
    }
}

bool Core::Engine::passes_min_notional_filter(const Core::Order& order) {
    static std::unordered_map<std::string, bool> failed_keys;
    static auto last_print_time = std::chrono::steady_clock::now();

    if (this->is_backtest) {
        return true;
    }
    const auto& key = Utils::get_symbol_exchange_market_key(
        order.symbol, order.exchange_name, order.market_type);
    std::shared_lock<std::shared_mutex> lock(this->exchange_symbol_infos_mutex);
    if (this->exchange_symbol_infos.find(key) !=
        this->exchange_symbol_infos.end()) {
        const auto& info = this->exchange_symbol_infos.at(key);
        const auto& min_notional = info.limits.cost.min;
        const auto& price = order.price;
        const auto& quantity = order.quantity;
        const auto& notional = price * quantity;
        if (notional < min_notional) {
            failed_keys[key] = true;
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::minutes>(
                    now - last_print_time)
                    .count() >= 1) {
                std::cout << "Keys failing min notional filter:" << "\n";
                for (const auto& [k, _] : failed_keys) {
                    std::cout << "  " << k << "\n";
                }
                failed_keys.clear();
                last_print_time = now;
            }
            return false;
        }
        return true;
    }
    this->create_system_log(
        order.symbol, LOG_LEVEL_ERROR,
        "Symbol not found in exchange symbol infos, not allowing order to "
        "pass: " +
            order.symbol,
        TRADING_LOG_EVENT_TYPE_ORDER_REJECTED_SYMBOL_UNKNOWN,
        nlohmann::json{
            {"order_id", order.id},
            {"id_str", order.id_str},
            {"symbol", order.symbol},
            {"exchange_name", order.exchange_name},
            {"market_type", order.market_type},
            {"strategy_id", order.strategy_id},
            {"side", order.side == Core::OrderSide::BUY ? "BUY" : "SELL"},
            {"client_order_id", order.client_order_id},
        },
        order.strategy_id, order.exchange_name, order.market_type);
    return false;
}

std::vector<Core::Order> Core::Engine::get_unacked_orders() {
    std::vector<Core::Order> ret;
    for (const auto& strategy : this->strategies_view()) {
        const auto orders = strategy->get_orders();
        for (const auto& order : orders) {
            if (!order.is_order_creation_acknowledged) {
                ret.push_back(order);
            }
        }
    }
    return ret;
}

std::vector<Core::Order> Core::Engine::get_failed_cancels() {
    std::vector<Core::Order> ret;
    const auto now_ms = this->get_time();
    for (const auto& strategy : this->strategies_view()) {
        const auto orders = strategy->get_orders();
        for (const auto& order : orders) {
            if (now_ms - order.first_cancel_request_time_ms > 15000) {
                ret.push_back(order);
            }
        }
    }
    return ret;
}

void Core::Engine::round_quantity_and_price(Core::Order& order) {
    if (this->is_backtest || order.is_prediction_market_order) {
        return;
    }
    order.price =
        this->round_price(order.get_potentially_alt_routed_symbol(),
                          order.exchange_name, order.market_type, order.price);
    order.price_str = StrMath::to_fixed_string(
        order.price,
        this->get_price_precision(order.get_potentially_alt_routed_symbol(),
                                  order.exchange_name, order.market_type));
    order.quantity = this->round_quantity(
        order.get_potentially_alt_routed_symbol(), order.exchange_name,
        order.market_type, order.quantity);
    order.quantity_str = StrMath::to_fixed_string(
        order.quantity,
        this->get_qty_precision(order.get_potentially_alt_routed_symbol(),
                                order.exchange_name, order.market_type));
}

std::string Core::Engine::round_quantity_str(const mpfr::mpreal& quantity,
                                             const std::string& symbol,
                                             const std::string& exchange_name,
                                             const std::string& market_type) {
    const auto& key = Utils::get_symbol_exchange_market_key(
        symbol, exchange_name, market_type);
    std::shared_lock<std::shared_mutex> lock(this->exchange_symbol_infos_mutex);
    if (this->exchange_symbol_infos.find(key) !=
        this->exchange_symbol_infos.end()) {
        const auto& info = this->exchange_symbol_infos.at(key);
        return StrMath::to_fixed_string(quantity, info.precision_int.amount);
    }
    return quantity.toString();
}
mpfr::mpreal Core::Engine::round_quantity(const std::string& symbol,
                                          const std::string& exchange_name,
                                          const std::string& market_type,
                                          const mpfr::mpreal& quantity) {
    const auto& key = Utils::get_symbol_exchange_market_key(
        symbol, exchange_name, market_type);
    std::shared_lock<std::shared_mutex> lock(this->exchange_symbol_infos_mutex);
    if (this->exchange_symbol_infos.find(key) !=
        this->exchange_symbol_infos.end()) {
        const auto& info = this->exchange_symbol_infos.at(key);
        return StrMath::to_fixed_string(quantity, info.precision_int.amount);
    }
    return quantity.toString();
}

mpfr::mpreal Core::Engine::round_price(const std::string& symbol,
                                       const std::string& exchange_name,
                                       const std::string& market_type,
                                       const mpfr::mpreal& price) {
    const auto& key = Utils::get_symbol_exchange_market_key(
        symbol, exchange_name, market_type);
    std::shared_lock<std::shared_mutex> lock(this->exchange_symbol_infos_mutex);
    if (this->exchange_symbol_infos.find(key) !=
        this->exchange_symbol_infos.end()) {
        const auto& info = this->exchange_symbol_infos.at(key);
        return StrMath::to_fixed_string(price, info.precision_int.price);
    }
    return price;
}

int Core::Engine::get_qty_precision(const std::string& symbol,
                                    const std::string& exchange_name,
                                    const std::string& market_type) {
    const auto& key = Utils::get_symbol_exchange_market_key(
        symbol, exchange_name, market_type);
    std::shared_lock<std::shared_mutex> lock(this->exchange_symbol_infos_mutex);
    if (this->exchange_symbol_infos.find(key) !=
        this->exchange_symbol_infos.end()) {
        const auto& info = this->exchange_symbol_infos.at(key);
        return info.precision_int.amount;
    }
    return 0;
}

int Core::Engine::get_price_precision(const std::string& symbol,
                                      const std::string& exchange_name,
                                      const std::string& market_type) {
    const auto& key = Utils::get_symbol_exchange_market_key(
        symbol, exchange_name, market_type);
    std::shared_lock<std::shared_mutex> lock(this->exchange_symbol_infos_mutex);
    if (this->exchange_symbol_infos.find(key) !=
        this->exchange_symbol_infos.end()) {
        const auto& info = this->exchange_symbol_infos.at(key);
        return info.precision_int.price;
    }
    return 0;
}

double Core::Engine::get_tick_size(const std::string& symbol,
                                   const std::string& exchange_name,
                                   const std::string& market_type) {
    const auto& key = Utils::get_symbol_exchange_market_key(
        symbol, exchange_name, market_type);
    std::shared_lock<std::shared_mutex> lock(this->exchange_symbol_infos_mutex);
    if (this->exchange_symbol_infos.find(key) !=
        this->exchange_symbol_infos.end()) {
        const auto& info = this->exchange_symbol_infos.at(key);
        return info.precision.price;
    }
    return 0;
}

double Core::Engine::get_step_size(const std::string& symbol,
                                   const std::string& exchange_name,
                                   const std::string& market_type) {
    const auto& key = Utils::get_symbol_exchange_market_key(
        symbol, exchange_name, market_type);
    std::shared_lock<std::shared_mutex> lock(this->exchange_symbol_infos_mutex);
    if (this->exchange_symbol_infos.find(key) !=
        this->exchange_symbol_infos.end()) {
        const auto& info = this->exchange_symbol_infos.at(key);
        return info.precision.amount;
    }
    return 0;
}

void Core::Engine::update_order(const Core::Order& order) {
    std::lock_guard<std::mutex> lock(this->new_orders_mutex);
    auto it = this->new_orders.find(Utils::get_symbol_exchange_market_key(
        order.symbol, order.exchange_name, order.market_type));
    if (it != this->new_orders.end()) {
        for (auto& order_it : it->second) {
            if (order_it.id == order.id) {
                order_it = order;
                break;
            }
        }
    }
}

Core::FullAccountState Core::Engine::get_full_account_state(
    long long account_id) {
    Core::FullAccountState ret;

    std::vector<Core::Trade> all_trades;
    for (const auto& strategy : this->strategies_view()) {
        const auto& trades = strategy->get_trades();
        all_trades.insert(all_trades.end(), trades.begin(), trades.end());
    }

    for (const auto& trade : all_trades) {
        if (trade.account_id == account_id) {
            ret.trades.push_back(trade);
        }
    }

    {
        std::shared_lock<std::shared_mutex> lock(this->inventory_mutex);
        for (const auto& [key, inventory] : this->inventories) {
            if (inventory.account_id == account_id) {
                ret.inventories[key] = inventory;
            }
        }
        for (const auto& [key, quote_asset] : this->quote_assets) {
            if (quote_asset.account_id == account_id) {
                ret.quote_assets[key] = quote_asset;
            }
        }
    }

    return ret;
}

std::optional<std::reference_wrapper<OnnxModel>> Core::Engine::get_model(
    int strategy_id, const std::string& model_path) {
    const std::string model_key = Utils::get_model_key(strategy_id, model_path);
    auto it = model_path_to_model_map.find(model_key);
    if (it == model_path_to_model_map.end()) {
        return std::nullopt;
    }
    return std::ref(*it->second);
}

const DigitalOptionCalibrationMap*
Core::Engine::get_digital_option_calibration_map(
    int strategy_id, const std::string& model_path) {
    const std::string model_key = Utils::get_model_key(strategy_id, model_path);
    auto it = digital_option_calibration_maps_.find(model_key);
    if (it == digital_option_calibration_maps_.end() || !it->second.loaded) {
        return nullptr;
    }
    return &it->second;
}

void Core::Engine::add_signal_log(const Core::Signal& signal,
                                  const std::string& symbol,
                                  const int strategy_id) {
    if (this->is_backtest) {
        return;
    }
    bool is_priority_log = signal.signal_strength != 0.0;
    this->create_system_log(symbol, "INFO", std::to_string(signal.prediction),
                            Core::TRADING_LOG_EVENT_TYPE_SIGNAL,
                            signal.to_json(), strategy_id, "", "",
                            is_priority_log);
}

void Core::Engine::remove_order_from_risk_engine(unsigned long long order_id) {
    this->risk_engine.remove_order(order_id);
}

void Core::Engine::register_order_in_risk_engine(const Core::Order& order) {
    this->risk_engine.register_order(order);
}

void Core::Engine::update_order_in_risk_engine(const Core::Order& order) {
    this->risk_engine.update_order(order);
}

void Core::Engine::add_trade_to_risk_engine(const Core::Trade& trade) {
    this->risk_engine.add_trade(trade);
}

void Core::Engine::remove_trade_from_risk_engine(unsigned long long trade_id) {
    this->risk_engine.remove_trade(trade_id);
}

void Core::Engine::update_trade_in_risk_engine(const Core::Trade& trade) {
    this->risk_engine.update_trade(trade);
}

void Core::Engine::remove_orders_and_trades_from_risk_engine_by_symbol(
    const std::string& symbol) {
    this->risk_engine.remove_orders_and_trades_by_symbol(symbol);
}

void Core::Engine::reconcile_risk_engine_with_tickers() {
    std::vector<Core::Order> all_live_orders;
    std::vector<Core::Trade> all_live_trades;

    for (const auto& strategy : this->strategies_view()) {
        // Use the strategy's prediction-market-aware ticker lock helper —
        // returns a held lock only when the strategy trades prediction
        // markets (where the ticker set mutates at runtime); a no-op
        // otherwise. Matches the pattern used by Engine::get_orders et al.
        auto tickers_lock = strategy->lock_on_pred_market_strat();
        for (const auto& [symbol, ticker] : strategy->trade_tickers) {
            // get_orders / get_trades each acquire ticker->trading_mutex
            // internally and return copies.
            auto ticker_orders = ticker->get_orders(/*require_lock=*/true);
            auto ticker_trades = ticker->get_trades(/*require_lock=*/true);
            all_live_orders.insert(
                all_live_orders.end(),
                std::make_move_iterator(ticker_orders.begin()),
                std::make_move_iterator(ticker_orders.end()));
            all_live_trades.insert(
                all_live_trades.end(),
                std::make_move_iterator(ticker_trades.begin()),
                std::make_move_iterator(ticker_trades.end()));
        }
    }

    this->risk_engine.rebuild_from_state(all_live_orders, all_live_trades);
}

void Core::Engine::run_periodic_strategy_cleanup() {
    if (!this->is_state_ready.load() || !this->initialized_.load()) {
        return;
    }

    const long long now_ms = this->get_time();

    constexpr long long kPeriodicStrategyCleanupIntervalMs = 3600LL * 1000;
    constexpr long long kRemovedOrderLateFillGraceMs = 3600LL * 1000;

    if (this->last_periodic_strategy_cleanup_ms_ > 0 &&
        now_ms - this->last_periodic_strategy_cleanup_ms_ <
            kPeriodicStrategyCleanupIntervalMs) {
        return;
    }
    this->last_periodic_strategy_cleanup_ms_ = now_ms;

    for (const auto& strategy : this->strategies_view()) {
        // Per-strategy periodic cleanup (custom static state, SM scratch, etc.)
        auto tickers_lock = strategy->lock_on_pred_market_strat();
        for (auto& [symbol, ticker] : strategy->trade_tickers) {
            (void)symbol;
            std::lock_guard<std::mutex> lock(ticker->trading_mutex);
            (void)ticker->purge_stale_removed_orders(
                now_ms, kRemovedOrderLateFillGraceMs);
        }
    }
}

std::tuple<Core::OrderConstructionResult, std::optional<Core::Order>>
Core::Engine::construct_order(Strategy& strategy,
                              const Core::CreateOrderParams& params) {
    if (!params.is_trade_close_order &&
        strategy.strategy_config.is_in_reduce_only) {
        return {Core::STRATEGY_IN_REDUCE_ONLY_MODE, std::nullopt};
    }
    return this->risk_engine.construct_order(*this, strategy, params);
}

mpfr::mpreal Core::Engine::get_long_ratio(bool require_risk_engine_lock) {
    const auto& long_exposure =
        this->risk_engine.get_long_exposure(require_risk_engine_lock);
    const auto& nav = this->get_nav();
    if (nav == 0) {
        return 0;
    }
    return long_exposure / nav;
}

mpfr::mpreal Core::Engine::get_short_ratio(bool require_risk_engine_lock) {
    const auto& short_exposure =
        this->risk_engine.get_short_exposure(require_risk_engine_lock);
    const auto& nav = this->get_nav();
    if (nav == 0) {
        return 0;
    }
    return short_exposure / nav;
}

mpfr::mpreal Core::Engine::get_long_ratio_by_symbol(
    const std::string& symbol, bool require_risk_engine_lock) {
    const auto& long_exposure = this->risk_engine.get_long_exposure_per_symbol(
        symbol, require_risk_engine_lock);
    const auto& nav = this->get_nav();
    if (nav == 0) {
        return 0;
    }
    return long_exposure / nav;
}

mpfr::mpreal Core::Engine::get_short_ratio_by_symbol(
    const std::string& symbol, bool require_risk_engine_lock) {
    const auto& short_exposure =
        this->risk_engine.get_short_exposure_per_symbol(
            symbol, require_risk_engine_lock);
    const auto& nav = this->get_nav();
    if (nav == 0) {
        return 0;
    }
    return short_exposure / nav;
}

void Core::Engine::update_strategy_virtual_balance(const Core::Trade& trade) {
    for (auto& strategy : this->strategies_view()) {
        if (strategy->strategy_config.id == trade.strategy_id) {
            strategy->update_virtual_balance(trade);
        }
    }
}

void Core::Engine::update_strategy_virtual_balance_revert(
    const Core::Trade& trade) {
    double pnl = trade.pnl.toDouble();
    for (auto& strategy : this->strategies_view()) {
        if (strategy->strategy_config.id == trade.strategy_id) {
            auto trade_copy = trade;
            trade_copy.pnl = -pnl;
            strategy->update_virtual_balance(trade_copy);
        }
    }
}

void Core::Engine::add_tracked_misc_order(const Core::Order& order) {
    std::lock_guard<std::mutex> lock(this->engine_state_mutex);
    const std::string& key = Utils::get_symbol_exchange_market_account_key(
        order.symbol, order.exchange_name, order.market_type, order.account_id);
    this->tracked_misc_orders[key].push_back(order);
}

void Core::Engine::remove_tracked_misc_order(const Core::Order& order) {
    std::lock_guard<std::mutex> lock(this->engine_state_mutex);
    const std::string& key = Utils::get_symbol_exchange_market_account_key(
        order.symbol, order.exchange_name, order.market_type, order.account_id);

    auto it = this->tracked_misc_orders.find(key);
    if (it != this->tracked_misc_orders.end()) {
        it->second.erase(
            std::remove(it->second.begin(), it->second.end(), order),
            it->second.end());
    }
}

std::atomic<std::chrono::steady_clock::time_point>
Core::Engine::get_last_periodic_calls_time() const {
    return this->last_periodic_calls_time.load();
}

bool Core::Engine::is_engine_trading() const { return this->running.load(); }

void Core::Engine::sync_persistent_state_to_disk() {
    for (const auto& strategy : this->strategies_view()) {
        strategy->sync_persistent_state_to_disk();
    }
}

void Core::Engine::apply_orderbook_update(const Core::OrderBookUpdate& update) {
    auto lob_it = this->order_books.find(update.symbol_full);
    if (lob_it == this->order_books.end()) {
        return;
    }
    auto& order_book = lob_it->second;
    order_book.has_received_update = true;

    bool is_contiguous =
        (update.sequence_start == order_book.last_sequence_number + 1);

    if (!is_contiguous) {
        bool is_valid_after_refresh =
            order_book.was_just_refreshed &&
            (update.sequence_start > order_book.last_sequence_number);
        if (!is_valid_after_refresh) {
            {
                std::lock_guard<std::mutex> lock(
                    this->cached_orderbook_updates_mutex);
                auto& buffer =
                    this->cached_orderbook_updates[update.symbol_full];
                buffer.push_back(update);
                if (buffer.size() > 200) {
                    buffer.erase(buffer.begin());
                }
            }
            order_book.begin_write();
            order_book.is_in_sync = false;
            order_book.ring.is_in_sync = false;  // Phase 1 mirror
            order_book.publish_bba();
            order_book.end_write();
            {
                std::lock_guard<std::mutex> lock(
                    this->order_books_out_of_sync_mutex);
                this->order_books_out_of_sync.insert(update.symbol_full);
            }
            return;
        }
    }

    bool was_out_of_sync = !order_book.is_in_sync;

    order_book.begin_write();
    // Phase 3: ring-only writer path. For each delta level we compute
    // the tick via `ring.price_to_tick`, call `ring.apply_ask/bid`
    // directly (qty=0 is a delete), and handle crossed-book eviction
    // by consulting `ring.best_bid_tick` / `ring.best_ask_tick` rather
    // than walking a std::map.
    auto updated_sec = static_cast<uint32_t>(update.timestamp / 1000);
    for (const auto& [price, quantity] : update.asks) {
        uint32_t tick = order_book.ring.price_to_tick(price);
        if (tick == 0) continue;
        (void)order_book.ring.apply_ask(tick, quantity, updated_sec);
        // Crossed-book eviction: if the new ask lands at or below the
        // current best bid, drop every bid at or above the new ask.
        // Each delete triggers find_next_best internally, so best_bid_tick
        // advances automatically.
        if (quantity > 0.0) {
            while (true) {
                uint32_t bb = order_book.ring.best_bid_tick.load(
                    std::memory_order_relaxed);
                if (bb == 0 || bb < tick) break;
                (void)order_book.ring.apply_bid(bb, 0.0, updated_sec);
            }
        }
    }
    for (const auto& [price, quantity] : update.bids) {
        uint32_t tick = order_book.ring.price_to_tick(price);
        if (tick == 0) continue;
        (void)order_book.ring.apply_bid(tick, quantity, updated_sec);
        if (quantity > 0.0) {
            while (true) {
                uint32_t ba = order_book.ring.best_ask_tick.load(
                    std::memory_order_relaxed);
                if (ba == 0 || ba > tick) break;
                (void)order_book.ring.apply_ask(ba, 0.0, updated_sec);
            }
        }
    }
    order_book.last_sequence_number = update.sequence_end;
    order_book.timestamp =
        ChronoUtils::make_utc_timepoint_from_epoch_ms(update.timestamp);
    order_book.is_in_sync = true;
    order_book.ring.is_in_sync = true;
    order_book.was_just_refreshed = false;
    order_book.publish_bba();
    order_book.end_write();

    if (was_out_of_sync) {
        std::lock_guard<std::mutex> lock(this->order_books_out_of_sync_mutex);
        this->order_books_out_of_sync.erase(update.symbol_full);
    }
}

void Core::Engine::init_orderbook(const std::string& exchange_name,
                                  const std::string& market_type,
                                  const std::string& symbol) {
    auto key = Utils::get_symbol_exchange_market_key(symbol, exchange_name,
                                                     market_type);
    // Phase 3: idempotent — always plumb tick_size, whether the book is
    // freshly inserted here or already exists (e.g. restored from a
    // state snapshot that lacked tick_size in its JSON payload).
    // Skipping the re-set on existing books was the Phase 1/2 behavior
    // and silently leaves ring-backed readers broken after a restart.
    auto [it, inserted] = this->order_books.try_emplace(key);
    double ts = this->get_tick_size(symbol, exchange_name, market_type);
    if (ts > 0.0) {
        it->second.ring.tick_size = ts;
    } else if (inserted) {
        // Only log once, on the insertion that first surfaced the issue.
        std::cerr << "[init_orderbook] get_tick_size returned 0 — "
                     "exchange_symbol_infos missing metadata for "
                  << key
                  << " (deep readers will return nullopt on this book "
                     "until metadata is loaded)\n";
    }
}

void Core::Engine::apply_full_refresh_orderbook(
    const std::string& exchange_name, const std::string& market_type,
    const std::string& symbol, const Core::OrderBookLevels& levels) {
    const std::string& key = Utils::get_symbol_exchange_market_key(
        symbol, exchange_name, market_type);
    auto it = this->order_books.find(key);
    if (it == this->order_books.end()) return;

    auto& order_book = it->second;
    order_book.begin_write();
    // Rebuild the ring from the transport levels. Preserves tick_size
    // (set at try_emplace time), iterates asks ascending and bids
    // descending so best-tracking settles on the real best.
    order_book.rebuild_ring_from_levels(levels);
    order_book.timestamp =
        ChronoUtils::make_utc_timepoint_from_epoch_ms(levels.timestamp_ms);
    order_book.last_full_refresh_timestamp = std::chrono::system_clock::now();
    order_book.last_sequence_number = levels.last_sequence_number;
    order_book.is_in_sync = true;
    order_book.ring.is_in_sync = true;
    order_book.was_just_refreshed = true;
    order_book.has_received_update = true;

    std::vector<OrderBookUpdate> buffered;
    {
        std::lock_guard<std::mutex> lock(this->cached_orderbook_updates_mutex);
        auto buf_it = this->cached_orderbook_updates.find(key);
        if (buf_it != this->cached_orderbook_updates.end()) {
            buffered = std::move(buf_it->second);
            this->cached_orderbook_updates.erase(buf_it);
        }
    }

    std::ranges::sort(buffered, [](const auto& a, const auto& b) {
        return a.sequence_end < b.sequence_end;
    });

    for (const auto& u : buffered) {
        if (u.sequence_end <= order_book.last_sequence_number) continue;
        auto u_sec = static_cast<uint32_t>(u.timestamp / 1000);
        for (const auto& [price, quantity] : u.asks) {
            uint32_t tick = order_book.ring.price_to_tick(price);
            if (tick == 0) continue;
            (void)order_book.ring.apply_ask(tick, quantity, u_sec);
        }
        for (const auto& [price, quantity] : u.bids) {
            uint32_t tick = order_book.ring.price_to_tick(price);
            if (tick == 0) continue;
            (void)order_book.ring.apply_bid(tick, quantity, u_sec);
        }
        order_book.last_sequence_number = u.sequence_end;
    }

    order_book.publish_bba();
    order_book.end_write();

    {
        std::lock_guard<std::mutex> lock(this->order_books_out_of_sync_mutex);
        this->order_books_out_of_sync.erase(key);
    }
}

void Core::Engine::apply_orderbook_levels_only(
    const std::string& exchange_name, const std::string& market_type,
    const std::string& symbol, const Core::OrderBookLevels& levels) {
    const std::string& key = Utils::get_symbol_exchange_market_key(
        symbol, exchange_name, market_type);
    auto it = this->order_books.find(key);
    if (it == this->order_books.end()) return;

    auto& order_book = it->second;
    order_book.begin_write();
    order_book.rebuild_ring_from_levels(levels);
    order_book.last_full_refresh_timestamp = std::chrono::system_clock::now();
    order_book.publish_bba();
    order_book.end_write();
}

void Core::Engine::update_bba(const std::string& exchange_name,
                              const std::string& market_type,
                              const std::string& symbol, double bid_price,
                              double bid_qty, double ask_price, double ask_qty,
                              long long timestamp_ms) {
    const auto key = Utils::get_symbol_exchange_market_key(
        symbol, exchange_name, market_type);

    std::shared_ptr<BBAState> state;

    {
        std::shared_lock<std::shared_mutex> lock(bba_mutex_);
        auto it = bba_states_.find(key);
        if (it != bba_states_.end()) {
            state = it->second;
        }
    }

    if (!state) {
        std::unique_lock<std::shared_mutex> lock(bba_mutex_);
        auto& ref = bba_states_[key];
        if (!ref) {
            ref = std::make_shared<BBAState>();
        }
        state = ref;
    }

    auto s = state->seq.load(std::memory_order_relaxed);
    state->seq.store(s + 1, std::memory_order_release);
    state->best_bid_price.store(bid_price, std::memory_order_relaxed);
    state->best_bid_qty.store(bid_qty, std::memory_order_relaxed);
    state->best_ask_price.store(ask_price, std::memory_order_relaxed);
    state->best_ask_qty.store(ask_qty, std::memory_order_relaxed);
    state->timestamp_ms.store(timestamp_ms, std::memory_order_relaxed);
    state->seq.store(s + 2, std::memory_order_release);
}

std::optional<Core::Engine::BBASnapshot> Core::Engine::get_bba(
    const std::string& symbol, const std::string& exchange_name,
    const std::string& market_type) const {
    const auto key = Utils::get_symbol_exchange_market_key(
        symbol, exchange_name, market_type);

    std::shared_ptr<BBAState> state;
    {
        std::shared_lock<std::shared_mutex> lock(bba_mutex_);
        auto it = bba_states_.find(key);
        if (it == bba_states_.end()) return std::nullopt;
        state = it->second;
    }

    BBASnapshot snap;
    uint64_t s1, s2;
    do {
        s1 = state->seq.load(std::memory_order_acquire);
        if (s1 & 1) continue;
        snap.best_bid_price =
            state->best_bid_price.load(std::memory_order_relaxed);
        snap.best_bid_qty = state->best_bid_qty.load(std::memory_order_relaxed);
        snap.best_ask_price =
            state->best_ask_price.load(std::memory_order_relaxed);
        snap.best_ask_qty = state->best_ask_qty.load(std::memory_order_relaxed);
        snap.timestamp_ms = state->timestamp_ms.load(std::memory_order_relaxed);
        s2 = state->seq.load(std::memory_order_acquire);
    } while (s1 != s2);

    if (snap.timestamp_ms == 0) return std::nullopt;
    return snap;
}

std::vector<std::string> Core::Engine::get_order_books_out_of_sync() {
    std::lock_guard<std::mutex> lock(this->order_books_out_of_sync_mutex);
    return {this->order_books_out_of_sync.begin(),
            this->order_books_out_of_sync.end()};
}

std::optional<Core::Engine::OrderbookRefreshCandidate>
Core::Engine::get_oldest_synced_orderbook() {
    std::optional<OrderbookRefreshCandidate> oldest;
    std::chrono::system_clock::time_point oldest_time =
        std::chrono::system_clock::now();

    for (const auto& [key, order_book] : this->order_books) {
        if (!order_book.is_in_sync || !order_book.has_received_update) {
            continue;
        }

        if (order_book.last_full_refresh_timestamp < oldest_time) {
            oldest_time = order_book.last_full_refresh_timestamp;

            size_t first_colon = key.find(':');
            size_t second_colon = key.find(':', first_colon + 1);
            if (first_colon == std::string::npos ||
                second_colon == std::string::npos) {
                continue;
            }

            oldest = OrderbookRefreshCandidate{
                .symbol = key.substr(0, first_colon),
                .exchange_name =
                    key.substr(first_colon + 1, second_colon - first_colon - 1),
                .market_type = key.substr(second_colon + 1)};
        }
    }

    return oldest;
}

std::optional<double> Core::Engine::get_best_ask(
    const std::string& symbol, const std::string& exchange_name,
    const std::string& market_type) const {
    const auto& key = Utils::get_symbol_exchange_market_key(
        symbol, exchange_name, market_type);
    auto it = this->order_books.find(key);
    if (it == this->order_books.end()) return std::nullopt;
    return it->second.get_best_ask();
}

std::optional<double> Core::Engine::get_best_bid(
    const std::string& symbol, const std::string& exchange_name,
    const std::string& market_type) const {
    const auto& key = Utils::get_symbol_exchange_market_key(
        symbol, exchange_name, market_type);
    auto it = this->order_books.find(key);
    if (it == this->order_books.end()) return std::nullopt;
    return it->second.get_best_bid();
}

std::optional<std::pair<double, double>> Core::Engine::get_best_ask_with_qty(
    const std::string& symbol, const std::string& exchange_name,
    const std::string& market_type) const {
    const auto& key = Utils::get_symbol_exchange_market_key(
        symbol, exchange_name, market_type);
    auto it = this->order_books.find(key);
    if (it == this->order_books.end()) return std::nullopt;
    return it->second.get_best_ask_with_qty();
}

std::optional<std::pair<double, double>> Core::Engine::get_best_bid_with_qty(
    const std::string& symbol, const std::string& exchange_name,
    const std::string& market_type) const {
    const auto& key = Utils::get_symbol_exchange_market_key(
        symbol, exchange_name, market_type);
    auto it = this->order_books.find(key);
    if (it == this->order_books.end()) return std::nullopt;
    return it->second.get_best_bid_with_qty();
}

std::optional<Core::BidAskWithQty> Core::Engine::get_best_bid_ask_with_qty(
    const std::string& symbol, const std::string& exchange_name,
    const std::string& market_type) const {
    const auto& key = Utils::get_symbol_exchange_market_key(
        symbol, exchange_name, market_type);
    auto it = this->order_books.find(key);
    if (it == this->order_books.end()) return std::nullopt;
    return it->second.get_best_bid_ask_with_qty();
}

std::optional<Core::BidAskWithQty> Core::Engine::get_best_bid_ask_with_qty(
    const std::string& symbol, const std::string& exchange_name,
    const std::string& market_type, long long max_age_ms) const {
    const auto& key = Utils::get_symbol_exchange_market_key(
        symbol, exchange_name, market_type);
    auto it = this->order_books.find(key);
    if (it == this->order_books.end()) return std::nullopt;

    const auto& order_book = it->second;
    const auto current_time_ms = this->get_time();
    const auto order_book_timestamp_ms =
        ChronoUtils::to_ms_timestamp(order_book.timestamp);
    const auto age_ms = current_time_ms - order_book_timestamp_ms;

    if (age_ms > max_age_ms) {
        return std::nullopt;
    }

    return order_book.get_best_bid_ask_with_qty();
}

std::optional<long long> Core::Engine::get_ms_since_last_orderbook_update(
    const std::string& symbol, const std::string& exchange_name,
    const std::string& market_type) const {
    const auto& key = Utils::get_symbol_exchange_market_key(
        symbol, exchange_name, market_type);
    auto it = this->order_books.find(key);
    if (it == this->order_books.end()) return std::nullopt;
    return it->second.get_ms_since_last_update();
}

std::optional<long long> Core::Engine::get_ms_since_last_orderbook_full_refresh(
    const std::string& symbol, const std::string& exchange_name,
    const std::string& market_type) const {
    const auto& key = Utils::get_symbol_exchange_market_key(
        symbol, exchange_name, market_type);
    auto it = this->order_books.find(key);
    if (it == this->order_books.end()) return std::nullopt;
    return it->second.get_ms_since_last_full_refresh();
}

std::optional<double> Core::Engine::get_spread(
    const std::string& symbol, const std::string& exchange_name,
    const std::string& market_type) const {
    const auto& key = Utils::get_symbol_exchange_market_key(
        symbol, exchange_name, market_type);
    auto it = this->order_books.find(key);
    if (it == this->order_books.end()) return std::nullopt;
    return it->second.get_spread();
}

std::optional<double> Core::Engine::get_ask_depth(
    const std::string& symbol, const std::string& exchange_name,
    const std::string& market_type, double slippage) const {
    const auto& key = Utils::get_symbol_exchange_market_key(
        symbol, exchange_name, market_type);
    auto it = this->order_books.find(key);
    if (it == this->order_books.end()) return std::nullopt;
    return it->second.get_ask_depth(slippage);
}

std::optional<double> Core::Engine::get_bid_depth(
    const std::string& symbol, const std::string& exchange_name,
    const std::string& market_type, double slippage) const {
    const auto& key = Utils::get_symbol_exchange_market_key(
        symbol, exchange_name, market_type);
    auto it = this->order_books.find(key);
    if (it == this->order_books.end()) return std::nullopt;
    return it->second.get_bid_depth(slippage);
}

std::optional<double> Core::Engine::get_avg_ask_size(
    const std::string& symbol, const std::string& exchange_name,
    const std::string& market_type, int levels) const {
    const auto& key = Utils::get_symbol_exchange_market_key(
        symbol, exchange_name, market_type);
    auto it = this->order_books.find(key);
    if (it == this->order_books.end()) return std::nullopt;
    return it->second.get_avg_ask_size(levels);
}

std::optional<double> Core::Engine::get_avg_bid_size(
    const std::string& symbol, const std::string& exchange_name,
    const std::string& market_type, int levels) const {
    const auto& key = Utils::get_symbol_exchange_market_key(
        symbol, exchange_name, market_type);
    auto it = this->order_books.find(key);
    if (it == this->order_books.end()) return std::nullopt;
    return it->second.get_avg_bid_size(levels);
}

std::optional<double> Core::Engine::get_book_volume_before_bid(
    const std::string& symbol, const std::string& exchange_name,
    const std::string& market_type, double price) const {
    const auto& key = Utils::get_symbol_exchange_market_key(
        symbol, exchange_name, market_type);
    auto it = this->order_books.find(key);
    if (it == this->order_books.end()) return std::nullopt;
    return it->second.get_book_volume_before_bid(price);
}

std::optional<double> Core::Engine::get_book_volume_before_ask(
    const std::string& symbol, const std::string& exchange_name,
    const std::string& market_type, double price) const {
    const auto& key = Utils::get_symbol_exchange_market_key(
        symbol, exchange_name, market_type);
    auto it = this->order_books.find(key);
    if (it == this->order_books.end()) return std::nullopt;
    return it->second.get_book_volume_before_ask(price);
}

std::optional<int> Core::Engine::get_number_of_levels_before_bid(
    const std::string& symbol, const std::string& exchange_name,
    const std::string& market_type, double price) const {
    const auto& key = Utils::get_symbol_exchange_market_key(
        symbol, exchange_name, market_type);
    auto it = this->order_books.find(key);
    if (it == this->order_books.end()) return std::nullopt;
    return it->second.get_number_of_levels_before_bid(price);
}

std::optional<double> Core::Engine::get_price_distance_from_mid(
    const std::string& symbol, const std::string& exchange_name,
    const std::string& market_type, double price) const {
    const auto bid_ask =
        this->get_best_bid_ask_with_qty(symbol, exchange_name, market_type);
    if (!bid_ask.has_value()) return std::nullopt;

    double mid_price = (bid_ask->bid_price + bid_ask->ask_price) / 2.0;
    if (mid_price == 0.0) return std::nullopt;

    return (price - mid_price) / mid_price;
}

bool Core::Engine::is_engine_actively_trading() const {
    return this->is_state_ready.load();
}

double Core::Engine::get_min_notional(const std::string& symbol,
                                      const std::string& exchange_name,
                                      const std::string& market_type) const {
    const auto& key = Utils::get_symbol_exchange_market_key(
        symbol, exchange_name, market_type);
    std::shared_lock<std::shared_mutex> lock(this->exchange_symbol_infos_mutex);
    auto it = this->exchange_symbol_infos.find(key);
    if (it == this->exchange_symbol_infos.end()) return 0;
    return it->second.limits.cost.min.toDouble();
}

void Core::Engine::check_trades_for_less_than_min_notional() {
    if (!this->is_state_ready.load()) {
        return;
    }

    auto now = std::chrono::steady_clock::now();

    if (now - this->last_check_trades_for_less_than_min_notional_time.load() <
        std::chrono::seconds(60 * 5)) {
        return;
    }

    this->last_check_trades_for_less_than_min_notional_time.store(now);

    const auto trades = this->get_trades();

    for (const auto& trade : trades) {
        const auto since_last_exit_fill =
            trade.get_time_since_last_exit_fill(*this);
        if (trade.get_time_since_open(*this) < 60000LL * 60LL ||
            (since_last_exit_fill.has_value() &&
             since_last_exit_fill.value() < 60000LL * 5LL)) {
            continue;
        }

        const auto min_notional = this->get_min_notional(
            trade.symbol, trade.exchange_name, trade.market_type);
        const auto price = this->get_price(trade.symbol, trade.exchange_name,
                                           trade.market_type);

        if (!price.has_value()) {
            continue;
        }

        const auto remaining_notional =
            trade.calculate_remaining_notional(price.value());
        if (remaining_notional < min_notional) {
            nlohmann::json ctx;
            ctx["remaining_notional"] = remaining_notional.toDouble();
            ctx["min_notional"] = min_notional;
            ctx["mark_price"] = price.value();

            for (const auto& strategy : this->strategies_view()) {
                if (strategy->strategy_config.id == trade.strategy_id) {
                    strategy->force_close_trade(
                        *this, trade,
                        ForceCloseReason::REMAINING_NOTIONAL_BELOW_MIN, ctx);
                    break;
                }
            }
        }
    }
}

void Core::Engine::prune_ticker_order_times() {
    if (!this->is_state_ready.load()) {
        return;
    }

    auto now = std::chrono::steady_clock::now();
    if (now - this->last_prune_ticker_order_times.load() <
        std::chrono::seconds(5)) {
        return;
    }
    this->last_prune_ticker_order_times.store(now);

    for (const auto& strategy : this->strategies_view()) {
        strategy->prune_recent_order_times();
        auto tickers_lock = strategy->lock_on_pred_market_strat();
        for (auto& [symbol, ticker] : strategy->trade_tickers) {
            if (ticker->order_count.load(std::memory_order_relaxed) == 0) {
                continue;
            }
            std::lock_guard<std::mutex> lock(ticker->trading_mutex);
            ticker->prune_recent_order_times();
        }
    }
}

void Core::Engine::recalculate_ticker_order_book_state() {
    if (!this->is_state_ready.load()) {
        return;
    }

    auto now = std::chrono::steady_clock::now();
    if (now - this->last_recalculate_ticker_order_book_state_time.load() <
        std::chrono::seconds(60 * 5)) {
        return;
    }
    this->last_recalculate_ticker_order_book_state_time.store(now);

    for (const auto& strategy : this->strategies_view()) {
        auto tickers_lock = strategy->lock_on_pred_market_strat();
        for (auto& [symbol, ticker] : strategy->trade_tickers) {
            ticker->recalculate_order_book_state();
        }
    }
}

// =============================================================================
// Polymarket Event Store
// =============================================================================

void Core::Engine::add_polymarket_event(
    std::shared_ptr<PolymarketActiveEvent> event) {
    std::unique_lock<std::shared_mutex> lock(polymarket_events_mutex_);
    polymarket_events_[event->condition_id] = std::move(event);
}

void Core::Engine::remove_polymarket_event(const std::string& condition_id) {
    std::unique_lock<std::shared_mutex> lock(polymarket_events_mutex_);
    polymarket_events_.erase(condition_id);
}

std::optional<std::shared_ptr<Core::PolymarketActiveEvent>>
Core::Engine::get_polymarket_event(const std::string& condition_id) const {
    std::shared_lock<std::shared_mutex> lock(polymarket_events_mutex_);
    auto it = polymarket_events_.find(condition_id);
    if (it != polymarket_events_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::optional<std::shared_ptr<Core::PolymarketActiveEvent>>
Core::Engine::get_active_polymarket_event(const std::string& series_slug,
                                          long long current_time_ms) const {
    std::shared_lock<std::shared_mutex> lock(polymarket_events_mutex_);
    for (const auto& [_, event] : polymarket_events_) {
        if (event->series_slug == series_slug &&
            event->event_start_time_ms <= current_time_ms &&
            current_time_ms <= event->event_end_time_ms) {
            return event;
        }
    }
    return std::nullopt;
}

void Core::Engine::update_polymarket_event_bba(const std::string& condition_id,
                                               int outcome_index,
                                               double best_bid, double best_ask,
                                               long long ts) {
    std::shared_lock<std::shared_mutex> lock(polymarket_events_mutex_);
    auto it = polymarket_events_.find(condition_id);
    if (it == polymarket_events_.end()) return;
    auto& event = it->second;
    if (outcome_index < 0 || outcome_index >= MAX_POLYMARKET_EVENT_OUTCOMES)
        return;
    event->outcome_state[outcome_index].best_bid.store(best_bid);
    event->outcome_state[outcome_index].best_ask.store(best_ask);
    event->outcome_state[outcome_index].bba_timestamp_ms.store(ts);
}

void Core::Engine::update_polymarket_event_last_trade(
    const std::string& condition_id, int outcome_index, double price,
    long long ts) {
    std::shared_lock<std::shared_mutex> lock(polymarket_events_mutex_);
    auto it = polymarket_events_.find(condition_id);
    if (it == polymarket_events_.end()) return;
    auto& event = it->second;
    if (outcome_index < 0 || outcome_index >= MAX_POLYMARKET_EVENT_OUTCOMES)
        return;
    event->outcome_state[outcome_index].last_trade_price.store(price);
    event->outcome_state[outcome_index].last_trade_timestamp_ms.store(ts);
}

// Polymarket L2 order books

static std::string polymarket_book_key(const std::string& asset_id) {
    return asset_id + ":polymarket:spot";
}

// Polymarket uses a fixed $0.001 minimum tick across all markets. Hardcoded
// here so Polymarket books use the same ring-backed path as exchange books,
// eliminating the map-fallback branch that existed in Phases 1-2.
static constexpr double kPolymarketTickSize = 0.001;

void Core::Engine::init_polymarket_orderbook(const std::string& asset_id) {
    auto key = polymarket_book_key(asset_id);
    // Polymarket markets have a bounded [$0, $1] price range at a
    // minimum $0.001 tick, which fits in 1000 ticks. Use window_bits=10
    // (1024 slots, ~32 KB virtual per side) instead of the exchange
    // default of 19 bits (~16 MB per side). At the scale of thousands
    // of simultaneous polymarket markets this is the difference between
    // ~300 MB and ~320 GB of virtual address space.
    //
    // Idempotent: we always set tick_size afterwards. If the book was
    // somehow created earlier with a different window_bits, we'd need
    // to fail loud — but state snapshot restore path already respects
    // the embedded window_bits, so the sizes stay consistent.
    auto [it, inserted] =
        this->order_books.try_emplace(key, Core::RingWindow::kPolymarket);
    it->second.ring.tick_size = kPolymarketTickSize;
    (void)inserted;
}

void Core::Engine::apply_polymarket_l2_snapshot(
    const std::string& asset_id, const Core::OrderBookLevels& levels) {
    auto key = polymarket_book_key(asset_id);
    auto it = this->order_books.find(key);
    if (it == this->order_books.end()) {
        auto [new_it, inserted] =
            this->order_books.try_emplace(key, Core::RingWindow::kPolymarket);
        if (inserted) {
            new_it->second.ring.tick_size = kPolymarketTickSize;
        }
        it = new_it;
    }

    auto& ob = it->second;
    ob.begin_write();
    ob.rebuild_ring_from_levels(levels);
    ob.timestamp = std::chrono::system_clock::now();
    ob.last_full_refresh_timestamp = ob.timestamp;
    ob.last_sequence_number = levels.last_sequence_number;
    ob.is_in_sync = true;
    ob.ring.is_in_sync = true;
    ob.has_received_update = true;
    ob.publish_bba();
    ob.end_write();
}

void Core::Engine::apply_polymarket_l2_delta(const std::string& asset_id,
                                             double price, double size,
                                             bool is_bid,
                                             long long timestamp_ms) {
    auto key = polymarket_book_key(asset_id);
    auto it = this->order_books.find(key);
    if (it == this->order_books.end()) {
        auto [new_it, inserted] =
            this->order_books.try_emplace(key, Core::RingWindow::kPolymarket);
        if (inserted) {
            new_it->second.ring.tick_size = kPolymarketTickSize;
        }
        it = new_it;
    }

    auto& ob = it->second;
    ob.begin_write();
    uint32_t tick = ob.ring.price_to_tick(price);
    if (tick != 0) {
        auto updated_sec = static_cast<uint32_t>(timestamp_ms / 1000);
        if (is_bid) {
            (void)ob.ring.apply_bid(tick, size > 0 ? size : 0.0, updated_sec);
        } else {
            (void)ob.ring.apply_ask(tick, size > 0 ? size : 0.0, updated_sec);
        }
    }
    ob.timestamp = std::chrono::system_clock::now();
    ob.is_in_sync = true;
    ob.ring.is_in_sync = true;
    ob.has_received_update = true;
    ob.publish_bba();
    ob.end_write();
}

const Core::OrderBook* Core::Engine::get_polymarket_orderbook(
    const std::string& asset_id) const {
    auto key = polymarket_book_key(asset_id);
    auto it = this->order_books.find(key);
    if (it == this->order_books.end()) return nullptr;
    return &it->second;
}

void Core::Engine::register_markout(const OrderFillInfo& fill_info) {
    auto now = std::chrono::steady_clock::now();
    auto duration =
        std::chrono::milliseconds(fill_info.markout_config->duration_ms);
    auto interval =
        std::chrono::milliseconds(fill_info.markout_config->interval_ms);

    MarkoutJob job;
    job.order_fill_id = fill_info.order_fill_id;
    job.order_id = fill_info.order_id;
    job.strategy_id = fill_info.strategy_id;
    job.symbol = fill_info.symbol;
    job.exchange_name = fill_info.exchange_name;
    job.market_type = fill_info.market_type;
    job.fill_price = fill_info.price.toDouble();
    job.fill_side = fill_info.side;
    job.fill_timestamp_ms = fill_info.order_fill_timestamp;
    job.duration_ms = fill_info.markout_config->duration_ms;
    job.interval_ms = fill_info.markout_config->interval_ms;
    job.next_snapshot_at = now + interval;
    job.expires_at = now + duration;
    job.fill_time_kv_cache = fill_info.kv_cache;

    std::lock_guard<std::mutex> lock(this->markout_mutex_);
    this->pending_markouts_.push_back(std::move(job));
}

void Core::Engine::process_markouts() {
    std::lock_guard<std::mutex> lock(this->markout_mutex_);
    if (this->pending_markouts_.empty()) return;

    auto now = std::chrono::steady_clock::now();

    for (auto& job : this->pending_markouts_) {
        if (now < job.next_snapshot_at) continue;
        if (job.fill_price <= 0.0) continue;

        auto bba =
            this->get_bba(job.symbol, job.exchange_name, job.market_type);
        if (bba.has_value()) {
            double mid = (bba->best_bid_price + bba->best_ask_price) / 2.0;
            if (mid > 0.0) {
                auto registered_at =
                    job.expires_at - std::chrono::milliseconds(job.duration_ms);
                auto elapsed =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - registered_at);

                double markout_bps;
                if (job.fill_side == OrderSide::BUY) {
                    markout_bps =
                        (mid - job.fill_price) / job.fill_price * 10000.0;
                } else {
                    markout_bps =
                        (job.fill_price - mid) / job.fill_price * 10000.0;
                }

                MarkoutSnapshot snapshot;
                snapshot.timestamp_ms = DatetimeUtils::get_current_time_ms();
                snapshot.elapsed_ms = elapsed.count();
                snapshot.mid_price = mid;
                snapshot.markout_bps = markout_bps;
                job.snapshots.push_back(snapshot);
            }
        }

        // Skip to current interval rather than catching up on missed ones
        auto interval = std::chrono::milliseconds(job.interval_ms);
        if (now - job.next_snapshot_at >= interval) {
            auto missed = (now - job.next_snapshot_at) / interval;
            job.next_snapshot_at += interval * (missed + 1);
        } else {
            job.next_snapshot_at += interval;
        }
    }

    // Partition: move expired jobs out
    auto it = std::partition(
        this->pending_markouts_.begin(), this->pending_markouts_.end(),
        [&now](const MarkoutJob& job) { return now < job.expires_at; });

    if (it != this->pending_markouts_.end()) {
        std::lock_guard<std::mutex> completed_lock(
            this->completed_markouts_mutex_);
        for (auto move_it = it; move_it != this->pending_markouts_.end();
             ++move_it) {
            CompletedMarkout cm;
            cm.order_fill_id = move_it->order_fill_id;
            cm.order_id = move_it->order_id;
            cm.strategy_id = move_it->strategy_id;
            cm.symbol = std::move(move_it->symbol);
            cm.exchange_name = std::move(move_it->exchange_name);
            cm.market_type = std::move(move_it->market_type);
            cm.fill_price = move_it->fill_price;
            cm.fill_side = move_it->fill_side;
            cm.fill_timestamp_ms = move_it->fill_timestamp_ms;
            cm.duration_ms = move_it->duration_ms;
            cm.interval_ms = move_it->interval_ms;
            cm.snapshots = std::move(move_it->snapshots);
            cm.fill_time_kv_cache = std::move(move_it->fill_time_kv_cache);
            this->completed_markouts_.push_back(std::move(cm));
        }
        this->pending_markouts_.erase(it, this->pending_markouts_.end());
    }
}

// ---------------------------------------------------------------------------
// Observability layer — see ObservabilityBatch in engine.h and
// Diagnostic snapshot helpers for engine observability.
// stack. Emit tick runs on the periodic_calls thread every ~10s; strategy +
// SM health are included every 3rd tick (~30s cadence).
// ---------------------------------------------------------------------------

void Core::Engine::emit_observability_tick(bool is_shutdown) {
    if (!this->observability_callback_.has_value()) return;
    if (this->is_backtest) return;
    if (!is_shutdown && !this->is_state_ready.load()) return;

    // Cooldown: one tick per 10s unless this is the shutdown flush.
    auto now = std::chrono::steady_clock::now();
    if (!is_shutdown) {
        if (this->last_observability_tick_time_ !=
            std::chrono::steady_clock::time_point::min()) {
            auto since_last = std::chrono::duration_cast<std::chrono::seconds>(
                                  now - this->last_observability_tick_time_)
                                  .count();
            if (since_last < 10) return;
        }
        this->last_observability_tick_time_ = now;
        this->observability_tick_count_++;
    }

    ObservabilityBatch batch;
    batch.engine_instance = this->engine_instance_;
    const int64_t ts_ms = DatetimeUtils::get_current_time_ms();

    // Engine health — always included.
    batch.engine = this->build_engine_health_snapshot(is_shutdown);

    // Metric samples — latency histograms + engine-wide counters.
    this->sample_latency_histograms(batch.metrics, ts_ms);
    {
        EngineMetricSample s;
        s.ts_ms = ts_ms;
        s.metric_name = "engine.market_events_queue.size";
        s.value = static_cast<double>(this->market_events.size());
        batch.metrics.push_back(s);
    }
    {
        EngineMetricSample s;
        s.ts_ms = ts_ms;
        s.metric_name = "engine.trading_logs_queue.size";
        s.value = static_cast<double>(this->trading_logs.size_approx());
        batch.metrics.push_back(s);
    }
    {
        EngineMetricSample s;
        s.ts_ms = ts_ms;
        s.metric_name = "engine.mem_rss.bytes";
        s.value = static_cast<double>(LinuxProcess::read_rss_bytes());
        batch.metrics.push_back(s);
    }
    {
        EngineMetricSample s;
        s.ts_ms = ts_ms;
        s.metric_name = "engine.market_events.total";
        s.value = static_cast<double>(this->num_market_events_processed.load());
        batch.metrics.push_back(s);
    }

    // Strategy + SM health: every 3rd tick (~30s) or on shutdown.
    const bool include_slow =
        is_shutdown || (this->observability_tick_count_ % 3 == 1);
    if (include_slow) {
        for (auto& strategy : this->strategies_view()) {
            if (strategy->is_disabled_now()) continue;
            batch.strategies.push_back(
                this->build_strategy_health_snapshot(*strategy));
        }
        for (auto& sm : ::MarketData::StateMachineDispatcher::get_instance()
                            .get_machines()) {
            if (!sm) continue;
            batch.state_machines.push_back(this->build_sm_health_snapshot(*sm));
        }
    }

    // Meta-metric: self-monitoring. The callback's wall-clock cost is
    // captured by live_infra after the callback returns; engine-side this is
    // just the build cost.
    const int64_t build_end_ms = DatetimeUtils::get_current_time_ms();
    {
        EngineMetricSample s;
        s.ts_ms = ts_ms;
        s.metric_name = "observability.build_latency_ms";
        s.value = static_cast<double>(build_end_ms - ts_ms);
        batch.metrics.push_back(s);
    }

    // The callback connects to a remote DB and runs a transaction, so it runs
    // on the pool: the periodic loop must never wait on it.
    auto deliver = [this](const ObservabilityBatch& batch) {
        try {
            (*this->observability_callback_)(batch);
        } catch (const std::exception& e) {
            // Pin a `snap_before` so an operator investigating this
            // CALLBACK_FAILURE row can reconstruct engine state at the moment
            // it fired. The 5-minute throttle is bypassed; downstream archiver
            // dedups. Called BEFORE the log emit so the snapshot's
            // event_link_id range closes just before the failure's row —
            // LATERAL joins in v_log_with_snapshot then map this log's
            // snap_before to the flush we just did.
            this->emit_engine_state_snapshot(true);
            nlohmann::json md;
            md["callback_name"] = "observability_callback";
            md["exception_what"] = e.what();
            md["retry_scheduled"] = false;
            md["snapshot_before_flushed"] = true;
            this->create_system_log(
                "", LOG_LEVEL_ERROR, "observability callback threw; tick dropped",
                TRADING_LOG_EVENT_TYPE_CALLBACK_FAILURE, md, -1, "", "");
            // Flush `snap_after` so the full before/after bracket is available
            // without waiting up to 5 minutes for the next periodic snapshot.
            this->emit_engine_state_snapshot(true);
        }
    };
    // The shutdown tick must reach the DB before the process exits.
    if (is_shutdown) {
        deliver(batch);
        return;
    }
    // One delivery at a time. A tick that finds the last one still running is
    // dropped; the next carries fresher data.
    if (this->observability_delivery_in_flight_.exchange(true)) return;
    this->periodic_tasks_thread_pool->enqueue(
        [this, deliver, batch = std::move(batch)]() {
            struct ClearInFlight {
                std::atomic<bool>& flag;
                ~ClearInFlight() { flag.store(false); }
            } clear{this->observability_delivery_in_flight_};
            deliver(batch);
        });
}

Core::EngineHealthSnapshot Core::Engine::build_engine_health_snapshot(
    bool is_shutdown) {
    EngineHealthSnapshot h;
    h.last_startup_ts_ms = this->last_startup_ts_ms_;
    h.last_heartbeat_ts_ms = DatetimeUtils::get_current_time_ms();
    h.is_running = !is_shutdown && this->running.load();
    h.market_events_queue_size = static_cast<int>(this->market_events.size());
    h.trading_logs_queue_size =
        static_cast<int>(this->trading_logs.size_approx());
    h.mem_rss_bytes = LinuxProcess::read_rss_bytes();
    h.total_market_events = this->num_market_events_processed.load();
    if (is_shutdown) {
        h.last_shutdown_ts_ms = DatetimeUtils::get_current_time_ms();
        h.last_shutdown_reason = std::string("gracefully_stop_called");
    }

    // Backpressure alarm: emit once per crossing above MARKET_EVENTS_QUEUE_
    // BACKPRESSURE_THRESHOLD. Rate-limited to one emit per minute per process
    // so a sustained overload doesn't flood the log — the trend is visible
    // in engine_metric, the log row is just the event-linked alarm.
    constexpr int BACKPRESSURE_THRESHOLD = 10000;
    constexpr int64_t BACKPRESSURE_COOLDOWN_MS = 60000;
    if (h.market_events_queue_size > BACKPRESSURE_THRESHOLD && !is_shutdown) {
        const int64_t now_ms = DatetimeUtils::get_current_time_ms();
        if (now_ms - this->last_backpressure_emit_ms_.load(
                         std::memory_order_acquire) >=
            BACKPRESSURE_COOLDOWN_MS) {
            this->last_backpressure_emit_ms_.store(now_ms,
                                                   std::memory_order_release);
            this->create_system_log(
                "", LOG_LEVEL_WARNING, "market-events queue backpressure",
                TRADING_LOG_EVENT_TYPE_MARKET_EVENTS_QUEUE_BACKPRESSURE,
                nlohmann::json{
                    {"queue_depth", h.market_events_queue_size},
                    {"threshold", BACKPRESSURE_THRESHOLD},
                    {"trading_logs_queue_size", h.trading_logs_queue_size},
                    {"total_market_events_processed", h.total_market_events},
                },
                -1);
        }
    }

    // Unacked-order sweep: emit once/min for every order that's been waiting
    // for creation-ack beyond UNACKED_ORDER_AGE_THRESHOLD_MS. The normal ack
    // round-trip is 10-100ms; anything over 5s is pathological (lost HTTP,
    // proxy stall, venue degraded) and deserves operator visibility even
    // before the order is reconciled. Per-order detail lives in metadata;
    // the sweep is bounded by the number of unacked orders.
    constexpr int64_t UNACKED_SWEEP_COOLDOWN_MS = 60000;
    constexpr int64_t UNACKED_ORDER_AGE_THRESHOLD_MS = 5000;
    if (!is_shutdown) {
        const int64_t now_ms = DatetimeUtils::get_current_time_ms();
        if (now_ms - this->last_unacked_timeout_sweep_ms_.load(
                         std::memory_order_acquire) >=
            UNACKED_SWEEP_COOLDOWN_MS) {
            this->last_unacked_timeout_sweep_ms_.store(
                now_ms, std::memory_order_release);
            for (const auto& strategy : this->strategies_view()) {
                const auto orders = strategy->get_orders();
                for (const auto& order : orders) {
                    if (order.is_order_creation_acknowledged) continue;
                    const int64_t age_ms = now_ms - order.create_timestamp;
                    if (age_ms < UNACKED_ORDER_AGE_THRESHOLD_MS) continue;
                    this->create_system_log(
                        order.symbol, LOG_LEVEL_WARNING,
                        "order unacked past threshold",
                        TRADING_LOG_EVENT_TYPE_UNACKED_ORDER_TIMEOUT,
                        nlohmann::json{
                            {"order_id", order.id},
                            {"id_str", order.id_str},
                            {"exchange_order_id", order.exchange_order_id},
                            {"strategy_id", order.strategy_id},
                            {"symbol", order.symbol},
                            {"exchange_name", order.exchange_name},
                            {"market_type", order.market_type},
                            {"side", order.side == Core::OrderSide::BUY
                                         ? "BUY"
                                         : "SELL"},
                            {"create_timestamp_ms", order.create_timestamp},
                            {"age_ms", age_ms},
                            {"threshold_ms", UNACKED_ORDER_AGE_THRESHOLD_MS},
                        },
                        order.strategy_id, order.exchange_name,
                        order.market_type);
                }
            }
        }
    }
#ifdef BUILD_COMMIT
    h.build_commit = std::string(BUILD_COMMIT);
#else
    h.build_commit = "unknown";
#endif
    return h;
}

Core::SmHealthSnapshot Core::Engine::build_sm_health_snapshot(
    const ::MarketData::StrategyStateMachine& sm) {
    SmHealthSnapshot s;
    s.sm_name = sm.get_name();
    s.has_loaded_state = sm.has_loaded_state();
    s.is_live_mode = sm.is_live_mode();
    s.is_fully_synced = sm.is_fully_synced();
    auto progress = sm.get_sync_progress();
    s.sync_progress = {
        {"current", progress.first},
        {"total", progress.second},
    };
    long long min_ts = sm.get_min_synced_ts();
    if (min_ts > 0) s.min_synced_ts = static_cast<int64_t>(min_ts);
    long long latest = sm.get_latest_event_ts_ms();
    if (latest > 0) s.latest_event_ts = static_cast<int64_t>(latest);
    s.dependencies = nlohmann::json::array();  // TODO: serialize deps
    // diagnostic_json: bounded soft-cap 10KB. Oversize drops to null + warn.
    nlohmann::json d = sm.to_diagnostic_json();
    std::string dump = d.dump();
    constexpr size_t kMaxDiagBytes = 10 * 1024;
    if (dump.size() > kMaxDiagBytes) {
        s.diagnostic_json = nullptr;
    } else {
        s.diagnostic_json = std::move(d);
    }
    return s;
}

Core::StrategyHealthSnapshot Core::Engine::build_strategy_health_snapshot(
    Strategy& strategy) {
    StrategyHealthSnapshot h;
    h.strategy_id = strategy.strategy_config.id;
    h.last_heartbeat_ts_ms = DatetimeUtils::get_current_time_ms();
    h.is_paused =
        strategy.stop_making_markets.load() || strategy.is_disabled_now();
    const auto known = [](int64_t ts) -> std::optional<int64_t> {
        if (ts == 0) return std::nullopt;
        return ts;
    };
    h.last_prediction_ts_ms = known(strategy.last_prediction_ts_ms.load());
    h.last_order_ts_ms = known(strategy.last_order_ts_ms.load());
    h.open_trade_count = 0;
    {
        auto lock = strategy.lock_on_pred_market_strat();
        for (auto& kv : strategy.trade_tickers) {
            // TradeTicker::active_trade_count is a std::atomic<int> member,
            // not a getter method. Load it with explicit memory_order.
            h.open_trade_count +=
                kv.second->active_trade_count.load(std::memory_order_relaxed);
        }
    }
    // last_fill_ts / last_market_event_ts: steady_clock inside Strategy.
    // Approximate with heartbeat ts for now; wired up properly as follow-on.
    h.last_market_event_ts_ms = h.last_heartbeat_ts_ms;
    h.inventory_summary = nlohmann::json::object();

    // SM deps: primary SM (if present) + all watched SMs.
    h.sm_dependencies = nlohmann::json::array();
    h.sm_health_summary = nlohmann::json::array();
    auto add_sm =
        [&](const std::shared_ptr<::MarketData::StrategyStateMachine>& sm_ptr) {
            if (!sm_ptr) return;
            h.sm_dependencies.push_back(sm_ptr->get_name());
            auto sm_health = this->build_sm_health_snapshot(*sm_ptr);
            h.sm_health_summary.push_back({
                {"sm_name", sm_health.sm_name},
                {"has_loaded_state", sm_health.has_loaded_state},
                {"is_live_mode", sm_health.is_live_mode},
                {"is_fully_synced", sm_health.is_fully_synced},
                {"sync_progress", sm_health.sync_progress},
                {"latest_event_ts",
                 sm_health.latest_event_ts.has_value()
                     ? nlohmann::json(*sm_health.latest_event_ts)
                     : nlohmann::json(nullptr)},
            });
        };
    add_sm(strategy.strategy_event_state_machine);
    for (auto& w : strategy.watched_state_machines) {
        add_sm(w.sm);
    }

    // Period counts: read + zero the atomics.
    h.period_counts = {
        {"decisions",
         strategy.period_decisions.exchange(0, std::memory_order_acq_rel)},
        {"orders_placed",
         strategy.period_orders_placed.exchange(0, std::memory_order_acq_rel)},
        {"orders_cancelled", strategy.period_orders_cancelled.exchange(
                                 0, std::memory_order_acq_rel)},
    };

    return h;
}

void Core::Engine::sample_latency_histograms(
    std::vector<EngineMetricSample>& out, int64_t ts_ms) {
    auto emit = [&](const std::string& prefix, nlohmann::json&& snap) {
        auto push = [&](const std::string& suffix, double val) {
            EngineMetricSample s;
            s.ts_ms = ts_ms;
            s.metric_name = prefix + suffix;
            s.value = val;
            out.push_back(std::move(s));
        };
        push(".count", static_cast<double>(snap.value("count", 0ULL)));
        push(".p50", static_cast<double>(snap.value("p50", 0LL)));
        push(".p95", static_cast<double>(snap.value("p95", 0LL)));
        push(".p99", static_cast<double>(snap.value("p99", 0LL)));
        push(".max", static_cast<double>(snap.value("max", 0LL)));
    };
    emit("engine.latency.order_sent_to_ack_ns",
         this->latency_order_sent_to_ack_.snapshot_and_reset());
    emit("engine.latency.fill_to_ack_ns",
         this->latency_fill_to_ack_.snapshot_and_reset());
}
