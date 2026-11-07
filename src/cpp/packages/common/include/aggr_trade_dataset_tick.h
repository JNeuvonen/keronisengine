#pragma once

#include <nlohmann/json.hpp>

struct AggrTradeDatasetTick {
    long long timestamp;
    double price;
    double quantity;
    long long trade_id;
    bool is_buyer_maker;

    nlohmann::json to_json() const;
};