#include "market_data/us_equity_stock_splits_module.h"

#include "parquet_utils.h"

namespace MarketData {

std::string UsEquityStockSplitsModule::path_for(const std::string& key) const {
    return alt_data_root() + "/corporate_actions/splits/" + key + ".parquet";
}

std::vector<StockSplitEvent> UsEquityStockSplitsModule::parse(
    const std::string& path) const {
    return ParquetUtils::load_us_equity_stock_splits(path);
}

std::optional<std::vector<StockSplitEvent>> UsEquityStockSplitsModule::splits_as_of(
    const std::string& symbol, long long as_of_ms) const {
    const auto entry = snapshot(symbol);
    if (entry == nullptr) return std::nullopt;
    std::vector<StockSplitEvent> visible;
    for (const auto& event : entry->payload) {
        if (event.execution_ts_ms > as_of_ms) break;  // sorted oldest first
        visible.push_back(event);
    }
    return visible;
}

std::optional<double> UsEquityStockSplitsModule::cumulative_ratio(
    const std::string& symbol, long long from_ms, long long to_ms) const {
    const auto entry = snapshot(symbol);
    if (entry == nullptr) return std::nullopt;
    double ratio = 1.0;
    for (const auto& event : entry->payload) {
        if (event.execution_ts_ms > to_ms) break;
        if (event.execution_ts_ms <= from_ms) continue;
        ratio *= event.ratio;
    }
    return ratio;
}

}  // namespace MarketData
