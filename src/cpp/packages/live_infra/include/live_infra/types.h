#pragma once
#include <memory>
#include <unordered_map>

#include "market_making/dtypes/trade.h"
#include "nlohmann/json.hpp"
#include "trading/account.h"
#include "ws_stream.h"

// Add these before the Live namespace
namespace nlohmann {
template <>
struct adl_serializer<Core::Order> {
    static void to_json(json& j, const Core::Order& order) {
        j = order.to_json();
    }

    static Core::Order from_json(const json& j) {
        return Core::Order::from_json(j);
    }
};

template <>
struct adl_serializer<Core::Trade> {
    static void to_json(json& j, const Core::Trade& trade) {
        j = trade.to_json();
    }

    static Core::Trade from_json(const json& j) {
        return Core::Trade::from_json(j);
    }
};

template <>
struct adl_serializer<Core::OrderFillInfo> {
    static void to_json(json& j, const Core::OrderFillInfo& fill) {
        j = fill.to_json();
    }

    static Core::OrderFillInfo from_json(const json& j) {
        return Core::OrderFillInfo::from_json(j);
    }
};

}  // namespace nlohmann

namespace Live {
struct TradingState {
    std::vector<Core::Order> orders;
    std::vector<Core::Trade> trades;

    std::unordered_map<std::string, std::vector<Core::Trade>> new_trades;
    std::unordered_map<std::string, std::vector<Core::Order>> new_orders;
    std::unordered_map<std::string, std::vector<Core::Order>>
        new_orders_cancels;
    std::unordered_map<std::string, std::vector<Core::OrderFillInfo>>
        new_order_fills;
    std::unordered_map<std::string, std::vector<Core::Order>>
        tracked_misc_orders;

    nlohmann::json discretionary_settlements = nlohmann::json::object();

    friend void to_json(nlohmann::json& j, const TradingState& state) {
        j = {{"orders",state.orders}, {"trades",state.trades}, {"new_trades",state.new_trades}, {"new_orders",state.new_orders}, {"new_orders_cancels",state.new_orders_cancels}, {"new_order_fills",state.new_order_fills}, {"tracked_misc_orders",state.tracked_misc_orders}, {"discretionary_settlements",state.discretionary_settlements}};
    }

    friend void from_json(const nlohmann::json& j, TradingState& state) {
        j.at("orders").get_to(state.orders);
        j.at("trades").get_to(state.trades);
        j.at("new_trades").get_to(state.new_trades);
        j.at("new_orders").get_to(state.new_orders);
        j.at("new_orders_cancels").get_to(state.new_orders_cancels);
        j.at("new_order_fills").get_to(state.new_order_fills);
        j.at("tracked_misc_orders").get_to(state.tracked_misc_orders);
        state.discretionary_settlements = j.value("discretionary_settlements",nlohmann::json::object());
    }

    bool operator==(const TradingState& other) const {
        if (discretionary_settlements != other.discretionary_settlements) return false;
        if (orders.size() != other.orders.size() ||
            trades.size() != other.trades.size()) {
            return false;
        }

        if (new_trades.size() != other.new_trades.size() ||
            new_orders.size() != other.new_orders.size() ||
            new_orders_cancels.size() != other.new_orders_cancels.size() ||
            new_order_fills.size() != other.new_order_fills.size()) {
            return false;
        }

        for (const auto& order : orders) {
            bool found = false;
            for (const auto& other_order : other.orders) {
                if (order == other_order) {
                    found = true;
                    break;
                }
            }
            if (!found) return false;
        }

        for (const auto& trade : trades) {
            bool found = false;
            for (const auto& other_trade : other.trades) {
                if (trade == other_trade) {
                    found = true;
                    break;
                }
            }
            if (!found) return false;
        }

        for (const auto& [key, trades_vec] : new_trades) {
            auto it = other.new_trades.find(key);
            if (it == other.new_trades.end() ||
                trades_vec.size() != it->second.size()) {
                return false;
            }
            for (const auto& trade : trades_vec) {
                bool found = false;
                for (const auto& other_trade : it->second) {
                    if (trade == other_trade) {
                        found = true;
                        break;
                    }
                }
                if (!found) return false;
            }
        }

        for (const auto& [key, orders_vec] : new_orders) {
            auto it = other.new_orders.find(key);
            if (it == other.new_orders.end() ||
                orders_vec.size() != it->second.size()) {
                return false;
            }
            for (const auto& order : orders_vec) {
                bool found = false;
                for (const auto& other_order : it->second) {
                    if (order == other_order) {
                        found = true;
                        break;
                    }
                }
                if (!found) return false;
            }
        }

        for (const auto& [key, orders_vec] : new_orders_cancels) {
            auto it = other.new_orders_cancels.find(key);
            if (it == other.new_orders_cancels.end() ||
                orders_vec.size() != it->second.size()) {
                return false;
            }
            for (const auto& order : orders_vec) {
                bool found = false;
                for (const auto& other_order : it->second) {
                    if (order == other_order) {
                        found = true;
                        break;
                    }
                }
                if (!found) return false;
            }
        }

        for (const auto& [key, fills_vec] : new_order_fills) {
            auto it = other.new_order_fills.find(key);
            if (it == other.new_order_fills.end() ||
                fills_vec.size() != it->second.size()) {
                return false;
            }
            for (const auto& fill : fills_vec) {
                bool found = false;
                for (const auto& other_fill : it->second) {
                    if (fill == other_fill) {
                        found = true;
                        break;
                    }
                }
                if (!found) return false;
            }
        }

        for (const auto& [key, orders_vec] : tracked_misc_orders) {
            auto it = other.tracked_misc_orders.find(key);
            if (it == other.tracked_misc_orders.end() ||
                orders_vec.size() != it->second.size()) {
                return false;
            }
            for (const auto& order : orders_vec) {
                bool found = false;
                for (const auto& other_order : it->second) {
                    if (order == other_order) {
                        found = true;
                        break;
                    }
                }
                if (!found) return false;
            }
        }

        return true;
    }

    bool operator!=(const TradingState& other) const {
        return !(*this == other);
    }

    nlohmann::json to_json() const { return nlohmann::json(*this); }
};

struct PrivateStream {
    std::unique_ptr<Trading::Account> account;
    std::unique_ptr<WsStream> stream;
};

}  // namespace Live