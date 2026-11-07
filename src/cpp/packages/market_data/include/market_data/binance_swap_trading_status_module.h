#pragma once

#include <fstream>
#include <optional>
#include <stdexcept>
#include <vector>

#include <nlohmann/json.hpp>

#include "market_data/alt_data_module.h"

namespace MarketData {

struct TradingStatusEvent {
    long long available_ms = 0;
    long long effective_ms = 0;
    long long settlement_ms = 0;
    bool entry_allowed = true;
};

class BinanceSwapTradingStatusModule final
    : public FileBackedAltDataModule<std::vector<TradingStatusEvent>> {
   public:
    AltDataSource source() const override { return AltDataSource::BinanceSwapTradingStatus; }

    static bool permits_entry(const std::vector<TradingStatusEvent>& events, long long at_ms) {
        bool allowed = true;
        for (const auto& event : events) {
            if (event.available_ms <= at_ms && event.effective_ms <= at_ms) {
                allowed = event.entry_allowed;
            }
        }
        return allowed;
    }

    std::optional<bool> entry_allowed(const std::string& symbol, long long at_ms) const {
        const auto entry = snapshot(symbol);
        if (entry == nullptr) return std::nullopt;
        return permits_entry(entry->payload, at_ms);
    }

    std::optional<long long> announced_exit(const std::string& symbol, long long at_ms) const {
        const auto entry = snapshot(symbol);
        if (entry == nullptr) return std::nullopt;
        std::optional<long long> exit;
        for (const auto& event : entry->payload) {
            if (event.available_ms > at_ms) continue;
            if (!event.entry_allowed && event.settlement_ms > 0) exit = event.effective_ms;
            if (event.entry_allowed && event.effective_ms <= at_ms) exit.reset();
        }
        return exit;
    }

   protected:
    std::string path_for(const std::string& symbol) const override {
        return alt_data_root() + "/trading_status/binance/swap/" + symbol + ".json";
    }

    std::vector<TradingStatusEvent> parse(const std::string& path) const override {
        std::ifstream input(path);
        nlohmann::json rows;
        input >> rows;
        std::vector<TradingStatusEvent> events;
        for (const auto& row : rows) {
            TradingStatusEvent event{row.at("available_ms"), row.at("effective_ms"),
                                     row.at("settlement_ms"), row.at("entry_allowed")};
            if (event.available_ms <= 0 || event.effective_ms <= 0 || event.settlement_ms < 0) {
                throw std::runtime_error("Invalid Binance trading-status timestamps");
            }
            events.push_back(event);
        }
        std::sort(events.begin(), events.end(), [](const auto& left, const auto& right) {
            if (left.effective_ms != right.effective_ms) return left.effective_ms < right.effective_ms;
            return left.available_ms < right.available_ms;
        });
        return events;
    }
};

}  // namespace MarketData
