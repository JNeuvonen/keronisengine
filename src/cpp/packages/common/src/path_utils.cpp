#include "path_utils.h"

#include "binance_utils.h"

BacktestMarketDatasetMeta CommonUtils::get_aggr_trades_dataset_meta(
    const std::string &path) {
    BacktestMarketDatasetMeta meta;

    std::vector<std::string> parts;
    std::istringstream path_stream(path);
    std::string part;

    while (std::getline(path_stream, part, '/')) {
        parts.push_back(part);
    }

    if (parts.size() >= 6) {
        meta.exchange = parts[parts.size() - 6];     // binance
        meta.symbol = parts[parts.size() - 3];       // SOLUSDT
        meta.market_type = parts[parts.size() - 2];  // futures

        if (meta.market_type == "futures") {
            meta.market_type = "swap";
        }
        return meta;
    }

    return meta;
}

BacktestMarketDatasetMeta CommonUtils::get_ohlcv_klines_dataset_meta(
    const std::string &path) {
    // e.g. <root>/datasets/binance/15m/swap/SOLUSDT_15m
    BacktestMarketDatasetMeta meta;

    std::vector<std::string> parts;
    std::istringstream path_stream(path);
    std::string part;

    while (std::getline(path_stream, part, '/')) {
        parts.push_back(part);
    }

    if (parts.size() >= 6) {
        meta.exchange = parts[parts.size() - 4];  // binance

        // Split the symbol at '_' and take first part
        std::string full_symbol = parts[parts.size() - 1];  // SOLUSDT_15m
        size_t underscore_pos = full_symbol.find('_');
        meta.symbol = underscore_pos != std::string::npos
                          ? full_symbol.substr(0, underscore_pos)  // SOLUSDT
                          : full_symbol;

        meta.market_type = parts[parts.size() - 2];   // swap
        meta.interval_str = parts[parts.size() - 3];  // 15m
        meta.interval_ms =
            BinaUtils::convert_interval_str_to_ms(meta.interval_str.value());

        if (meta.market_type == "futures") {
            meta.market_type = "swap";
        }
    }

    return meta;
}
