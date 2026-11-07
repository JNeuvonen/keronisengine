#pragma once

#include <array>
#include <shared_mutex>
#include <tuple>
#include <unordered_set>

#include "common_types/trading_log.h"
#include "concurrent_queue_include_helper.h"
#include "exclusive_periodic_job.h"
#include "market_making/constants.h"
#include "market_making/dtypes/def_only_structs.h"
#include "market_making/dtypes/engine_signal.h"
#include "market_making/dtypes/engine_state_snapshot.h"
#include "market_making/dtypes/full_account_state.h"
#include "market_making/dtypes/market_event.h"
#include "market_making/dtypes/orderbook.h"
#include "market_making/dtypes/risk_engine.h"
#include "market_making/dtypes/strategy_telemetry.h"
#include "market_making/dtypes/trade.h"
#include "market_making/latency_histogram.h"
#include "market_making/strategy.h"
#include "threadpool.h"

class OnnxModel;

#include "digital_option_calibration_map.h"

namespace Core {

class Strategy;
struct Order;

static constexpr int MAX_POLYMARKET_EVENT_OUTCOMES = 10;

struct PolymarketActiveEvent {
    std::string condition_id;
    std::string series_slug;
    std::string slug;
    long long event_start_time_ms = 0;
    long long event_end_time_ms = 0;
    int num_outcomes = 0;
    std::vector<std::string> outcome_names;
    std::vector<std::string> clob_token_ids;

    struct OutcomeState {
        // Live BBA from polymarket stream
        std::atomic<double> best_bid{0.0};
        std::atomic<double> best_ask{0.0};
        std::atomic<long long> bba_timestamp_ms{0};

        // Last trade price (used as implied probability in backtest)
        std::atomic<double> last_trade_price{0.0};
        std::atomic<long long> last_trade_timestamp_ms{0};
    };
    std::array<OutcomeState, MAX_POLYMARKET_EVENT_OUTCOMES> outcome_state;

    // Raw BBA access (live only)
    std::optional<std::pair<double, double>> get_outcome_bba(
        const std::string& outcome_name) const {
        for (int i = 0; i < num_outcomes; ++i) {
            if (outcome_names[i] == outcome_name) {
                return {{outcome_state[i].best_bid.load(),
                         outcome_state[i].best_ask.load()}};
            }
        }
        return std::nullopt;
    }

    // Returns implied probability for an outcome.
    // Live: BBA midpoint. Backtest: last trade price.
    std::optional<double> get_outcome_implied_probability(
        const std::string& outcome_name, bool is_backtest) const {
        for (int i = 0; i < num_outcomes; ++i) {
            if (outcome_names[i] != outcome_name) continue;

            if (is_backtest) {
                double p = outcome_state[i].last_trade_price.load();
                if (p <= 0.0) return std::nullopt;
                return p;
            }

            double bid = outcome_state[i].best_bid.load();
            double ask = outcome_state[i].best_ask.load();
            if (bid <= 0.0 && ask <= 0.0) return std::nullopt;
            return (bid + ask) / 2.0;
        }
        return std::nullopt;
    }
};

}  // namespace Core

namespace Core {

struct MarkoutJob {
    unsigned long long order_fill_id;
    unsigned long long order_id;
    int strategy_id;
    std::string symbol;
    std::string exchange_name;
    std::string market_type;
    double fill_price;
    OrderSide fill_side;
    long long fill_timestamp_ms;
    long long duration_ms;
    long long interval_ms;
    std::chrono::steady_clock::time_point next_snapshot_at;
    std::chrono::steady_clock::time_point expires_at;
    std::vector<MarkoutSnapshot> snapshots;
    std::unordered_map<std::string, double> fill_time_kv_cache;
};

using TradingLogCallback = std::function<void(const std::vector<TradingLog>&)>;
using EngineStateSnapshotCallback =
    std::function<void(const EngineStateSnapshot&)>;

struct FeeModel {
    mpfr::mpreal taker_fee_rate;
    mpfr::mpreal maker_fee_rate;
};

struct ManuallyRemovedTrade {
    long long db_id;
    std::string symbol;
    int strategy_id;
    unsigned long long trade_id;
};

struct RealtimeDataSubscription {
    int id;
    std::string data_type;
    std::string symbol;
    std::string exchange_name;
    std::string market_type;
    bool is_active;
};

using FetchRealtimeSubscriptionsCallback =
    std::function<std::vector<RealtimeDataSubscription>()>;

// ---------------------------------------------------------------------------
// Observability layer DTOs — emitted in batches by the engine's heartbeat
// tick, consumed by live_infra/state_updater.cpp which UPSERTs them into the
// engine_metric / engine_health / strategy_health / state_machine_health
// tables. See agentic_system/skills/ENGINE_DIAGNOSTICS.md for the four-layer
// diagnostic stack.
// ---------------------------------------------------------------------------

// One metric sample. Corresponds to one row in engine_metric.
// metric_name must come from the closed set validated at emit time.
struct EngineMetricSample {
    int64_t ts_ms;
    std::string metric_name;
    double value;
    std::optional<int> strategy_id;     // null if engine-wide
    std::optional<std::string> symbol;  // null if not symbol-scoped
    nlohmann::json labels;              // may be empty object
};

// Snapshot of engine-wide health at the tick. UPSERTed into engine_health.
struct EngineHealthSnapshot {
    int64_t last_startup_ts_ms;
    int64_t last_heartbeat_ts_ms;
    bool is_running;
    int market_events_queue_size;
    int trading_logs_queue_size;
    int64_t mem_rss_bytes;
    int64_t total_market_events;
    std::optional<int64_t> last_shutdown_ts_ms;
    std::optional<std::string> last_shutdown_reason;
    std::string build_commit;
};

// Per-SM health snapshot — one row per registered SM per tick. UPSERTed into
// state_machine_health. All fields sourced from StrategyStateMachine base
// class getters + optional to_diagnostic_json() override.
struct SmHealthSnapshot {
    std::string sm_name;
    bool has_loaded_state;
    bool is_live_mode;
    bool is_fully_synced;
    nlohmann::json sync_progress;  // {"current": N, "total": M}
    std::optional<int64_t> min_synced_ts;
    std::optional<int64_t> latest_event_ts;
    nlohmann::json dependencies;     // from get_market_data_dependencies()
    nlohmann::json diagnostic_json;  // from to_diagnostic_json(); may be null
};

// Per-strategy health snapshot — one row per active strategy per tick.
// UPSERTed into strategy_health. sm_health_summary is denormalized: the
// per-watched-SM view so agents get both strategy liveness and its SM deps'
// state in one row read.
struct StrategyHealthSnapshot {
    int strategy_id;
    int64_t last_heartbeat_ts_ms;
    bool is_paused;
    int open_trade_count;
    std::optional<int64_t> last_fill_ts_ms;
    int64_t last_market_event_ts_ms;
    nlohmann::json inventory_summary;
    nlohmann::json sm_dependencies;    // ["sm_name1", ...] — canonical names
    nlohmann::json sm_health_summary;  // [SmHealthSnapshot JSON, ...]
    nlohmann::json
        period_counts;  // {decisions, orders_placed, orders_cancelled}
    std::optional<int64_t> last_prediction_ts_ms;
    std::optional<int64_t> last_order_ts_ms;
};

// One batch per emit tick (every 10s). Strategies + state_machines are
// included every 3rd tick (30s cadence); metrics + engine snapshot every
// tick. The batch is invoked as a single callback so live_infra can UPSERT
// all four tables in one DB transaction.
struct ObservabilityBatch {
    std::string engine_instance;
    std::vector<EngineMetricSample> metrics;
    std::optional<EngineHealthSnapshot> engine;
    std::vector<StrategyHealthSnapshot> strategies;
    std::vector<SmHealthSnapshot> state_machines;
};

using ObservabilityCallback = std::function<void(const ObservabilityBatch&)>;

struct EngineConfig {
    bool is_backtest;
    bool is_strategy_development;
    bool emit_trading_logs;
    bool emit_logs_in_real_time = false;
    std::optional<std::function<void(const TradingLog&)>>
        publish_trading_log_callback = std::nullopt;
    bool emit_realtime_trading_state = false;
    std::optional<std::function<void(const Core::Engine& engine)>>
        publish_realtime_trading_state_callback = std::nullopt;
    long long seconds_between_realtime_trading_state_updates = 5;
    std::unordered_map<std::string, FeeModel> fees_by_exchange_and_market;
    bool enable_engine_signals = false;
    std::optional<FetchEngineSignalsCallback> fetch_engine_signals_callback =
        std::nullopt;
    std::optional<AckEngineSignalCallback> ack_engine_signal_callback =
        std::nullopt;
    bool enable_realtime_subscriptions = false;
    std::optional<FetchRealtimeSubscriptionsCallback>
        fetch_realtime_subscriptions_callback = std::nullopt;
    long long seconds_between_subscription_polls = 10;
    bool is_order_creation_disabled = false;
    bool enable_strategy_telemetry = false;
    std::optional<std::function<void(const std::string&)>>
        publish_strategy_telemetry_callback = std::nullopt;

    // Stable identity of this engine process for observability tables
    // (engine_health, strategy_health PK, engine_metric.engine_instance,
    // engine_snapshot_index.engine_instance). Populated from env vars
    // ${HOSTNAME}:${ENGINE_INSTANCE_NAME} at startup in live_infra/main.cpp.
    // Empty in backtest mode — observability is live-only.
    std::string engine_instance;

    // Observability emit batch callback (metrics + health snapshots). Called
    // every 10s from run_periodic_calls_loop when
    // ENGINE_OBSERVABILITY_ENABLED=1. Null by default — the emit tick becomes
    // a no-op if no callback is supplied. See ObservabilityBatch for the
    // payload shape and agentic_system/skills/ENGINE_DIAGNOSTICS.md.
    std::optional<ObservabilityCallback> observability_callback = std::nullopt;
};

// Iteration over the engine's strategies under a shared lock held for the
// view's lifetime. A strategy replaced or removed while trading leaves the
// list but is never destroyed (see retired_strategies).
class StrategiesView {
   public:
    StrategiesView(std::shared_mutex& mutex,
                   std::vector<std::unique_ptr<Strategy>>& strategies)
        : lock_(mutex), strategies_(strategies) {}
    auto begin() const { return strategies_.begin(); }
    auto end() const { return strategies_.end(); }
    std::size_t size() const { return strategies_.size(); }
    bool empty() const { return strategies_.empty(); }
    std::unique_ptr<Strategy>& operator[](std::size_t index) const {
        return strategies_[index];
    }

   private:
    std::shared_lock<std::shared_mutex> lock_;
    std::vector<std::unique_ptr<Strategy>>& strategies_;
};

class Engine {
   public:
    static Engine& getInstance();

    void initialize(
        const TradingLogCallback& trading_log_callback,
        const EngineStateSnapshotCallback& engine_state_snapshot_callback,
        const EngineConfig& config);

    bool isInitialized() const { return initialized_; }
    void reset();

    ~Engine();

    std::optional<Trade> find_trade(int strategy_id, const std::string& symbol,
                                    unsigned long long trade_id);

    void order_created_ack(Core::Order& order);
    void order_filled_ack(Core::OrderFillInfo& order_fill_info);
    void order_canceled_ack(Core::Order& order, bool creation_failed = false);
    void add_strategy(std::unique_ptr<Core::Strategy> strategy);
    // Live strategy sync. Each call takes `strategy` only when APPLIED. BUSY
    // when readers kept the list locked for ~200ms; retry later.
    enum class SyncResult { APPLIED, BUSY, NOT_FOUND, RESTART_REQUIRED };
    // RESTART_REQUIRED for a strategy with models: the model map is read
    // without a lock once the engine trades.
    SyncResult add_strategy_while_trading(
        std::unique_ptr<Core::Strategy>& strategy);
    // Disables `strategy` and waits until no prediction or decision pass of
    // it is running. On false the disabled flag is restored.
    bool quiesce_strategy(Core::Strategy& strategy,
                          std::chrono::milliseconds timeout);
    // Swaps the strategy with `next`'s id for `next`, which must already
    // hold the old one's state (Strategy::adopt_runtime_state).
    SyncResult replace_strategy(std::unique_ptr<Core::Strategy>& next);
    SyncResult remove_strategy(int strategy_id);
    void run_periodic_calls();
    void add_funding_payment(const Core::MarketEvent& market_event);

    void gracefully_stop();

    void add_symbol_exchange_info(
        const std::string& exchange_name, const std::string& market_type,
        const std::vector<Core::SymbolExchangeInfo>& exchange_market_symbols);
    void inventory_mismatch_remove_trade(const Core::Trade& trade);
    bool manually_remove_trade(const Core::ManuallyRemovedTrade& removal);
    void register_engine_signal_handler(const std::string& signal_type,
                                        EngineSignalHandler handler);
    std::vector<std::string> engine_signal_types();
    // Throttled; the fetch, handlers and acks run on periodic_tasks_thread_pool.
    void process_engine_signals();
    std::vector<EngineSignalOutcome> apply_engine_signals(
        const std::vector<EngineSignal>& signals);
    void poll_strategy_states();
    void poll_realtime_subscriptions();
    void emit_strategy_telemetry(StrategyTelemetryEvent&& event);
    void drain_strategy_telemetry();
    std::shared_ptr<std::unordered_map<int, StrategyTelemetryEvent>>
    get_latest_strategy_telemetry();
    void add_trade_to_logs(const Core::Trade& trade);
    void add_tracked_misc_order(const Core::Order& order);
    void remove_tracked_misc_order(const Core::Order& order);
    std::unordered_map<std::string, std::vector<Core::Order>>
    get_tracked_misc_orders();
    void set_tracked_misc_orders(
        const std::unordered_map<std::string, std::vector<Core::Order>>&
            tracked_misc_orders);
    void add_order_to_logs(const Core::Order& order);
    void add_signal_log(const Core::Signal& signal, const std::string& symbol,
                        const int strategy_id);
    void update_order(const Core::Order& order);

    long long get_time_to_next_kline_close(long long interval_ms) const;
    long long get_latest_fully_completed_kline_open_time_ms(
        long long interval_ms) const;

    long long get_latest_fully_completed_kline_close_time_ms(
        long long interval_ms) const;

    void generate_predictions();
    void initialize_symbol_precision_access();
    void sync_persistent_state_to_disk();
    void add_order_cancel_to_logs(const Core::Order& order);
    void add_order_fill_to_logs(const Core::OrderFillInfo& order_fill_info);

    void update_strategy_virtual_balance(const Core::Trade& trade);
    void update_strategy_virtual_balance_revert(const Core::Trade& trade);

    void create_system_log(const std::string& symbol,
                           const std::string& log_level,
                           const std::string& message,
                           const std::string& event_type,
                           const nlohmann::json& metadata,
                           const int strategy_id,
                           const std::string& exchange_name = "",
                           const std::string& market_type = "",
                           bool is_trading_activity_log = false);

    std::optional<double> get_price(const std::string& symbol,
                                    const std::string& exchage_name,
                                    const std::string& market_type);

    std::optional<double> get_borrowable_inventory(
        const std::string& asset, const std::string& exchange_name,
        const std::string& market_type, long long account_id);
    void update_borrowable_inventory(const std::string& key, double value);
    void update_borrowable_inventories(
        const std::unordered_map<std::string, double>& updates);

    void set_unsynced_analytics_data(
        std::unordered_map<std::string, std::vector<Core::Order>> new_orders,
        std::unordered_map<std::string, std::vector<Core::Trade>> new_trades,
        std::unordered_map<std::string, std::vector<Core::Order>>
            new_orders_cancels,
        std::unordered_map<std::string, std::vector<Core::OrderFillInfo>>
            new_order_fills);

    StrategiesView get_strategies();
    std::unordered_map<std::string, std::vector<Core::Order>> get_new_orders();
    std::unordered_map<std::string, std::vector<Core::Trade>> get_new_trades();
    std::unordered_map<std::string, std::vector<Core::Order>>
    get_new_orders_cancels();
    std::unordered_map<std::string, std::vector<Core::OrderFillInfo>>
    get_new_order_fills();

    std::vector<Core::Order> get_failed_cancels();
    std::vector<Core::Order> get_unacked_orders();
    std::vector<Core::Order> get_orders(const std::string& symbol);
    std::vector<Core::Order> get_orders(const std::string& symbol,
                                        const std::string& exchange_name,
                                        const std::string& market_type);

    Core::OrderBook get_order_book(const std::string& exchange_name,
                                   const std::string& symbol) const;

    std::optional<double> get_best_ask(const std::string& symbol,
                                       const std::string& exchange_name,
                                       const std::string& market_type) const;
    std::optional<double> get_best_bid(const std::string& symbol,
                                       const std::string& exchange_name,
                                       const std::string& market_type) const;
    std::optional<std::pair<double, double>> get_best_ask_with_qty(
        const std::string& symbol, const std::string& exchange_name,
        const std::string& market_type) const;
    std::optional<std::pair<double, double>> get_best_bid_with_qty(
        const std::string& symbol, const std::string& exchange_name,
        const std::string& market_type) const;
    std::optional<BidAskWithQty> get_best_bid_ask_with_qty(
        const std::string& symbol, const std::string& exchange_name,
        const std::string& market_type) const;
    std::optional<BidAskWithQty> get_best_bid_ask_with_qty(
        const std::string& symbol, const std::string& exchange_name,
        const std::string& market_type, long long max_age_ms) const;
    std::optional<long long> get_ms_since_last_orderbook_update(
        const std::string& symbol, const std::string& exchange_name,
        const std::string& market_type) const;
    std::optional<long long> get_ms_since_last_orderbook_full_refresh(
        const std::string& symbol, const std::string& exchange_name,
        const std::string& market_type) const;
    std::optional<double> get_spread(const std::string& symbol,
                                     const std::string& exchange_name,
                                     const std::string& market_type) const;
    std::optional<double> get_ask_depth(const std::string& symbol,
                                        const std::string& exchange_name,
                                        const std::string& market_type,
                                        double slippage) const;
    std::optional<double> get_bid_depth(const std::string& symbol,
                                        const std::string& exchange_name,
                                        const std::string& market_type,
                                        double slippage) const;
    double get_min_notional(const std::string& symbol,
                            const std::string& exchange_name,
                            const std::string& market_type) const;
    std::optional<double> get_avg_ask_size(const std::string& symbol,
                                           const std::string& exchange_name,
                                           const std::string& market_type,
                                           int levels) const;
    std::optional<double> get_avg_bid_size(const std::string& symbol,
                                           const std::string& exchange_name,
                                           const std::string& market_type,
                                           int levels) const;
    std::optional<double> get_book_volume_before_bid(
        const std::string& symbol, const std::string& exchange_name,
        const std::string& market_type, double price) const;
    std::optional<double> get_book_volume_before_ask(
        const std::string& symbol, const std::string& exchange_name,
        const std::string& market_type, double price) const;
    std::optional<int> get_number_of_levels_before_bid(
        const std::string& symbol, const std::string& exchange_name,
        const std::string& market_type, double price) const;
    std::optional<double> get_price_distance_from_mid(
        const std::string& symbol, const std::string& exchange_name,
        const std::string& market_type, double price) const;

    std::vector<Core::Trade> get_trades();
    std::vector<Core::Order> get_orders();
    EngineStateSnapshot build_realtime_snapshot();

    // Read-only snapshot of pending markouts. Returns a copy under
    // markout_mutex_. Exposed for the engine_control_server's
    // /control/engine/markouts/pending endpoint — completed markouts
    // are already drained via build_realtime_snapshot but pending
    // ones aren't otherwise externally observable, so testing the
    // register-on-fill path requires this view.
    std::vector<MarkoutJob> get_pending_markouts_snapshot() {
        std::lock_guard<std::mutex> lock(this->markout_mutex_);
        return this->pending_markouts_;
    }

    void recover_state_from_disk(const std::vector<Core::Trade>& trades,
                                 const std::vector<Core::Order>& orders);
    // Disk entries no loaded strategy could take. They are written back on
    // every state sync and keep their strategy paused until a strategy build
    // restores them or the operator removes them.
    std::vector<Core::Trade> get_held_trades();
    std::vector<Core::Order> get_held_orders();
    void restore_held_state(Core::Strategy& strategy, bool at_startup);

    bool passes_min_notional_filter(const Core::Order& order);

    void round_quantity_and_price(Core::Order& order);
    std::string round_quantity_str(const mpfr::mpreal& quantity,
                                   const std::string& symbol,
                                   const std::string& exchange_name,
                                   const std::string& market_type);
    mpfr::mpreal round_quantity(const std::string& symbol,
                                const std::string& exchange_name,
                                const std::string& market_type,
                                const mpfr::mpreal& quantity);
    mpfr::mpreal round_price(const std::string& symbol,
                             const std::string& exchange_name,
                             const std::string& market_type,
                             const mpfr::mpreal& price);

    int get_qty_precision(const std::string& symbol,
                          const std::string& exchange_name,
                          const std::string& market_type);

    int get_price_precision(const std::string& symbol,
                            const std::string& exchange_name,
                            const std::string& market_type);

    double get_tick_size(const std::string& symbol,
                         const std::string& exchange_name,
                         const std::string& market_type);

    double get_step_size(const std::string& symbol,
                         const std::string& exchange_name,
                         const std::string& market_type);

    void state_is_ready();
    void stop_trading();
    bool is_engine_actively_trading() const;
    void update_exposures_notional();
    void check_trades_for_less_than_min_notional();
    void prune_ticker_order_times();
    void recalculate_ticker_order_book_state();
    EngineStateSnapshot build_engine_state_snapshot_core(bool reset_funding);
    void emit_engine_state_snapshot(bool bypass_guard = false);
    FullAccountState get_full_account_state(long long account_id);
    void process_market_event(const Core::MarketEvent& market_event);
    void trade_market_event(const Core::MarketEvent& market_event);
    // Atomic-swaps the addressed strategy's `active_bundle` with a new
    // ModelBundle built from the event's spec. Cheap — no ONNX loading; the
    // strategy lazy-loads sessions keyed by path on first use.
    void dispatch_model_update(const Core::MarketEvent& market_event);
    // Called once per strategy at add_strategy time. Reads the strategy's
    // fold_schedule.json (if configured), picks the entry whose timestamp
    // is <= engine.get_time(), and seeds active_bundle via
    // dispatch_model_update. Live: get_time() is wall-clock, seeds with
    // currently-active fold. Backtest: get_time() is 0 pre-replay, no entry
    // matches, seeding deferred to read_model_fold_events's per-chunk warm-up
    // event.
    void seed_active_bundle_from_schedule(Core::Strategy& strategy);
    void update_sizing_coeff(const Core::MarketEvent& market_event);
    void update_quote_asset(const std::string& exchange_name,
                            const std::string& market_type,
                            const std::string& asset, long long account_id,
                            const Core::QuoteAsset& quote_asset);
    void update_inventory(const std::string& exchange_name,
                          const std::string& market_type,
                          const std::string& symbol, long long account_id,
                          const Core::Inventory& inventory);

    std::optional<Inventory> get_inventory(const std::string& exchange_name,
                                           const std::string& market_type,
                                           const std::string& symbol,
                                           long long account_id);
    void remove_inventories_by_symbol(const std::string& symbol,
                                      const std::string& exchange_name,
                                      const std::string& market_type);

    void flush_trading_logs(bool force_flush = false);
    void update_price(const std::string& symbol,
                      const std::string& exchange_name,
                      const std::string& market_type, double price);

    void update_inventory_and_quote_assets(
        const std::string& exchange_name, const std::string& market_type,
        long long account_id, const std::vector<Core::Inventory>& inventories,
        const std::vector<Core::QuoteAsset>& quote_assets);

    void order_created_log(const Core::Order& order);
    void order_cancelled_log(const Core::Order& order);

    void update_price_market_event(const MarketEvent& market_event);

    mpfr::mpreal get_fee_rate(const std::string& exchange,
                              const std::string& market_type, bool is_maker);

    std::optional<std::reference_wrapper<OnnxModel>> get_model(
        int strategy_id, const std::string& model_path);

    const DigitalOptionCalibrationMap* get_digital_option_calibration_map(
        int strategy_id, const std::string& model_path);

    void apply_orderbook_update(const Core::OrderBookUpdate& update);
    void init_orderbook(const std::string& exchange_name,
                        const std::string& market_type,
                        const std::string& symbol);
    void apply_full_refresh_orderbook(const std::string& exchange_name,
                                      const std::string& market_type,
                                      const std::string& symbol,
                                      const Core::OrderBookLevels& levels);
    void apply_orderbook_levels_only(const std::string& exchange_name,
                                     const std::string& market_type,
                                     const std::string& symbol,
                                     const Core::OrderBookLevels& levels);
    std::vector<std::string> get_order_books_out_of_sync();

    // BBA (Best Bid/Ask) state — separate from order book, updated by book
    // ticker stream. Seqlock per entry for lock-free consistent reads.
    struct BBAState {
        std::atomic<uint64_t> seq{0};
        std::atomic<double> best_bid_price{0.0};
        std::atomic<double> best_bid_qty{0.0};
        std::atomic<double> best_ask_price{0.0};
        std::atomic<double> best_ask_qty{0.0};
        std::atomic<long long> timestamp_ms{0};
    };

    struct BBASnapshot {
        double best_bid_price;
        double best_bid_qty;
        double best_ask_price;
        double best_ask_qty;
        long long timestamp_ms;
    };

    void update_bba(const std::string& exchange_name,
                    const std::string& market_type, const std::string& symbol,
                    double bid_price, double bid_qty, double ask_price,
                    double ask_qty, long long timestamp_ms);
    std::optional<BBASnapshot> get_bba(const std::string& symbol,
                                       const std::string& exchange_name,
                                       const std::string& market_type) const;

    bool is_order_creation_disabled() const;

    // Polymarket event store
    void add_polymarket_event(std::shared_ptr<PolymarketActiveEvent> event);
    void remove_polymarket_event(const std::string& condition_id);
    std::optional<std::shared_ptr<PolymarketActiveEvent>> get_polymarket_event(
        const std::string& condition_id) const;
    std::optional<std::shared_ptr<PolymarketActiveEvent>>
    get_active_polymarket_event(const std::string& series_slug,
                                long long current_time_ms) const;
    void update_polymarket_event_bba(const std::string& condition_id,
                                     int outcome_index, double best_bid,
                                     double best_ask, long long ts);

    // Polymarket L2 order books (keyed by asset_id in order_books map)
    void init_polymarket_orderbook(const std::string& asset_id);
    void apply_polymarket_l2_snapshot(const std::string& asset_id,
                                      const Core::OrderBookLevels& levels);
    void apply_polymarket_l2_delta(const std::string& asset_id, double price,
                                   double size, bool is_bid,
                                   long long timestamp_ms);
    const Core::OrderBook* get_polymarket_orderbook(
        const std::string& asset_id) const;
    void update_polymarket_event_last_trade(const std::string& condition_id,
                                            int outcome_index, double price,
                                            long long ts);
    struct OrderbookRefreshCandidate {
        std::string symbol;
        std::string exchange_name;
        std::string market_type;
    };
    std::optional<OrderbookRefreshCandidate> get_oldest_synced_orderbook();

    // Markout tracking
    void register_markout(const OrderFillInfo& fill_info);
    void process_markouts();

    // Risk engine methods
    std::tuple<Core::OrderConstructionResult, std::optional<Core::Order>>
    construct_order(Strategy& strategy, const Core::CreateOrderParams& params);
    void remove_order_from_risk_engine(unsigned long long order_id);
    void register_order_in_risk_engine(const Core::Order& order);
    void update_order_in_risk_engine(const Core::Order& order);
    void add_trade_to_risk_engine(const Core::Trade& trade);
    mpfr::mpreal get_long_ratio(bool require_risk_engine_lock = true);
    mpfr::mpreal get_short_ratio(bool require_risk_engine_lock = true);
    mpfr::mpreal get_long_ratio_by_symbol(const std::string& symbol,
                                          bool require_risk_engine_lock = true);
    mpfr::mpreal get_short_ratio_by_symbol(
        const std::string& symbol, bool require_risk_engine_lock = true);
    void remove_trade_from_risk_engine(unsigned long long trade_id);
    void update_trade_in_risk_engine(const Core::Trade& trade);
    void remove_orders_and_trades_from_risk_engine_by_symbol(
        const std::string& symbol);
    // Periodic reconciliation: snapshot live orders/trades from every
    // TradeTicker across every Strategy, then rebuild risk_engine state
    // from that projection. Defense-in-depth against any aggregate-state
    // drift bug. Lock-order-safe: takes Strategy::trade_tickers_mutex,
    // then per-ticker trading_mutex briefly (one at a time), then
    // RiskEngine::mutex once via rebuild_from_state.
    void reconcile_risk_engine_with_tickers();
    void run_periodic_strategy_cleanup();

    // Diag accessor — read-only view of the risk engine for backtest
    // instrumentation. Lock-free; only safe to call from sim-event handlers.
    const RiskEngine& peek_risk_engine() const { return risk_engine; }

    double get_nav();
    std::optional<mpfr::mpreal> get_nav_by_account_id(long long account_id);

    long long get_time() const;
    std::optional<int> get_minutes_until_market_close() const;

    std::unordered_map<std::string, Core::SymbolExchangeInfo>
    get_exchange_symbol_infos() const;

    bool skip_historical_order_accumulation_on_backtest = false;

    std::atomic<long long> num_market_events_processed;
    long long num_backpressure_events_;
    long long num_dropped_trade_events_;
    long long num_dropped_orderbook_events_;

    bool is_backtest;
    bool is_strategy_development;
    bool is_order_creation_disabled_ = false;
    // Stable identity of this engine process, populated from
    // EngineConfig.engine_instance at initialize() time. Read at observability
    // emit time to tag rows in engine_health, strategy_health, engine_metric,
    // and engine_snapshot_index. Empty in backtest mode.
    std::string engine_instance_;

    // Observability callback, populated from
    // EngineConfig.observability_callback at initialize(). Null when
    // observability is disabled — tick becomes no-op.
    std::optional<ObservabilityCallback> observability_callback_;

    // Wall-clock startup epoch ms, set in initialize() so engine_health can
    // report last_startup_ts.
    int64_t last_startup_ts_ms_ = 0;

    // event_link_id range tracking for snapshot archival. Updated in
    // create_system_log(); read + reset in emit_engine_state_snapshot().
    // The snapshot records the [lo, hi] range of event_link_ids drained
    // since the previous snapshot — agents cross-ref a log row to the
    // snapshot whose range contains its event_link_id.
    std::atomic<int64_t> snapshot_event_link_id_lo_{-1};
    std::atomic<int64_t> snapshot_event_link_id_hi_{-1};

    // Last emit timestamp for MARKET_EVENTS_QUEUE_BACKPRESSURE — one emit
    // per minute per process, set in build_engine_health_snapshot.
    std::atomic<int64_t> last_backpressure_emit_ms_{0};
    // Last emit timestamp for UNACKED_ORDER_TIMEOUT — one emit per minute
    // per order_id; cleared on order ack or removal.
    std::atomic<int64_t> last_unacked_timeout_sweep_ms_{0};
    bool emit_logs_in_real_time;
    std::function<void(const TradingLog&)> publish_trading_log_callback =
        nullptr;
    bool emit_realtime_trading_state;
    std::function<void(const Core::Engine& engine)>
        publish_realtime_trading_state_callback = nullptr;
    long long seconds_between_realtime_trading_state_updates = 5;
    std::atomic<std::chrono::steady_clock::time_point>
        last_realtime_trading_state_update_time =
            std::chrono::steady_clock::now();
    long long program_start_time_ms = 0;

    bool enable_engine_signals = false;
    FetchEngineSignalsCallback fetch_engine_signals_callback = nullptr;
    AckEngineSignalCallback ack_engine_signal_callback = nullptr;
    std::atomic<std::chrono::steady_clock::time_point>
        last_process_engine_signals_time = std::chrono::steady_clock::now();
    // A slow handler must not let the next tick fetch the same rows again.
    std::atomic<bool> engine_signals_in_flight{false};
    std::mutex engine_signal_handlers_mutex;
    std::unordered_map<std::string, EngineSignalHandler> engine_signal_handlers;

    bool enable_realtime_subscriptions = false;
    FetchRealtimeSubscriptionsCallback fetch_realtime_subscriptions_callback =
        nullptr;
    long long seconds_between_subscription_polls = 10;
    std::atomic<std::chrono::steady_clock::time_point>
        last_poll_realtime_subscriptions_time =
            std::chrono::steady_clock::now();
    std::unordered_set<std::string> active_orderbook_subscriptions;

    template <typename F>
    auto enqueue_order_task(F&& task) {
        if (!order_thread_pool) {
            throw std::runtime_error("Order thread pool not initialized");
        }
        return order_thread_pool->enqueue(std::forward<F>(task));
    }

    template <typename F>
    auto enqueue_cancel_order_task(F&& task) {
        if (!cancel_order_thread_pool) {
            throw std::runtime_error(
                "Cancel order thread pool not initialized");
        }
        return cancel_order_thread_pool->enqueue(std::forward<F>(task));
    }

    template <typename F>
    auto enqueue_strategy_task(F&& task) {
        if (!strategy_thread_pool) {
            throw std::runtime_error("Strategy thread pool not initialized");
        }
        return strategy_thread_pool->enqueue(std::forward<F>(task));
    }

    std::atomic<std::chrono::steady_clock::time_point>
    get_last_periodic_calls_time() const;

    bool is_engine_trading() const;

    // Read-only accessors for the EngineControlServer (live_infra HTTP control
    // plane). Internal state otherwise — exposed so external tests/diagnostics
    // can poll without taking any locks.
    bool get_is_state_ready() const { return is_state_ready.load(); }
    long long get_latest_market_event_time_ms() const {
        return latest_market_event_time_ms.load();
    }

   private:
    bool remove_held_trade(const Core::ManuallyRemovedTrade& removal);

    std::mutex held_state_mutex_;
    std::vector<Core::Trade> held_trades_;
    std::vector<Core::Order> held_orders_;
    std::unordered_set<int> held_paused_strategy_ids_;

    std::atomic<bool> running{false};
    bool emit_trading_logs;
    std::atomic<bool> is_state_ready{false};

    TradingLogCallback trading_log_callback_;
    EngineStateSnapshotCallback engine_state_snapshot_callback_;
    std::unique_ptr<ThreadPool> order_thread_pool;
    std::unique_ptr<ThreadPool> cancel_order_thread_pool;
    std::unique_ptr<ThreadPool> strategy_thread_pool;
    std::unique_ptr<ThreadPool> generate_predictions_thread_pool;
    std::unique_ptr<ThreadPool> periodic_tasks_thread_pool;

    std::mutex engine_state_mutex;
    mutable std::shared_mutex inventory_mutex;
    std::shared_mutex price_mutex;
    mutable std::shared_mutex exchange_symbol_infos_mutex;
    std::mutex new_trades_mutex;
    std::mutex new_orders_mutex;
    std::mutex new_orders_cancels_mutex;
    std::mutex new_order_fills_mutex;

    // Time state
    std::atomic<std::chrono::steady_clock::time_point>
        last_update_exposures_notional_time;
    std::atomic<std::chrono::steady_clock::time_point>
        last_check_trades_for_less_than_min_notional_time{
            std::chrono::steady_clock::now()};
    std::atomic<std::chrono::steady_clock::time_point>
        last_prune_ticker_order_times{std::chrono::steady_clock::now()};
    std::atomic<std::chrono::steady_clock::time_point>
        last_recalculate_ticker_order_book_state_time{
            std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point last_poll_market_data_streams_time;
    std::chrono::steady_clock::time_point last_flush_trading_logs_time;
    std::atomic<std::chrono::steady_clock::time_point>
        last_log_throughput_stats_time;
    std::atomic<std::chrono::steady_clock::time_point> last_periodic_calls_time;
    std::chrono::steady_clock::time_point last_engine_state_snapshot_time;

    std::atomic<long long> latest_market_event_time_ms;
    long long last_periodic_strategy_cleanup_ms_ = 0;

    std::thread run_periodic_calls_thread;
    std::thread generate_predictions_thread;
    std::thread generate_strategy_decisions_thread;
    // Writers hold this exclusively only to change the vector, take no other
    // lock and never wait inside it. Readers may nest (the snapshot builder
    // calls get_trades) and may hold trading_mutex (a completed trade
    // updates the virtual balance): both rely on glibc's reader-preferring
    // rwlock.
    std::shared_mutex strategies_mutex;
    std::vector<std::unique_ptr<Core::Strategy>> strategies;
    // Queued tasks and control-server handlers hold raw Strategy pointers,
    // so a replaced or removed strategy lives here until the process exits.
    std::vector<std::unique_ptr<Core::Strategy>> retired_strategies;
    std::unique_lock<std::shared_mutex> try_lock_strategies_exclusive();
    StrategiesView strategies_view() {
        return StrategiesView(this->strategies_mutex, this->strategies);
    }

    moodycamel::ConcurrentQueue<TradingLog> trading_logs;
    moodycamel::ConcurrentQueue<StrategyTelemetryEvent>
        strategy_telemetry_queue_;
    ExclusivePeriodicJob strategy_telemetry_drain_job_;
    bool enable_strategy_telemetry_ = false;
    std::function<void(const std::string&)>
        publish_strategy_telemetry_callback_;
    std::unordered_map<int, std::chrono::steady_clock::time_point>
        strategy_telemetry_last_emit_;
    std::mutex strategy_telemetry_emit_mutex_;
    std::shared_ptr<std::unordered_map<int, StrategyTelemetryEvent>>
        latest_strategy_telemetry_;
    std::mutex latest_strategy_telemetry_mutex_;

    std::unordered_map<std::string, std::vector<Core::Order>>
        tracked_misc_orders;
    std::unordered_map<std::string, Core::SymbolExchangeInfo>
        exchange_symbol_infos;

    std::unordered_map<std::string, Core::OrderBook> order_books;
    std::unordered_set<std::string> order_books_out_of_sync;
    std::mutex order_books_out_of_sync_mutex;
    std::unordered_map<std::string, std::vector<Core::OrderBookUpdate>>
        cached_orderbook_updates;
    std::mutex cached_orderbook_updates_mutex;

    std::unordered_map<std::string, std::atomic<double>> prices;
    std::unordered_map<std::string, Core::QuoteAsset> quote_assets;
    std::unordered_map<std::string, Core::Inventory> inventories;

    // BBA state — shared_ptr so readers can grab the pointer under shared lock
    // then read atomics without holding any lock
    std::unordered_map<std::string, std::shared_ptr<BBAState>> bba_states_;
    mutable std::shared_mutex bba_mutex_;

    // Seqlock-protected borrowable_inventories
    std::atomic<uint64_t> borrowable_inventories_seq{0};
    std::unordered_map<std::string, double> borrowable_inventories;
    std::unordered_map<std::string, Core::FeeModel> fees_by_exchange_and_market;
    std::unordered_map<std::string, std::vector<Core::Trade>>
        new_trades;  // flushed in emit engine snapshot
    std::unordered_map<std::string, std::vector<Core::Order>>
        new_orders;  // flushed in emit engine snapshot
    std::unordered_map<std::string, std::vector<Core::Order>>
        new_orders_cancels;  // flushed in emit engine snapshot
    std::unordered_map<std::string, std::vector<Core::OrderFillInfo>>
        new_order_fills;  // flushed in emit engine snapshot

    std::unordered_map<std::string, std::unique_ptr<OnnxModel>>
        model_path_to_model_map;

    std::unordered_map<std::string, DigitalOptionCalibrationMap>
        digital_option_calibration_maps_;

    std::deque<Core::MarketEvent> market_events;

    // Polymarket event store
    std::unordered_map<std::string, std::shared_ptr<PolymarketActiveEvent>>
        polymarket_events_;
    mutable std::shared_mutex polymarket_events_mutex_;

    std::atomic<double> global_long_exposure;
    std::atomic<double> global_short_exposure;
    std::atomic<double> global_nav;
    std::atomic<double> global_margin_notional;
    std::atomic<double> global_funding_payments;
    std::unordered_map<long long, mpfr::mpreal> navs_by_account_id;
    RiskEngine risk_engine;

    // Markout tracking
    std::vector<MarkoutJob> pending_markouts_;
    std::mutex markout_mutex_;
    std::vector<CompletedMarkout> completed_markouts_;
    std::mutex completed_markouts_mutex_;

    void load_models();
    void event_loop();
    void run_periodic_calls_loop();
    void generate_predictions_loop();
    void generate_strategy_decisions_loop();

    void create_system_log(const std::string& message,
                           const std::string& event_type,
                           const nlohmann::json& metadata);
    void log_throughput_stats();

    // --- Observability emitter — see ObservabilityBatch + LOGGING.md ---
    // Called every ~10s from run_periodic_calls_loop. No-op unless
    // observability_callback_ is populated and is_state_ready is true.
    // Strategy + SM health are included every 3rd tick (~30s cadence);
    // metrics + engine health every tick.
    void emit_observability_tick(bool is_shutdown = false);
    EngineHealthSnapshot build_engine_health_snapshot(bool is_shutdown);
    StrategyHealthSnapshot build_strategy_health_snapshot(Strategy& strategy);
    SmHealthSnapshot build_sm_health_snapshot(
        const ::MarketData::StrategyStateMachine& sm);
    void sample_latency_histograms(std::vector<EngineMetricSample>& out,
                                   int64_t ts_ms);

    // Latency histograms — sampled + reset by sample_latency_histograms at
    // each observability tick. record() call sites live at the order/fill
    // ack paths. See agentic_system/subsystems/cpp/common/LOG_EVENT_SCHEMA.md.
    LatencyHistogram latency_order_sent_to_ack_;
    LatencyHistogram latency_fill_to_ack_;

    // Periodic tick state.
    std::chrono::steady_clock::time_point last_observability_tick_time_ =
        std::chrono::steady_clock::time_point::min();
    uint32_t observability_tick_count_ = 0;  // for 30s-gated strategy/SM emit
    std::atomic<bool> observability_delivery_in_flight_{false};

    std::atomic<bool> initialized_{false};
};
}  // namespace Core
