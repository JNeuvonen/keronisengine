#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "market_data/market_event.h"
#include "nlohmann/json.hpp"

namespace MarketData {

struct SMMarketDataDependency {
    std::string symbol;
    std::string exchange_name;
    std::string market_type;
    MarketEvent::Type event_type;

    std::atomic<bool> is_synced{false};
    long long first_live_event_ts_ms = 0;
    long long last_synced_ts_ms = 0;
    std::string last_synced_trade_id;
    std::vector<MarketEvent> buffer;
    // Declared-but-may-be-empty deps (e.g., a symbol not listed on the exchange
    // anymore, or a pair we track optimistically). Optional deps don't block
    // the live sync gate and are treated as synced at prepare_live.
    bool is_optional = false;
    static constexpr size_t MAX_BUFFER_SIZE = 100000;

    // Non-copyable due to atomic — must stay in place in map
    SMMarketDataDependency() = default;
    SMMarketDataDependency(const SMMarketDataDependency&) = delete;
    SMMarketDataDependency& operator=(const SMMarketDataDependency&) = delete;
    SMMarketDataDependency(SMMarketDataDependency&& o) noexcept
        : symbol(std::move(o.symbol)),
          exchange_name(std::move(o.exchange_name)),
          market_type(std::move(o.market_type)),
          event_type(o.event_type),
          is_synced(o.is_synced.load()),
          first_live_event_ts_ms(o.first_live_event_ts_ms),
          last_synced_ts_ms(o.last_synced_ts_ms),
          last_synced_trade_id(std::move(o.last_synced_trade_id)),
          buffer(std::move(o.buffer)),
          is_optional(o.is_optional) {}
    SMMarketDataDependency& operator=(SMMarketDataDependency&& o) noexcept {
        symbol = std::move(o.symbol);
        exchange_name = std::move(o.exchange_name);
        market_type = std::move(o.market_type);
        event_type = o.event_type;
        is_synced.store(o.is_synced.load());
        first_live_event_ts_ms = o.first_live_event_ts_ms;
        last_synced_ts_ms = o.last_synced_ts_ms;
        last_synced_trade_id = std::move(o.last_synced_trade_id);
        buffer = std::move(o.buffer);
        is_optional = o.is_optional;
        return *this;
    }

    // State mutation helpers
    void mark_synced() { is_synced.store(true, std::memory_order_release); }
    void reset_sync() {
        is_synced.store(false, std::memory_order_release);
        first_live_event_ts_ms = 0;
        buffer.clear();
    }
    void set_first_live_event(long long ts) {
        if (first_live_event_ts_ms == 0) first_live_event_ts_ms = ts;
    }
    void advance_synced_ts(long long ts) {
        if (ts > last_synced_ts_ms) last_synced_ts_ms = ts;
    }
    bool check_synced() const {
        return is_synced.load(std::memory_order_acquire);
    }

    static std::string event_type_to_str(MarketEvent::Type type) {
        switch (type) {
            case MarketEvent::TRADE:
                return "TRADE";
            case MarketEvent::BBA:
                return "BBA";
            case MarketEvent::L2_UPDATE:
                return "L2_UPDATE";
            case MarketEvent::FUNDING:
                return "FUNDING";
            case MarketEvent::OHLCV:
                return "OHLCV";
            default:
                return "UNKNOWN";
        }
    }

    static MarketEvent::Type event_type_from_str(const std::string& s) {
        if (s == "TRADE") return MarketEvent::TRADE;
        if (s == "BBA") return MarketEvent::BBA;
        if (s == "L2_UPDATE") return MarketEvent::L2_UPDATE;
        if (s == "FUNDING") return MarketEvent::FUNDING;
        if (s == "OHLCV") return MarketEvent::OHLCV;
        return MarketEvent::TRADE;
    }

    static std::string make_key(const std::string& symbol,
                                const std::string& exchange_name,
                                const std::string& market_type,
                                MarketEvent::Type event_type) {
        return symbol + ":" + exchange_name + ":" + market_type + ":" +
               event_type_to_str(event_type);
    }

    std::string get_key() const {
        return make_key(symbol, exchange_name, market_type, event_type);
    }

    nlohmann::json to_json() const {
        return {{"symbol", symbol},
                {"exchange_name", exchange_name},
                {"market_type", market_type},
                {"event_type", event_type_to_str(event_type)},
                {"first_live_event_ts_ms", first_live_event_ts_ms},
                {"last_synced_ts_ms", last_synced_ts_ms},
                {"last_synced_trade_id", last_synced_trade_id},
                {"is_synced", is_synced.load()},
                {"is_optional", is_optional}};
    }

    static SMMarketDataDependency from_json(const nlohmann::json& j) {
        SMMarketDataDependency dep;
        dep.symbol = j.at("symbol").get<std::string>();
        dep.exchange_name = j.at("exchange_name").get<std::string>();
        dep.market_type = j.at("market_type").get<std::string>();
        dep.event_type =
            event_type_from_str(j.at("event_type").get<std::string>());
        dep.first_live_event_ts_ms =
            j.value("first_live_event_ts_ms", 0LL);
        dep.last_synced_ts_ms = j.value("last_synced_ts_ms", 0LL);
        dep.last_synced_trade_id =
            j.value("last_synced_trade_id", std::string{});
        dep.is_synced.store(j.value("is_synced", false));
        dep.is_optional = j.value("is_optional", false);
        return dep;
    }
};

inline long long get_event_timestamp_ms(const MarketEvent& event) {
    switch (event.type) {
        case MarketEvent::TRADE:
            return event.trade->timestamp_ms;
        case MarketEvent::BBA:
            return event.bba->timestamp_ms;
        case MarketEvent::L2_UPDATE:
            return event.l2_update->update.timestamp;
        case MarketEvent::FUNDING:
            return event.funding->timestamp_ms;
        case MarketEvent::POLYMARKET_BBA:
            return event.polymarket_bba->timestamp_ms;
        case MarketEvent::OHLCV:
            return event.ohlcv->kline_close_time_ms;
        default:
            return 0;
    }
}

inline std::string get_event_dependency_key(const MarketEvent& event) {
    switch (event.type) {
        case MarketEvent::TRADE:
            return SMMarketDataDependency::make_key(
                event.trade->symbol, event.trade->exchange_name,
                event.trade->market_type, MarketEvent::TRADE);
        case MarketEvent::BBA:
            return SMMarketDataDependency::make_key(
                event.bba->symbol, event.bba->exchange_name,
                event.bba->market_type, MarketEvent::BBA);
        case MarketEvent::L2_UPDATE:
            return SMMarketDataDependency::make_key(
                event.l2_update->symbol, event.l2_update->exchange_name,
                event.l2_update->market_type, MarketEvent::L2_UPDATE);
        case MarketEvent::FUNDING:
            return SMMarketDataDependency::make_key(
                event.funding->symbol, event.funding->exchange_name,
                event.funding->market_type, MarketEvent::FUNDING);
        case MarketEvent::OHLCV:
            return SMMarketDataDependency::make_key(
                event.ohlcv->symbol, event.ohlcv->exchange_name,
                event.ohlcv->market_type, MarketEvent::OHLCV);
        default:
            return "";
    }
}

}  // namespace MarketData
