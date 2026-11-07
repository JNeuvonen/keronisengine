#pragma once

#include <string>

#include "common_types/backtest_processor_types.h"
#include "market_data/ohlcv_dataset.h"

namespace CommonUtils {

BacktestMarketDatasetMeta get_aggr_trades_dataset_meta(const std::string &path);

BacktestMarketDatasetMeta get_ohlcv_klines_dataset_meta(
    const std::string &path);

BacktestMarketDatasetMeta get_order_book_snapshots_dataset_meta(
    const std::string &path);

BacktestMarketDatasetMeta get_order_book_updates_dataset_meta(
    const std::string &path);

BacktestMarketDatasetMeta get_predictions_dataset_meta(const std::string &path);
}  // namespace CommonUtils
