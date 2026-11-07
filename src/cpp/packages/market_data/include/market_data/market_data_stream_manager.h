#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "market_data/market_data_coverage.h"

namespace MarketData {

// Subscribes a process to the coverage in MARKET_DATA_COVERAGE_PATH. Outside
// the trading client it records: datasets hold only the last few days in
// memory and are flushed and trimmed every 10 minutes.
class MarketDataStreamManager {
   public:
    MarketDataStreamManager(
        const std::function<void(const std::string&)>& error_log_callback,
        const std::function<void(const std::string&)>& info_log_callback,
        const std::chrono::system_clock::time_point data_start_date,
        bool is_called_from_trading_client = false);
    ~MarketDataStreamManager() = default;

    void start_stream();
    void update_polymarket_subscriptions(
        const std::vector<std::string>& asset_ids);

    // Re-reads the coverage file and applies the change to the running
    // streams. Refused in the trading client: the engine's order-book map is
    // read without a lock there, so books can only be added before trading.
    // Removed datasets stay loaded until the next restart.
    void refresh_coverage();

   private:
    void init_state();
    void init_ohlcv_datasets(const std::vector<ResolvedOhlcvDataset>& datasets);
    void init_trade_bar_datasets(
        const std::vector<ResolvedTradeBarDataset>& datasets);
    void init_order_books(const std::vector<ResolvedMarketSymbol>& books);
    void apply_change(const CoverageChange& change);
    // Applies the in-memory half of splits that data_library's
    // us_equity_split_adjuster has already applied on disk.
    void apply_split_requests();
    void validate_state_machine_dependencies(
        const ResolvedCoverage& coverage) const;
    [[nodiscard]] static ResolvedCoverage resolve(
        const CoverageConfig& config, const ResolvedCoverage& current);

    std::atomic<bool> is_initialized{false};
    const long long process_start_ms_;
    bool is_called_from_trading_client;
    std::function<void(const std::string&)> error_log_callback;
    std::function<void(const std::string&)> info_log_callback;
    std::chrono::system_clock::time_point data_start_date;

    std::mutex refresh_mutex_;
    std::mutex coverage_mutex_;
    ResolvedCoverage coverage_;
    // Dataset identities ever created, so a dataset removed and later
    // re-added is not registered twice.
    std::set<std::string> created_datasets_;
};
}  // namespace MarketData
