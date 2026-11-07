#pragma once

#ifndef DISABLE_VISUALIZATION
#ifdef __APPLE__
#define GL_SILENCE_DEPRECATION
#endif

#include <GLFW/glfw3.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "market_data/dataset_summary.h"
#include "market_data/stream_status.h"
#include "market_making/dtypes/engine_state_snapshot.h"
#include "market_making/dtypes/order.h"
#include "market_making/dtypes/strategy_telemetry.h"
#include "market_making/dtypes/trade.h"

namespace Core {
struct ObservabilityBatch;
struct EngineHealthSnapshot;
struct StrategyHealthSnapshot;
}  // namespace Core

namespace MarketData {
class StrategyStateMachine;
}  // namespace MarketData

namespace Charting {

struct StateMachineStatus {
    std::string name;
    bool has_loaded_state = false;
    bool is_live_mode = false;
    bool is_fully_synced = false;
    int synced_deps = 0;
    int total_deps = 0;
    long long latest_event_ts_ms = 0;
};

struct StrategySummary {
    int id = 0;
    std::string name;
    std::string type;
    std::string account_mode;
    bool is_disabled = false;
    bool is_paused = false;
    bool is_in_reduce_only = false;
    bool is_using_hedges = false;
    bool is_market_making = false;
    bool needs_to_run_realtime = false;
    double virtual_balance = 0.0;
    int open_trade_count = 0;
    std::vector<std::string> trade_ticker_symbols;
    std::vector<StateMachineStatus> state_machines;
};

struct StreamRateHistory {
    long long last_msg_count = 0;
    long long last_sampled_at_ms = 0;
    std::vector<float> msgs_per_sec;
};

class Dashboard {
   public:
    static Dashboard& getInstance();

    bool init(int width = 1280, int height = 720,
              const char* title = "Market Data View");

    bool update();

    void add_engine_state_snapshot(const Core::EngineStateSnapshot& snapshot);

    void shutdown();
    void render_frame();
    void render_controls_pane();
    void render_statistics();
    void render_equity_curve();
    void render_trades_panel();
    void render_orders_panel();
    void render_active_orderbook_panel();
    void render_signals_panel();
    void render_inventories_panel();
    void render_strategy_list_panel();
    void render_strategy_detail_panel();
    void render_health_summary_panel();
    void render_streams_panel();
    void render_state_machines_panel();
    void render_datasets_panel();
    void render_period_returns();
    void set_buy_and_hold_enabled(bool enabled) {
        show_buy_and_hold = enabled;
    }
    void set_live_trades(const std::vector<Core::Trade>& trades);
    void set_event_start_times(
        const std::unordered_map<std::string, long long>& times);

    // Polling mode — call build_realtime_snapshot() directly from the engine
    void set_polling_mode(bool enabled, int interval_ms = 1000);
    void poll_engine_state();

    // Backtest real-time state polling (overwrite current-state panels only)
    void poll_backtest_realtime_state();

    // Market-data dataset introspection. Refreshed on its own timer so it is
    // independent of polling mode, and always built before data_mutex is
    // taken — the render thread must never hold data_mutex while waiting on a
    // dataset's lock.
    void poll_dataset_summaries();

    // Live only; built before data_mutex is taken, like poll_dataset_summaries.
    void poll_health();
    // Engine strategies are appended during init, so they are read only once
    // the engine reports its state ready.
    void poll_strategy_summaries();

    // live_infra's private streams are unreachable from here (the dashboard is
    // also built into backtest_runner), so live_infra supplies them.
    void set_private_stream_status_provider(
        std::function<std::vector<MarketData::StreamStatus>()> provider);
    void set_observability_batch(const Core::ObservabilityBatch& batch);

   private:
    bool is_polling_mode = false;
    std::vector<MarketData::DatasetSummary> dataset_summaries;
    size_t dataset_init_done_count_ = 0;
    std::chrono::steady_clock::time_point last_dataset_poll_time{};
    static constexpr int DATASET_POLL_INTERVAL_MS = 1000;
    std::chrono::steady_clock::time_point last_poll_time{};
    int poll_interval_ms = 1000;

    bool backtest_realtime_view = false;
    std::chrono::steady_clock::time_point last_backtest_realtime_poll_time{};
    int backtest_realtime_poll_interval_ms = 200;
    Dashboard() = default;
    ~Dashboard();
    Dashboard(const Dashboard&) = delete;
    Dashboard& operator=(const Dashboard&) = delete;

    void update_visualization_data();

    // Thread synchronization
    mutable std::mutex data_mutex;

    // Backtest replay emits one snapshot per market event (~25M/sec wall-clock
    // in fast modes). Without subsampling + a cap, `snapshots` grows unbounded
    // and after ~10–11 sim-months malloc consolidation dominates CPU, the
    // strategy effectively stalls, and the kernel OOM-kills the process.
    // Push only when sim-time advances by at least SNAPSHOT_MIN_INTERVAL_MS;
    // when size exceeds MAX_LIVE_SNAPSHOTS, halve via stride-2 decimation
    // (preserving index 0 and the back). Amortizes to O(1) per push.
    static constexpr long long SNAPSHOT_MIN_INTERVAL_MS = 1000;  // 1 sim-second
    static constexpr size_t MAX_LIVE_SNAPSHOTS = 500000;
    long long last_pushed_snapshot_ms = std::numeric_limits<long long>::min();

    std::vector<Core::EngineStateSnapshot> snapshots;

    std::vector<double> timestamps;
    std::vector<double> nav_values;
    std::vector<double> nav_values_with_rebates;
    std::vector<double> long_exposures;
    std::vector<double> short_exposures;
    std::vector<double> margin_notionals;
    std::vector<double> drawdowns;

    std::vector<double> buy_and_hold_returns;
    std::unordered_map<std::string, double> prev_prices;
    std::unordered_map<std::string, double> buy_and_hold_sizes;

    std::unordered_set<unsigned long long> filled_order_ids;
    std::unordered_set<unsigned long long> canceled_order_ids;

    std::unordered_map<std::string, std::vector<Core::Trade>> trades;
    std::unordered_map<std::string, std::vector<Core::Trade>> live_trades;
    std::unordered_map<std::string, long long> symbol_event_start_times;
    std::unordered_map<std::string, std::vector<Core::Order>> orders;
    long long total_orders_seen = 0;

    long long last_market_event_time = 0;

    double max_nav = 0.0;
    double min_nav = 0.0;
    double max_exposure = 0.0;
    double min_exposure = 0.0;
    double max_drawdown = 0.0;

    double max_long_ratio = 0.0;
    double max_short_ratio = 0.0;

    size_t total_snapshots = 0;
    static constexpr size_t RESAMPLE_INTERVAL = 1000;
    static constexpr size_t VISUALIZATION_POINTS = 1000;

    GLFWwindow* window = nullptr;
    const char* glsl_version = nullptr;
    bool is_initialized = false;

    bool show_inventory = true;  // Toggle for inventory visualization
    bool show_buy_and_hold = true;
    bool show_rebates_curve = true;
    bool show_drawdown = false;
    bool render_chart = false;

    // Profit metrics
    double total_profit = 0.0;
    double max_profit = 0.0;
    double total_funding_collected = 0.0;

    Core::EngineStateSnapshot day_start_tick;
    Core::EngineStateSnapshot month_start_tick;
    Core::EngineStateSnapshot year_start_tick;

    std::vector<double> day_percentage_changes;
    std::vector<double> month_percentage_changes;
    std::vector<double> year_percentage_changes;

    std::pair<std::chrono::system_clock::time_point, Core::EngineStateSnapshot>
        first_snapshot;
    std::pair<std::chrono::system_clock::time_point, Core::EngineStateSnapshot>
        last_snapshot;

    // Signals panel state
    std::vector<Core::Signal> current_signals;

    // Closed trade accumulation (polling mode)
    std::unordered_map<unsigned long long, Core::Trade> accumulated_closed_trades_by_id;
    std::unordered_set<unsigned long long> prev_active_trade_ids;
    static constexpr size_t MAX_ACCUMULATED_ITEMS = 20000;

    // Fill volume tracking (polling mode)
    std::unordered_set<unsigned long long> seen_fill_ids;
    double total_fill_volume_usd = 0.0;

    // Persistent outcome mapping (survives trade/order closure)
    std::unordered_map<std::string, std::string> clob_to_outcome_persistent;

    // Inventories panel state
    std::unordered_map<std::string, Core::QuoteAsset> current_quote_assets;
    std::unordered_map<std::string, Core::Inventory> current_inventories;

    // Active orderbook panel state
    std::vector<Core::Order> current_active_orders;
    int orderbook_selected_symbol_idx = 0;
    std::vector<std::string> orderbook_symbols;
    int orderbook_selected_clob_idx = 0;  // 0 = "All" (show all clob_ids)
    std::vector<std::string> orderbook_clob_ids;
    int orderbook_prev_symbol_idx = -1;

    std::shared_ptr<std::unordered_map<int, Core::StrategyTelemetryEvent>>
        current_telemetry_;
    std::vector<Core::Trade> current_active_trades;

    // Every poll_* runs here, never on the GUI thread: they wait on engine,
    // ticker, dataset and stream locks that other threads can hold for
    // seconds, which would freeze the window.
    void poller_loop();
    std::thread poller_thread_;
    std::atomic<bool> poller_running_{false};
    static constexpr int POLLER_TICK_MS = 50;

    std::unordered_map<int, double> realized_pnl_by_strategy_;

    void render_strategy_telemetry(const Core::StrategyTelemetryEvent& tel);
    static StateMachineStatus to_state_machine_status(
        const MarketData::StrategyStateMachine& sm);
    int count_stream_problems() const;

    static constexpr int STATUS_POLL_INTERVAL_MS = 1000;
    static constexpr size_t STREAM_RATE_SAMPLES = 60;

    // Strategies tab
    std::vector<StrategySummary> strategy_summaries_;
    bool engine_state_ready_ = false;
    std::chrono::steady_clock::time_point last_strategy_poll_time_{};
    int selected_strategy_id_ = -1;
    bool show_disabled_strategies_ = false;
    long long signals_strategy_filter_ = -1;

    // Health tab
    std::mutex provider_mutex_;
    std::function<std::vector<MarketData::StreamStatus>()>
        private_stream_status_provider_;
    std::vector<MarketData::StreamStatus> stream_statuses_;
    std::unordered_map<std::string, StreamRateHistory> stream_rates_;
    std::vector<StateMachineStatus> state_machine_statuses_;
    std::chrono::steady_clock::time_point last_health_poll_time_{};
    bool streams_problems_only_ = false;

    // Observability batch: engine health every 10s, strategy health every 30s.
    std::shared_ptr<const Core::EngineHealthSnapshot> engine_health_;
    long long engine_health_at_ms_ = 0;
    double engine_events_per_sec_ = 0.0;
    std::shared_ptr<const std::vector<Core::StrategyHealthSnapshot>>
        strategy_health_;
    long long strategy_health_at_ms_ = 0;
};
}  // namespace Charting
#else
// Stub implementation when visualization is disabled
#include <vector>

#include "market_making/dtypes/engine_state_snapshot.h"
#include "market_making/dtypes/trade.h"
namespace Charting {
class Dashboard {
   public:
    static Dashboard& getInstance() {
        static Dashboard instance;
        return instance;
    }
    bool init(int = 0, int = 0, const char* = nullptr) { return false; }
    bool update() { return false; }
    void add_engine_state_snapshot(const Core::EngineStateSnapshot&) {}
    void shutdown() {}
    void render_frame() {}
    void render_statistics() {}
    void render_equity_curve() {}
    void render_active_orderbook_panel() {}
    void render_signals_panel() {}
    void render_inventories_panel() {}
    void render_datasets_panel() {}
    void render_period_returns() {}
    void set_buy_and_hold_enabled(bool) {}
    void set_polling_mode(bool, int = 1000) {}
    void poll_engine_state() {}
    void poll_backtest_realtime_state() {}
    void poll_dataset_summaries() {}
    void set_live_trades(const std::vector<Core::Trade>&) {}
    void set_event_start_times(
        const std::unordered_map<std::string, long long>&) {}

   private:
    Dashboard() = default;
};
}  // namespace Charting
#endif