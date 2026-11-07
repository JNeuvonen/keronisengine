#include "market_data/dataset_manager.h"

#include "market_data/ohlcv_dataset.h"
#include "market_data/utils.h"

namespace MarketData {

DatasetsManager& DatasetsManager::get_instance() {
    static DatasetsManager instance;
    return instance;
}

void DatasetsManager::add_ohlcv_dataset(
    const std::shared_ptr<OhlcvDataset>& dataset) {
    std::lock_guard<std::mutex> lock(store_mutex);
    if (added_dataset_pointers.find(dataset) != added_dataset_pointers.end()) {
        return;
    }
    store.add_ohlcv_dataset(
        ::MarketData::get_derived_bars_base_dir(
            dataset->meta.symbol, dataset->meta.exchange_name,
            dataset->meta.market_type, dataset->meta.bar_close_policy, true),
        dataset);
    added_dataset_pointers.insert(dataset);
}

std::vector<std::shared_ptr<OhlcvDataset>>
DatasetsManager::get_ohlcv_datasets() {
    std::lock_guard<std::mutex> lock(store_mutex);
    return store.get_ohlcv_datasets();
}

// store_mutex is released before any per-dataset lock is taken, so this never
// holds two market-data locks at once.
std::vector<DatasetSummary> DatasetsManager::build_dataset_summaries() {
    const auto datasets = get_ohlcv_datasets();
    std::vector<DatasetSummary> summaries;
    summaries.reserve(datasets.size());
    for (const auto& dataset : datasets) {
        if (dataset == nullptr) continue;
        summaries.push_back(dataset->build_summary());
    }
    return summaries;
}

std::optional<std::shared_ptr<OhlcvDataset>> DatasetsManager::get_ohlcv_dataset(
    const std::string& key) {
    std::lock_guard<std::mutex> lock(store_mutex);
    return store.get_ohlcv_dataset(key);
}

std::shared_ptr<OhlcvDataset> DatasetsManager::get_or_create_ohlcv_dataset(
    const OhlcvBarMeta& meta) {
    const std::string key = ::MarketData::get_derived_bars_base_dir(
        meta.symbol, meta.exchange_name, meta.market_type,
        meta.bar_close_policy, true);
    {
        std::lock_guard<std::mutex> lock(store_mutex);
        auto existing = store.get_ohlcv_dataset(key);
        if (existing.has_value()) {
            return *existing;
        }
    }
    // read_data_on_init=false: batch-ingest workflows write per-day slices
    // and rely on the parquet partition writer's row-count-skip guard in
    // write_ohlcv_ticks_partitioned for dedup. Loading full history on every
    // construction would be O(history) per ticker per day — fatal for
    // multi-year batch runs across 11k+ tickers.
    auto dataset = std::make_shared<OhlcvDataset>(
        meta, ChronoUtils::make_timepoint(2000, 1, 1, 0, 0, 0),
        /*read_data_on_init=*/false,
        /*can_fetch_from_proxy=*/false);
    dataset->mark_init_sync_done();
    {
        std::lock_guard<std::mutex> lock(store_mutex);
        auto existing = store.get_ohlcv_dataset(key);
        if (existing.has_value()) {
            return *existing;
        }
        store.add_ohlcv_dataset(key, dataset);
        added_dataset_pointers.insert(dataset);
    }
    return dataset;
}

void DatasetsManager::add_ohlcv_tick(const BarFlushInfo& bar_flush_info) {
    std::shared_ptr<OhlcvDataset> dataset;
    {
        std::lock_guard<std::mutex> lock(store_mutex);
        auto existing = store.get_ohlcv_dataset(bar_flush_info.bar_identifier);
        if (!existing.has_value()) {
            return;
        }
        dataset = *existing;
    }
    dataset->add_new_kline(bar_flush_info.tick);
}

void DatasetsManager::rm_unused_klines(const std::string& key,
                                       long long max_klines_to_keep) {
    store.rm_unused_klines(key, max_klines_to_keep);
}

void DatasetsManager::remove_all_ohlcv_datasets_from_state() {
    store.remove_all_ohlcv_datasets();
    added_dataset_pointers.clear();
}

void DatasetsManager::sync_ohlcv_datasets_to_disk() {
    std::unordered_set<std::shared_ptr<OhlcvDataset>> datasets_to_sync;
    {
        std::lock_guard<std::mutex> lock(store_mutex);
        if (is_syncing_to_disk.load()) {
            return;
        }
        is_syncing_to_disk.store(true);
        datasets_to_sync = added_dataset_pointers;
    }
    for (const auto& dataset : datasets_to_sync) {
        if (dataset->is_disk_dirty()) {
            dataset->sync_to_disk();
        }
    }
    is_syncing_to_disk.store(false);
}

Store DatasetsManager::copy_store() {
    std::lock_guard<std::mutex> lock(store_mutex);
    return store;
}

Store& DatasetsManager::get_store() {
    std::lock_guard<std::mutex> lock(store_mutex);
    return store;
}

}  // namespace MarketData
