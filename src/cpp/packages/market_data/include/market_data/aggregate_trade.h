#pragma once
#include <string>

#include "nlohmann/json.hpp"

namespace MarketData {
struct NormalizedParsedTrade;

struct AggregateTrade {
    std::string symbol;
    std::string exchange_name;
    std::string market_type;
    double price;
    double quantity;
    long long first_id;
    long long last_id;
    long long timestamp;
    bool is_buyer_maker;

    static AggregateTrade from_json(const nlohmann::json& json,
                                    const std::string& symbol,
                                    const std::string& exchange_name,
                                    const std::string& market_type);
    std::vector<NormalizedParsedTrade> to_normalized_trades();
};

}  // namespace MarketData
