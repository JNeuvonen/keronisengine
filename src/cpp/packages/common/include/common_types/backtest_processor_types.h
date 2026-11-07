#pragma once

#include <nlohmann/json.hpp>
#include <unordered_map>
#include <variant>
#include <vector>

using json = nlohmann::json;

using ValueVariant = std::variant<int64_t, double>;
using ColToValueMap = std::unordered_map<std::string, ValueVariant>;
using DbColumnsVector = std::vector<ColToValueMap>;

struct StrategyDecisionTick {
    long long timestamp_ms;
    char sizing_coefficient[5];
    uint16_t trading_order;
};

struct PredictionTick {
    long long timestamp_ms;
    double prediction;
    double close_price;
};

using ColNameToValueMap =
    std::unordered_map<std::string, std::variant<long long, double>>;

struct BacktestMarketDatasetMeta {
    std::string symbol;
    std::string exchange;
    std::string market_type;
    std::optional<std::string> interval_str;
    std::optional<long long> interval_ms;
};