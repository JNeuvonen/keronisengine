#pragma once

#include <atomic>
#include <mutex>
#include <optional>
#include <ostream>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "market_data/market_event.h"
#include "market_data/sm_market_data_dependency.h"

namespace MarketData {

class StrategyStateMachine {
   public:
    virtual ~StrategyStateMachine() = default;

    void react(const MarketEvent& event, bool is_historical = false);
    std::optional<std::vector<float>> emit_state() const;
    virtual void initialize_symbol_states() = 0;
    virtual void configure(const nlohmann::json& params) { (void)params; }
    virtual std::vector<std::string> feature_names() const { return {}; }

    // Per-symbol emission for multi-symbol SMs (default: not supported)
    virtual std::optional<std::vector<float>> emit_state_for_symbol(
        int symbol_idx) const {
        (void)symbol_idx;
        return std::nullopt;
    }
    virtual std::vector<std::string> feature_names_for_symbol(
        int symbol_idx) const {
        (void)symbol_idx;
        return {};
    }

    virtual std::string get_name() const { return "unnamed_sm"; }
    virtual std::vector<SMMarketDataDependency>
    get_market_data_dependencies() const {
        return {};
    }

    void prepare_historical();
    void prepare_live();
    bool is_live_mode() const { return live_mode_; }
    bool has_loaded_state() const { return has_loaded_state_; }

    // Bar-SM warmup hook. SMs declaring OHLCV deps override this to replay the
    // last `max(window)` bars from the on-disk parquet store through react(),
    // populating internal rolling state. Tick-only SMs use the default no-op.
    // Called from prepare_live() AND prepare_historical(). Bars are gap-free
    // by construction (the aggregator fills them) so no syncer is needed —
    // warmup-from-disk is the equivalent of the tick syncer's REST backfill.
    // The SM is responsible for locating + reading the parquet bars itself
    // (typically via DataLibraryPaths + duckdb/polars).
    virtual void warmup_from_bars() {}

    // Bounded warm-up opt-in. Return true ONLY if every piece of this SM's
    // state is bounded by required_warmup_seconds() — pure rolling-window
    // accumulators with no unbounded memory (EMAs, running extrema,
    // cumulative counters, fixed-epoch references). When true, prepare_live()
    // may discard a persisted anchor older than the window and warm fresh
    // over just required_warmup_seconds() instead of REST-backfilling full
    // history. This is a correctness contract: a too-aggressive override
    // silently breaks train/serve parity. Default false = always full replay.
    virtual bool supports_bounded_warmup() const { return false; }

    // Longest feature window in seconds (the warm-up horizon). Independent of
    // the policy above — exposed for the live syncer clamp and diagnostics.
    virtual long long required_warmup_seconds() const { return 0; }

    // Minimum time between periodic persists (the syncer's cadence is 30s);
    // an SM with a large state raises it.
    virtual long long persist_interval_ms() const { return 0; }
    // False keeps state.bin as it is, e.g. while live state is incomplete.
    virtual bool should_persist() const { return true; }

    void serialize_state() const;
    void serialize_state_to_stream(std::ostream& out) const;
    bool deserialize_state();

    bool is_fully_synced() const;
    std::pair<int, int> get_sync_progress() const;
    long long get_min_synced_ts() const;
    SMMarketDataDependency* get_next_unsynced_dependency();
    void init_dependencies();
    void set_fully_synced();
    void stream_disconnected(const std::string& symbol,
                             const std::string& exchange_name,
                             const std::string& market_type,
                             MarketEvent::Type event_type);

    std::string get_persistence_dir() const;
    // Called by stream_disconnected() for a trade stream, whatever the
    // machine's declared dependencies. From the health-check thread.
    virtual void on_trade_stream_disconnected(const std::string& symbol,
                                              const std::string& exchange_name,
                                              const std::string& market_type) {
        (void)symbol; (void)exchange_name; (void)market_type;
    }
    // Called about once a second from the state machine syncer thread,
    // synced or not: for work too slow for the dispatcher thread (REST).
    virtual void run_background_work() {}
    virtual long long get_latest_event_ts_ms() const { return 0; }

    // Per-symbol freshness for multi-symbol SMs. A single global
    // get_latest_event_ts_ms() is kept fresh by the busiest symbol, so it
    // cannot gate a quiet symbol's staleness. Override to return the last
    // event ts for one symbol slot; default falls back to the global value
    // (correct for single-symbol SMs).
    virtual long long get_symbol_latest_event_ts_ms(int symbol_idx) const {
        (void)symbol_idx;
        return get_latest_event_ts_ms();
    }

    // Optional: compact JSON summary of persisted state for agent diagnostics.
    // Called at heartbeat cadence (~30s) by the observability emitter and
    // written to state_machine_health.diagnostic_json. Payload MUST be bounded
    // (soft cap ~10KB at the emitter) — DO NOT return full internal tables;
    // return counts, summary statistics, or most-recently-updated keys.
    // Default returns {} (column will be null).
    virtual nlohmann::json to_diagnostic_json() const { return {}; }

    struct ReferenceState {
        double mid;
        long long timestamp_ms;
    };
    // The quote the machine last priced against; feature generation can
    // write it beside each row to audit what the features saw.
    virtual std::optional<ReferenceState> reference_state() const {
        return std::nullopt;
    }

    // Event-driven machines decide themselves when a feature row exists
    // (feature generation's emit_strategy "sm_rows").
    struct EmittedRow {
        std::string symbol;
        long long timestamp_ms;
        std::vector<float> features;
    };
    // Rows emitted since the last call, in emission order.
    virtual std::vector<EmittedRow> drain_rows() { return {}; }
    // Events that arrived after the row they belonged to was emitted.
    virtual long long late_events() const { return 0; }

   protected:
    // Exclusive lock over SM state for emit_state_for_symbol overrides. emit
    // reads live containers (e.g. std::map book ladders) AND mutates a feature
    // cache, so it must be fully exclusive against react() (which takes the
    // same lock) AND against any concurrent emit (a shared_lock would leave a
    // cache-write race between two emits). Pairs with the unique_lock react()
    // holds — a reader never sees state mid-mutation.
    [[nodiscard]] std::unique_lock<std::shared_mutex> lock_state_exclusive()
        const {
        return std::unique_lock<std::shared_mutex>(state_mutex_);
    }

    virtual void react_impl(const MarketEvent& event) = 0;
    virtual std::optional<std::vector<float>> emit_state_impl() const = 0;
    virtual void serialize_state_impl(std::ostream& out) const { (void)out; }
    virtual bool deserialize_state_impl(std::istream& in) {
        (void)in;
        return true;
    }

   private:
    bool all_dependencies_synced() const;
    void buffer_push_dep(SMMarketDataDependency* dep,
                         const MarketEvent& event);
    void flush_dep_buffer(SMMarketDataDependency* dep);
    void flush_all_dep_buffers_and_go_live();
    void serialize_deps(std::ostream& out) const;
    void deserialize_deps(std::istream& in);
    void write_debug_deps_json() const;
    // Clamp below-floor TRADE cursors to now-required-buffer (live, bounded
    // warm-up SMs only). Returns true iff it clamped. See definition.
    bool apply_bounded_warmup_floor();

    std::unordered_map<std::string, SMMarketDataDependency> dependencies_;
    std::vector<std::string> dep_keys_order_;
    size_t next_dep_index_ = 0;
    std::mutex buffer_mutex_;
    mutable std::shared_mutex state_mutex_;
    std::atomic<bool> is_fully_synced_{false};
    std::once_flag init_deps_flag_;
    // dependencies_ is filled lazily on whichever thread first calls
    // init_dependencies(); readers on other threads check this first.
    std::atomic<bool> deps_initialized_{false};
    bool use_react_lock_ = false;
    bool live_mode_ = false;
    bool has_loaded_state_ = false;
};

}  // namespace MarketData
