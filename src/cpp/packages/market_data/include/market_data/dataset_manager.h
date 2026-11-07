#pragma once

#include <mutex>
#include <unordered_set>

#include "market_data/bar.h"
#include "market_data/dataset_summary.h"
#include "market_data/ohlcv_bar_meta.h"
#include "market_data/store.h"

struct OhlcvDataset;

namespace MarketData {

class DatasetsManager {
   public:
    static DatasetsManager& get_instance();
    void add_ohlcv_dataset(const std::shared_ptr<OhlcvDataset>& dataset);
    std::shared_ptr<OhlcvDataset> get_or_create_ohlcv_dataset(
        const OhlcvBarMeta& meta);
    std::optional<std::shared_ptr<OhlcvDataset>> get_ohlcv_dataset(
        const std::string& key);
    void add_ohlcv_tick(const BarFlushInfo& bar_flush_info);
    void rm_unused_klines(const std::string& key, long long max_klines_to_keep);
    void remove_all_ohlcv_datasets_from_state();
    void sync_ohlcv_datasets_to_disk();
    std::vector<std::shared_ptr<OhlcvDataset>> get_ohlcv_datasets();
    std::vector<DatasetSummary> build_dataset_summaries();
    Store copy_store();
    Store& get_store();

    // Make destructor public
    ~DatasetsManager() = default;

   private:
    DatasetsManager() = default;

    // Delete copy constructor and assignment operator
    DatasetsManager(const DatasetsManager&) = delete;
    DatasetsManager& operator=(const DatasetsManager&) = delete;

    std::unordered_set<std::shared_ptr<OhlcvDataset>> added_dataset_pointers;
    std::atomic<bool> is_syncing_to_disk{false};

    Store store;
    std::mutex store_mutex;
};

}  // namespace MarketData
