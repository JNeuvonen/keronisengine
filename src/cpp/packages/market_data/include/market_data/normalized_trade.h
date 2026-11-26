#pragma once

#include <string>

#include "nlohmann/json.hpp"
#include "simdjson.h"

namespace arrow {
class Table;
}

namespace MarketData {

struct NormalizedParsedTrade {
    std::string symbol;
    std::string exchange_name;
    std::string market_type;
    double price;
    double quantity;
    long long parsed_timestamp;
    long long timestamp;
    long long latency_ms;
    bool is_buyer_maker;
    std::string trade_id;
    long long trade_time_ms = 0;  // the venue's match time (Binance T); 0 when the feed has none

    nlohmann::json to_json() const;

    static NormalizedParsedTrade from_json(const nlohmann::json& json,
                                           const std::string& exchange_name,
                                           const std::string& market_type);
    static NormalizedParsedTrade from_simdjson(
        const simdjson::dom::element& json, const std::string& exchange_name,
        const std::string& market_type);

    static std::vector<NormalizedParsedTrade> from_arrow_table(
        const std::shared_ptr<arrow::Table>& table, const std::string& symbol,
        const std::string& exchange_name, const std::string& market_type);

    static std::vector<NormalizedParsedTrade> from_list_of_aggregated_trades(
        const nlohmann::json& trades, const std::string& symbol,
        const std::string& exchange_name, const std::string& market_type);

    static std::vector<NormalizedParsedTrade> from_list_of_gateio_trades(
        const nlohmann::json& trades, const std::string& symbol,
        const std::string& exchange_name, const std::string& market_type);

    static std::vector<NormalizedParsedTrade>
    from_list_of_binance_individual_trades(
        const nlohmann::json& trades, const std::string& symbol,
        const std::string& exchange_name, const std::string& market_type);

    [[nodiscard]] std::string get_key() const;
};

}  // namespace MarketData
