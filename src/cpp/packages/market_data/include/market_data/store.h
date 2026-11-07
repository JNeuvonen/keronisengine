#pragma once
#include <unordered_map>

#include "market_data/ohlcv_dataset.h"
#include "market_making/dtypes/prediction.h"
#include "ohlcv_dataset_tick.h"

namespace Core {
class Engine;
}

namespace MarketData {

struct Store {
    std::unordered_map<std::string, std::shared_ptr<OhlcvDataset>>
        ohlcv_datasets_map;
    std::vector<std::shared_ptr<OhlcvDataset>> ohlcv_datasets_vec;

    long get_num_ready_datasets(Core::Engine& engine, long long interval);

    std::vector<std::shared_ptr<OhlcvDataset>> get_ohlcv_datasets();
    std::optional<std::shared_ptr<OhlcvDataset>> get_ohlcv_dataset(
        const std::string& key);
    std::optional<std::shared_ptr<OhlcvDataset>> get_synced_ohlcv_dataset(
        const std::string& key, bool is_backtest);
    void add_ohlcv_dataset(const std::string& key,
                           const std::shared_ptr<OhlcvDataset>& dataset);
    void add_ohlcv_tick(const std::string& key, const OhlcvDatasetTick& tick);
    void rm_unused_klines(const std::string& key, long long max_klines_to_keep);
    void remove_all_ohlcv_datasets();
};

}  // namespace MarketData