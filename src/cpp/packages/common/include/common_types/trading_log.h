#pragma once

#include <nlohmann/json.hpp>
#include <string>

#include "uuid.h"

using json = nlohmann::json;

struct TradingLog {
    std::string symbol;
    int32_t strategy_id;
    std::string log_level;
    std::string message;
    std::string event_type;
    json metadata;
    std::string source;
    int64_t event_link_id;
    int64_t timestamp;
    int64_t event_time_ns;
    std::string exchange_name;
    std::string market_type;
    bool is_trading_activity_log;

    static TradingLog create(const std::string& symbol, int32_t strategy_id,
                             const std::string& log_level,
                             const std::string& message,
                             const std::string& event_type,
                             const json& metadata, const std::string& source,
                             const std::string& exchange_name,
                             const std::string& market_type,
                             bool is_trading_activity_log = false) {
        TradingLog log;
        log.symbol = symbol;
        log.strategy_id = strategy_id;
        log.log_level = log_level;
        log.message = message;
        log.event_type = event_type;
        log.metadata = metadata;
        log.source = source;
        log.event_link_id = Common::generate_uuid();
        log.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
        log.event_time_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
        log.exchange_name = exchange_name;
        log.market_type = market_type;
        log.is_trading_activity_log = is_trading_activity_log;
        return log;
    }

    json to_json() const {
        return {{"symbol", symbol},
                {"strategy_id", strategy_id},
                {"log_level", log_level},
                {"message", message},
                {"event_type", event_type},
                {"metadata", metadata},
                {"source", source},
                {"event_link_id", event_link_id},
                {"timestamp", timestamp},
                {"event_time_ns", event_time_ns},
                {"exchange_name", exchange_name},
                {"market_type", market_type},
                {"is_trading_activity_log", is_trading_activity_log}};
    }
};