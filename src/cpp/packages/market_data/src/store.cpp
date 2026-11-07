#include "market_data/store.h"

#include "market_making/engine.h"

namespace MarketData {

void Store::add_ohlcv_dataset(const std::string& key,
                              const std::shared_ptr<OhlcvDataset>& dataset) {
    if (ohlcv_datasets_map.find(key) != ohlcv_datasets_map.end()) {
        return;
    }
    ohlcv_datasets_map[key] = dataset;
    ohlcv_datasets_vec.push_back(dataset);
}

std::vector<std::shared_ptr<OhlcvDataset>> Store::get_ohlcv_datasets() {
    return ohlcv_datasets_vec;
}

std::optional<std::shared_ptr<OhlcvDataset>> Store::get_ohlcv_dataset(
    const std::string& key) {
    if (ohlcv_datasets_map.find(key) == ohlcv_datasets_map.end()) {
        return std::nullopt;
    }
    return ohlcv_datasets_map[key];
}

std::optional<std::shared_ptr<OhlcvDataset>> Store::get_synced_ohlcv_dataset(
    const std::string& key, bool is_backtest) {
    if (ohlcv_datasets_map.find(key) == ohlcv_datasets_map.end()) {
        return std::nullopt;
    }
    auto dataset = ohlcv_datasets_map[key];
    return dataset;
}

void Store::add_ohlcv_tick(const std::string& key,
                           const OhlcvDatasetTick& tick) {
    if (ohlcv_datasets_map.find(key) == ohlcv_datasets_map.end()) {
        return;
    }

    const auto& dataset = ohlcv_datasets_map[key];
    dataset->add_new_kline(tick);
}

void Store::remove_all_ohlcv_datasets() {
    ohlcv_datasets_map.clear();
    ohlcv_datasets_vec.clear();
}

void Store::rm_unused_klines(const std::string& key,
                             long long max_klines_to_keep) {
    if (ohlcv_datasets_map.find(key) == ohlcv_datasets_map.end()) {
        return;
    }
    ohlcv_datasets_map[key]->rm_unused_klines(max_klines_to_keep);
}

long Store::get_num_ready_datasets(Core::Engine& engine, long long interval) {
    long num_ready_datasets = 0;
    long long curr_time = engine.get_time();
    for (const auto& dataset : ohlcv_datasets_vec) {
        const auto latest_bar_close_time_ms =
            dataset->get_latest_bar_close_time_ms();
        if (!latest_bar_close_time_ms.has_value()) {
            continue;
        }
        if (ChronoUtils::is_two_ts_on_same_interval(
                curr_time, latest_bar_close_time_ms.value(), interval)) {
            num_ready_datasets++;
        }
    }
    return num_ready_datasets;
}

}  // namespace MarketData
