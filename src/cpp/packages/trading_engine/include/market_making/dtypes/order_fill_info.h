#pragma once

#include "market_making/dtypes/def_only_structs.h"
#include "market_making/dtypes/enums.h"
#include "market_making/dtypes/order_res.h"
#include "market_making/dtypes/prediction.h"
#include "mpreal.h"
#include "nlohmann/json.hpp"

namespace Core {

class Engine;

struct OrderFillInfo {
    std::string symbol;
    std::string exchange_name;
    std::string market_type;
    mpfr::mpreal price;
    mpfr::mpreal fill_amount;
    mpfr::mpreal orig_order_full_size;
    mpfr::mpreal orig_order_sizing_coeff_delta;
    long long account_id;
    OrderSide side;
    int strategy_id;
    unsigned long long order_id;
    unsigned long long order_fill_id;
    long long order_created_timestamp;
    long long order_fill_timestamp;
    mpfr::mpreal sizing_coeff_delta;
    std::optional<Signal> prediction;
    std::optional<unsigned long long> trade_id;
    std::optional<OrderResFees> fee;
    std::unordered_map<std::string, double> kv_cache;
    std::optional<unsigned long long> hedge_for_trade_id;
    std::optional<std::string> hedge_symbol = std::nullopt;
    bool is_fee_paid_in_base_asset = false;
    std::string clob_token_id = "";
    TradeCreationType trade_creation_type = TradeCreationType::ORGANIC;

    bool is_polymarket_fill_confirmed = false;
    mpfr::mpreal fill_delta_applied = 0;
    std::string taker_order_id = "";
    bool is_maker = true;
    bool is_order_cancelled_before_fill = false;
    long long order_cancel_timestamp = 0;
    std::optional<MarkoutConfig> markout_config = std::nullopt;

    nlohmann::json to_json() const;
    nlohmann::json to_db_json() const;
    static OrderFillInfo from_json(const nlohmann::json &json);

    std::string get_rounding_symbol() const;

    bool operator==(const OrderFillInfo &other) const {
        return order_fill_id == other.order_fill_id &&
               order_id == other.order_id;
    }

    bool operator!=(const OrderFillInfo &other) const {
        return !(*this == other);
    }
};

}  // namespace Core
