#pragma once

#include <optional>

#include "market_making/dtypes/market_data_dataset.h"
#include "nlohmann/json.hpp"

namespace Core {

struct PredictionDataset {
    std::string path;
    std::string model_name;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(PredictionDataset, path, model_name);
};

struct PolymarketConfig {
    std::vector<std::string> series_slugs;
    // Shared ".../prediction_markets/polymarket/series/" dir. Each series' DB is
    // <series_base_dir>/<slug>/store.db, resolved inside load_polymarket_markets.
    std::string series_base_dir;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(PolymarketConfig, series_slugs,
                                                series_base_dir);
};

struct OhlcvUniverseSelection {
    std::string mode;
    std::vector<std::string> symbols;
    std::string namespace_name;
    std::string symbol_group;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        OhlcvUniverseSelection, mode, symbols, namespace_name, symbol_group);
};

struct OhlcvDatasetSelection {
    std::string dataset_root;
    std::string exchange_name;
    std::string market_type;
    long long interval_ms = 60000;
    OhlcvUniverseSelection universe;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        OhlcvDatasetSelection, dataset_root, exchange_name, market_type,
        interval_ms, universe);
};

struct MarketData {
    std::vector<MarketDataDataset> aggr_trades;
    std::vector<MarketDataDataset> trades;
    std::vector<MarketDataDataset> ohlcv_klines;
    std::vector<MarketDataDataset> order_book_snapshots;
    std::vector<MarketDataDataset> order_book_updates;
    std::vector<PredictionDataset> predictions;
    PolymarketConfig polymarket;
    std::optional<OhlcvDatasetSelection> ohlcv_dataset_selection;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(MarketData, aggr_trades, trades,
                                                ohlcv_klines,
                                                order_book_snapshots,
                                                order_book_updates, predictions,
                                                polymarket,
                                                ohlcv_dataset_selection);
    nlohmann::json to_json() const;
};

}  // namespace Core
