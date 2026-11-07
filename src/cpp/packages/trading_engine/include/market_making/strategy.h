#pragma once

#include <atomic>
#include <deque>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "common_types/strategy.h"
#include "market_data/bar_close_policy.h"
#include "market_data/strategy_state_machine.h"
#include "market_data/store.h"
#include "market_making/dtypes/enums.h"
#include "market_making/dtypes/mint_contracts_request.h"
#include "market_making/dtypes/trade.h"
#include "mpreal.h"

namespace MarketData {
class OhlcvDataset;
}  // namespace MarketData

namespace Core {

// Reason codes for set_stop_making_markets. Closed set — pick the tightest one
// that describes *why* the strategy is being paused/resumed. The value flows
// into the STRATEGY_PAUSED / STRATEGY_RESUMED event's metadata.reason field.
namespace StrategyPauseReason {
constexpr const char* POLYMARKET_SIGNAL_STOP = "POLYMARKET_SIGNAL_STOP";
constexpr const char* PRE_SHUTDOWN_CANCEL_BUFFER = "PRE_SHUTDOWN_CANCEL_BUFFER";
constexpr const char* OPERATOR_REQUESTED = "OPERATOR_REQUESTED";
constexpr const char* RISK_KILL_SWITCH = "RISK_KILL_SWITCH";
constexpr const char* DATA_STALE = "DATA_STALE";
constexpr const char* SM_RESET = "SM_RESET";
constexpr const char* WARMUP = "WARMUP";
constexpr const char* SIGNAL_EXPIRED = "SIGNAL_EXPIRED";
}  // namespace StrategyPauseReason

// Forward declarations
class Engine;
class Order;
class TradeTicker;
class MarketEvent;
class StrategyDecision;

class Strategy;
struct OrderFillInfo;
struct BacktestPredictionEvent;

using StrategyCallback = std::vector<StrategyDecision> (*)(
    Engine&, Strategy&, std::optional<MarketEvent>);
using ComputePredictionsCallback = void (*)(Engine&, Strategy&);
using OnFillCallback = void (*)(Engine&, Strategy&, OrderFillInfo&);
// Backtest only: model predictions precomputed offline, keyed by model name.
using ApplyPrecomputedPredictionsCallback = void (*)(
    Engine&, Strategy&,
    const std::unordered_map<std::string, BacktestPredictionEvent>&);
using MintContractsCallback = std::vector<MintContractsRequest> (*)(Engine&,
                                                                    Strategy&);

class Strategy {
   public:
    bool is_disabled_now() const { return this->is_disabled.load(); }
    void set_disabled(bool disabled) { this->is_disabled.store(disabled); }
    bool is_idle() const {
        return !this->is_prediction_thread_running.load() &&
               !this->is_generating_strategy_decisions.load();
    }
    // A second instance of these would register another machine under the
    // same name with fresh state, so the strategy is never rebuilt live.
    static bool registers_own_state_machine(const std::string& strategy_name);

    // Live row change: the rebuilt strategy takes over `old`'s tickers for
    // `shared_symbols` (their orders, trades and positions) and its runtime
    // state. `old` must be idle. Runs before publish; adopt_moving_state
    // repeats the values that can still change once the swap is locked.
    void adopt_runtime_state(Strategy& old,
                             const std::vector<std::string>& shared_symbols);
    void adopt_moving_state(const Strategy& old);
    Strategy(const StrategyConfig& strategy_config);
    ~Strategy();

    std::vector<Core::Trade> get_trades();
    std::vector<Core::Order> get_orders();

    void add_trade_ticker(const std::string& symbol,
                          std::shared_ptr<TradeTicker> trade_ticker);
    void add_trade_ticker_lockless(const std::string& symbol,
                                   std::shared_ptr<TradeTicker> trade_ticker);
    void remove_trade_ticker(const std::string& symbol);
    void remove_trade_ticker_lockless(const std::string& symbol);
    void on_create_order(Core::Engine& engine, Core::Order& order);
    void on_cancel_order(Core::Engine& engine, Core::Order& order);

    void record_enter_order_sent();
    void record_exit_order_sent();
    void prune_recent_order_times();
    int get_recent_enter_orders_count(std::chrono::seconds window) const;
    int get_recent_exit_orders_count(std::chrono::seconds window) const;
    int get_recent_enter_orders_count_10s() const;
    int get_recent_exit_orders_count_10s() const;

    bool can_start_new_prediction() const;
    void mark_prediction_start();
    void mark_prediction_end();
    void sync_persistent_state_to_disk();
    void force_close_trade(Core::Engine& engine, const Core::Trade& trade,
                           ForceCloseReason reason,
                           const nlohmann::json& context = nlohmann::json::object());

    bool can_generate_new_strategy_decisions(bool needs_to_run_realtime) const;
    void mark_strategy_decisions_generation_start();
    void mark_strategy_decisions_generation_end();
    int get_num_active_trades_excluding_symbol(const std::string& symbol);
    int get_num_active_enter_orders_excluding_symbol(const std::string& symbol);
    void update_virtual_balance(const Core::Trade& trade);
    void mark_received_update();
    std::chrono::steady_clock::time_point get_last_received_update() const;
    std::chrono::seconds get_seconds_since_last_received_update() const;
    std::chrono::steady_clock::time_point
    get_last_generated_strategy_decisions() const;
    std::chrono::seconds get_seconds_since_last_generated_strategy_decisions()
        const;
    double get_virtual_balance();

    void process_strategy_decisions(
        Core::Engine& engine,
        const std::vector<StrategyDecision>& strategy_decisions);
    void post_decisions_backtest(
        Core::Engine& engine,
        const std::vector<StrategyDecision>& strategy_decisions);
    void process_crypto_and_stock_market_decisions(
        Core::Engine& engine,
        const std::vector<StrategyDecision>& strategy_decisions);
    void process_prediction_market_decisions(
        Core::Engine& engine,
        const std::vector<StrategyDecision>& strategy_decisions);
    double get_strategy_trade_size(Engine& engine,
                                   const std::string& exchange_name,
                                   const std::string& market_type);

    std::unordered_map<std::string, std::shared_ptr<TradeTicker>> trade_tickers;
    mutable std::shared_mutex trade_tickers_mutex;
    std::optional<std::shared_lock<std::shared_mutex>>
    lock_on_pred_market_strat() const;

    std::vector<std::string> trade_tickers_symbols;
    std::unordered_set<std::string> trade_tickers_symbols_set;
    StrategyConfig strategy_config;

    std::optional<long long> get_exchange_and_market_type_account_id(
        const std::string& exchange_name, const std::string& market_type);

    bool is_ready_to_run_new_prediction(
        const std::vector<std::shared_ptr<::MarketData::OhlcvDataset>>&
            dependant_datasets);
    bool is_ready_to_run_new_prediction(
        const std::string& ticker_symbol,
        const std::vector<std::shared_ptr<::MarketData::OhlcvDataset>>&
            dependant_datasets);
    void poll_market_data_streams(Core::Engine& engine);
    void react_using_polling(Core::Engine& engine);
    struct RecoveredIds {
        std::unordered_set<unsigned long long> trades;
        std::unordered_set<unsigned long long> orders;
    };
    // Restores this strategy's entries that have a ticker; the caller keeps
    // the rest.
    RecoveredIds recover_state_from_disk(Core::Engine& engine,
                                         const std::vector<Core::Trade>& trades,
                                         const std::vector<Core::Order>& orders);
    void update_dataset_store();
    void update_navs_by_account_id(
        const std::unordered_map<long long, mpfr::mpreal>& navs);
    mpfr::mpreal get_nav_by_account_id(long long account_id);

    std::optional<::MarketData::Store> get_dataset_store();
    void generate_predictions(Core::Engine& engine);
    void update_last_prediction_ts(
        const std::vector<std::shared_ptr<::MarketData::OhlcvDataset>>&
            dependant_datasets);
    void update_last_prediction_ts(
        const std::string& ticker_symbol,
        const std::vector<std::shared_ptr<::MarketData::OhlcvDataset>>&
            dependant_datasets);
    std::vector<::MarketData::BarClosePolicy> required_bar_types;
    bool is_in_reduce_only;
    bool is_using_hedges;
    bool needs_to_run_realtime;

    // --- STATE MACHINE ---
    std::shared_ptr<::MarketData::StrategyStateMachine> strategy_event_state_machine;
    // False when the machine is a borrowed global, which ~Strategy must not
    // unregister.
    bool owns_event_state_machine{false};
    std::vector<ExchangeAndMarket> signal_exchanges;

    // --- OBSERVABILITY: WATCHED STATE MACHINES ---
    // Additional SMs this strategy depends on beyond strategy_event_state_machine
    // (e.g. shared globals like "global_bba_sm"). Populated at strategy init via
    // watch_state_machine(). The observability heartbeat walks this vector +
    // strategy_event_state_machine to build strategy_health.sm_dependencies and
    // strategy_health.sm_health_summary.
    //
    // READ-ONLY CONVENTION: strategies MUST NOT call react(), serialize_state(),
    // or any mutating method on these SMs. The StateMachineDispatcher consumer
    // thread is the sole mutator. Violations are flagged by codebase_health.
    struct WatchedStateMachine {
        std::shared_ptr<::MarketData::StrategyStateMachine> sm;
    };
    std::vector<WatchedStateMachine> watched_state_machines;

    // Register a dependency by dispatcher-registered name. Throws
    // std::runtime_error if the name is not registered. Fail-fast is load-
    // bearing — a typo'd name must not silently skip.
    void watch_state_machine(const std::string& name);

    // --- OBSERVABILITY: PERIOD COUNTERS ---
    // Reset (via exchange(0)) by the heartbeat emitter every 30s. Incremented
    // at the respective sites in strategy.cpp and engine.cpp. Reported in
    // strategy_health.period_counts JSONB column.
    std::atomic<uint64_t> period_decisions{0};
    std::atomic<uint64_t> period_orders_placed{0};
    std::atomic<uint64_t> period_orders_cancelled{0};
    // Wall-clock ms, 0 until the first one; reported in strategy_health.
    std::atomic<int64_t> last_prediction_ts_ms{0};
    std::atomic<int64_t> last_order_ts_ms{0};

    // --- STRATEGY MODE FLAGS ---
    bool is_trading_on_prediction_market{false};
    bool is_market_making_strategy{false};
    std::string account_mode{"shared"};
    std::atomic<bool> stop_making_markets{false};

    // Sole write-site for `stop_making_markets`. Emits STRATEGY_PAUSED on a
    // false→true transition and STRATEGY_RESUMED on true→false. Direct
    // `stop_making_markets.store(...)` from outside is a bug — the transition
    // would be invisible to agents tracing "why did strategy X stop quoting".
    // `reason` must be one of Core::StrategyPauseReason::* constants.
    // `extra_metadata` is merged into the emit's metadata JSON (e.g. signal_id).
    void set_stop_making_markets(bool should_stop, const char* reason,
                                 nlohmann::json extra_metadata = {});

    std::vector<MintContractsRequest> get_mint_contracts(Engine& engine);

    // --- FILL CALLBACK ---
    OnFillCallback on_fill_callback = nullptr;

    // --- ACTIVE MODEL BUNDLE ---
    // Spec-only (paths + columns + opaque metadata) — strategy lazy-loads
    // ONNX sessions keyed by path. Engine atomic-stores on MODEL_UPDATE event;
    // strategy atomic-loads at prediction time. shared_ptr keeps in-flight
    // predictions reading the old bundle valid until they finish.
    std::shared_ptr<Core::ModelBundle> active_bundle;

    // Set by the strategy's registered binder (see strategy_registry.h).
    StrategyCallback strategy_callback = nullptr;
    ComputePredictionsCallback compute_predictions_callback = nullptr;
    MintContractsCallback mint_contracts_callback = nullptr;
    ApplyPrecomputedPredictionsCallback apply_precomputed_predictions_callback =
        nullptr;

   private:
    std::unordered_map<std::string, ExchangeAndMarket> exchange_market_map;
    std::chrono::steady_clock::time_point
        last_reacted_to_market_state_by_polling;
    std::mutex strategy_mutex;
    std::atomic<bool> is_disabled;
    std::optional<::MarketData::Store> dataset_store = std::nullopt;
    std::unordered_map<std::shared_ptr<::MarketData::OhlcvDataset>, long long>
        dataset_pointer_to_last_kline_close_time_ms;
    std::unordered_map<
        std::string,
        std::unordered_map<std::shared_ptr<::MarketData::OhlcvDataset>,
                           long long>>
        ticker_dataset_to_last_kline_close_time_ms;
    std::chrono::steady_clock::time_point last_dataset_store_update_time =
        std::chrono::steady_clock::now() - std::chrono::seconds(60 * 60);
    std::atomic<std::chrono::steady_clock::time_point> last_received_update{
        std::chrono::steady_clock::now() - std::chrono::seconds(6000)};
    std::atomic<std::chrono::steady_clock::time_point>
        last_generated_strategy_decisions{std::chrono::steady_clock::now() -
                                          std::chrono::seconds(6000)};

    mutable std::atomic<bool> is_prediction_thread_running{false};
    mutable std::atomic<bool> is_generating_strategy_decisions{false};

    std::atomic<double> virtual_balance{0.0};
    std::unordered_map<long long, mpfr::mpreal> navs_by_account_id;
    bool allow_co_strategy_compounding;

    mutable std::mutex recent_orders_mutex;
    std::deque<std::chrono::steady_clock::time_point> recent_enter_order_times;
    std::deque<std::chrono::steady_clock::time_point> recent_exit_order_times;
    void prune_old_order_times(
        std::deque<std::chrono::steady_clock::time_point>& times,
        std::chrono::seconds max_age) const;
};
}  // namespace Core