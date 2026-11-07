#pragma once
#include <set>
#include <unordered_set>

#include "nlohmann/json.hpp"
#include "simdjson.h"

namespace arrow {
class Table;
}

namespace MarketData {
struct BarClosePolicy;

struct OhlcvDatasetTick {
    long long kline_open_time = 0;
    double close_price = 0;
    double open_price = 0;
    double high_price = 0;
    double low_price = 10000000000000000;
    double volume = 0;
    long long kline_close_time = 0;
    double quote_asset_volume = 0;
    long number_of_trades = 0;
    double taker_buy_base_volume = 0;
    double taker_buy_quote_volume = 0;
    bool is_closed = false;
    bool is_read_from_slow_storage = false;
    bool is_read_from_fast_storage = false;
    long long last_trade_timestamp_ms = 0;

    std::unordered_set<std::string> added_trade_ids;

    [[nodiscard]] nlohmann::json to_json() const;
    [[nodiscard]] bool is_closed_tick(
        const MarketData::BarClosePolicy& bar_close_policy,
        bool is_live_synced, long long buffer_ms) const;

    static std::vector<OhlcvDatasetTick> from_arrow_table(
        const std::shared_ptr<arrow::Table>& table);

    static std::vector<OhlcvDatasetTick> from_cpp_proxy_res(
        const nlohmann::json& json, const std::string& exchange_name,
        const std::string& market_type, long long interval_ms);

    static OhlcvDatasetTick from_simdjson(const simdjson::dom::element& data);
};
}  // namespace MarketData
