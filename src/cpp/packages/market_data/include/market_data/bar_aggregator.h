#pragma once

#include <atomic>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "market_data/bar.h"
#include "market_data/bar_close_policy.h"
#include "market_data/normalized_trade.h"
#include "market_making/dtypes/def_only_structs.h"
#include "ohlcv_dataset_tick.h"

namespace MarketData {
struct OhlcvBarMeta;
struct OhlcvDataset;

class BarAggregator {
   public:
    static BarAggregator& getInstance();
    std::vector<BarFlushInfo> emit_closed_bars();
    std::vector<BarFlushInfo> emit_closed_bars_live();
    std::vector<BarFlushInfo> emit_closed_bars_for_rules(
        const BarClosePolicy& bar_close_policy);

    void add_trade(const NormalizedParsedTrade& trade, bool is_live_streamed);
    void check_completed_bars();

    void add_ohlcv_dataset(const std::shared_ptr<OhlcvDataset>& dataset);
    std::vector<std::shared_ptr<OhlcvDataset>> get_ohlcv_datasets();
    void remove_all_ohlcv_datasets();
    void sync_live_streamed_bars();
    void run_full_tick_level_sync();

    void sync_ohlcv_dataset_historical_bars(
        const std::shared_ptr<OhlcvDataset>& dataset);

    void live_trade_stream_disconnected_reset(const std::string& exchange_name,
                                              const std::string& market_type);

    void live_trade_stream_disconnected_reset(const std::string& exchange_name,
                                              const std::string& market_type,
                                              const std::string& symbol);

    void reread_klines_from_disk();
    void set_reread_klines_from_disk_config(
        bool reread_klines_from_disk_config);
    bool reread_klines_from_disk_config = false;

    void log_bar_sync_status();

   private:
    BarAggregator();
    ~BarAggregator() = default;

    // Delete copy constructor and assignment operator
    BarAggregator(const BarAggregator&) = delete;
    BarAggregator& operator=(const BarAggregator&) = delete;

    std::unordered_map<std::string, std::vector<std::shared_ptr<Bar>>>
        open_bars_map;

    std::unordered_set<std::string> added_ohlcv_datasets;
    std::unordered_map<std::string, Core::ExchangeMarketSymbol> unique_symbols;

    std::vector<std::shared_ptr<Bar>> open_bars_vec;
    std::string storage_path_root;
    std::atomic<bool> currently_live_syncing{false};
    std::chrono::steady_clock::time_point last_reread_klines_from_disk_time =
        std::chrono::steady_clock::now();
    mutable std::shared_mutex mutex;
};

}  // namespace MarketData
