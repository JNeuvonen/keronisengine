#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "market_making/dtypes/inventory.h"
#include "market_making/dtypes/order.h"
#include "market_making/dtypes/orderbook.h"
#include "market_making/dtypes/prediction.h"
#include "market_making/dtypes/trade.h"
#include "nlohmann/json.hpp"

namespace Core {

struct TelemetryBook {
    std::string symbol;
    std::string exchange_name;
    std::string market_type;
    OrderBookLevels levels;

    nlohmann::json to_json() const {
        nlohmann::json j;
        j["symbol"] = symbol;
        j["exchange_name"] = exchange_name;
        j["market_type"] = market_type;
        j["levels"] = levels.to_json();
        return j;
    }
};

struct StrategyTelemetryEvent {
    int strategy_id = 0;
    uint64_t timestamp_us = 0;

    std::vector<Order> orders;
    std::vector<Trade> trades;
    std::vector<Signal> signals;
    std::vector<Inventory> inventories;
    std::vector<TelemetryBook> books;

    nlohmann::json payload;

    nlohmann::json to_json() const {
        nlohmann::json j;
        j["strategy_id"] = strategy_id;
        j["timestamp_us"] = timestamp_us;

        j["orders"] = nlohmann::json::array();
        for (const auto& o : orders) {
            j["orders"].push_back(o.to_json());
        }

        j["trades"] = nlohmann::json::array();
        for (const auto& t : trades) {
            j["trades"].push_back(t.to_json());
        }

        j["signals"] = nlohmann::json::array();
        for (const auto& s : signals) {
            j["signals"].push_back(s.to_json());
        }

        j["inventories"] = nlohmann::json::array();
        for (const auto& inv : inventories) {
            j["inventories"].push_back(inv.to_json());
        }

        j["books"] = nlohmann::json::array();
        for (const auto& b : books) {
            j["books"].push_back(b.to_json());
        }

        if (!payload.is_null()) {
            j["payload"] = payload;
        }

        return j;
    }
};

}  // namespace Core
