#pragma once

#include <market_data/market_event.h>
#include <market_data/strategy_state_machine.h>

#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace HistoricalFeatureGen {

struct DatasetConfig {
    std::string type;          // "trade" or "book_ticker" — determines column parsing
    std::string path;          // absolute path to parquet directory
    std::string symbol;
    std::string exchange;
    std::string market_type;
    bool use_local_timestamp = false;
};

// Default chunk size: 1 week in milliseconds
static constexpr long long DEFAULT_CHUNK_SIZE_MS = 7LL * 24 * 3600 * 1000;

struct EmitSymbolConfig {
    int symbol_idx;
    std::string symbol_name;
    std::string output_path;
};

struct Config {
    long long start_ts_ms;                // replay window start (0 = all)
    long long end_ts_ms;                  // replay window end (0 = all)
    long long emit_interval_ms;           // min emit interval (0 = every event)
    long long emit_interval_max_ms;      // max emit interval (0 = use emit_interval_ms fixed)
    long long chunk_size_ms;             // time window loaded at once (0 = load all)
    std::string emit_strategy;            // "wallclock" or "tick"
    std::string emit_tick_type;           // "trade", "bba" — only used when emit_strategy == "tick"
    std::string output_path;              // where to write output parquets (legacy single-output)
    std::string state_machine;            // SM type name (e.g. "spread")
    std::vector<DatasetConfig> datasets;  // explicit dataset definitions with absolute paths
    nlohmann::json sm_params;             // optional SM-specific parameters from JSON config
    long long incremental_buffer_ms = 3600000;  // 1 hour overlap for incremental runs
    long long emit_start_ts_ms = 0;
    long long output_part_rows = 0;
    bool include_reference_state = false;
    bool use_streaming = true;           // use merge iterator (streaming) instead of load-all-sort

    // Per-symbol emission: if non-empty, emit_state_for_symbol(idx) is called
    // for each entry instead of emit_state(). Each symbol gets its own output.
    std::vector<EmitSymbolConfig> emit_symbols;
};

Config parse_config(const std::string& json_path);

// Create a state machine by name. Throws if name is unknown.
std::unique_ptr<MarketData::StrategyStateMachine> create_state_machine(
    const std::string& name);

// Load datasets, merge into MarketEvent stream sorted by timestamp
std::vector<MarketData::MarketEvent> load_events(
    const std::vector<DatasetConfig>& datasets,
    long long start_ts_ms, long long end_ts_ms);

// Replay events through a state machine, emit features at intervals, write
// parquet output. Calls sm.feature_names() after replay for column names.
int run(const Config& config, MarketData::StrategyStateMachine& sm);

}  // namespace HistoricalFeatureGen
