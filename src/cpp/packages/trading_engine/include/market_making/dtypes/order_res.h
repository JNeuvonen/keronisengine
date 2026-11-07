#pragma once

#include "mpreal.h"
#include "nlohmann/json.hpp"

namespace Core {

struct OrderResFees {
    std::string currency;
    mpfr::mpreal amount;

    nlohmann::json to_json() const;
    static OrderResFees from_json(const nlohmann::json &json);

    bool operator==(const OrderResFees &other) const {
        return currency == other.currency && amount == other.amount;
    }

    bool operator!=(const OrderResFees &other) const {
        return !(*this == other);
    }
};

// minimal order response normalized (crypto all exchanges, stocks etc)
struct OrderResNormalized {
    std::string symbol;
    std::string client_order_id;
    std::string exchange_order_id;
    std::string status;

    mpfr::mpreal price;
    mpfr::mpreal orig_quantity;
    mpfr::mpreal filled_quantity;

    long long timestamp;
    long long last_updated_timestamp;
    long long last_trade_timestamp;

    std::optional<OrderResFees> fee = std::nullopt;
    std::string taker_order_id = "";
    bool is_maker = true;

    std::string get_order_key(const std::string &exchange_name) const {
        if (exchange_name == "polymarket" && !exchange_order_id.empty()) {
            return exchange_order_id;
        }
        return client_order_id;
    }

    nlohmann::json to_json() const;
    void normalize_status();
    static OrderResNormalized from_ccxt_user_data_stream_msg(
        const nlohmann::json &order_json);

    bool operator==(const OrderResNormalized &other) const {
        return symbol == other.symbol &&
               client_order_id == other.client_order_id &&
               exchange_order_id == other.exchange_order_id &&
               status == other.status && price == other.price &&
               orig_quantity == other.orig_quantity &&
               filled_quantity == other.filled_quantity &&
               timestamp == other.timestamp &&
               last_updated_timestamp == other.last_updated_timestamp &&
               last_trade_timestamp == other.last_trade_timestamp &&
               fee == other.fee;
    }

    bool operator!=(const OrderResNormalized &other) const {
        return !(*this == other);
    }
};

}  // namespace Core