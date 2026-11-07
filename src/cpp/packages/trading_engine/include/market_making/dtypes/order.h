#pragma once

#include "market_making/dtypes/enums.h"
#include "market_making/dtypes/order_fill_info.h"
#include "market_making/dtypes/prediction.h"
#include "mpreal.h"

namespace Core {

struct OrderResNormalized;
struct OrderFillInfo;
class Engine;

struct Order {
    OrderType type;
    OrderSide side;

    std::string status;
    std::string symbol;
    std::string exchange_name;
    std::string market_type;
    Exchange exchange_enum = Exchange::UNKNOWN;
    Market market_enum = Market::UNKNOWN;

    std::string quote_asset;
    std::string base_asset;

    long long create_timestamp;
    long long cancel_timestamp = 0;
    long long last_fill_timestamp = 0;
    long long ack_create_timestamp = 0;
    long long ack_state_removal = 0;
    long long expires_on_ms;
    long long sending_is_valid_for_deadline = 0;
    long long server_sending_deadline_ms = 0;
    long long account_id = 0;

    long long last_cancel_request_time_ms = 0;
    long long cancel_retries = 0;
    long long first_cancel_request_time_ms = 0;

    mpfr::mpreal price;
    mpfr::mpreal quantity;
    std::string price_str;
    std::string quantity_str;
    mpfr::mpreal filled_quantity;
    mpfr::mpreal sizing_coeff_delta;

    unsigned long long id;
    std::string id_str;
    unsigned long long signal_id = 0;
    std::string client_order_id = "";
    std::string exchange_order_id = "";
    std::optional<unsigned long long> trade_id;
    std::optional<Signal> prediction;
    int strategy_id;

    bool reduce_only = false;
    bool allowed_to_bypass_risk_engine = false;
    bool is_trade_close_order = false;
    bool post_only = false;
    bool is_taker_in_simulation = false;
    bool is_order_creation_acknowledged = false;
    bool needs_rest_api_fetch = false;
    OrderTypeInternal type_internal = OrderTypeInternal::DEFAULT;
    bool is_alt_market_routed = false;
    std::optional<std::string> route_order_to_alt_symbol = std::nullopt;
    std::optional<std::string> routed_order_base_asset = std::nullopt;
    std::optional<std::string> routed_order_quote_asset = std::nullopt;
    bool margin_auto_borrow = false;
    bool margin_auto_repay = false;
    std::string derivatives_settle_currency = "USDT";

    std::optional<OrderCancelReason> cancel_reason;
    std::vector<OrderFillInfo> fills;

    std::unordered_map<std::string, double> kv_cache = {};
    std::optional<unsigned long long> hedge_for_trade_id = std::nullopt;
    std::optional<PositionSide> position_side = std::nullopt;

    long long valid_in_backtest_from_timestamp = 0;
    long long removed_in_backtest_at_timestamp = 0;

    bool is_prediction_market_order = false;
    std::string clob_id = "";
    std::string outcome_name = "";
    bool is_polymarket_order_res_success = false;
    std::optional<MarkoutConfig> markout_config = std::nullopt;

    static Order create(
        const std::string &symbol, OrderType type, OrderSide side,
        const mpfr::mpreal &price, const std::optional<Signal> &prediction,
        const mpfr::mpreal &quantity, const std::string &exchange_name,
        const std::string &market_type, int strategy_id,
        long long expires_on_ms, const mpfr::mpreal &sizing_coeff_delta,
        std::optional<unsigned long long> trade_id, bool reduce_only,
        long long timestamp, const std::string &quote_asset,
        const std::string &base_asset, bool allowed_to_bypass_risk_engine,
        bool is_trade_close_order, OrderTypeInternal type_internal,
        bool is_alt_market_routed,
        const std::optional<std::string> &route_order_to_alt_symbol,
        const std::optional<std::string> &routed_order_base_asset,
        const std::optional<std::string> &routed_order_quote_asset,
        bool margin_auto_borrow, bool margin_auto_repay,
        const std::string &derivatives_settle_currency,
        const std::unordered_map<std::string, double> &kv_cache,
        std::optional<unsigned long long> hedge_for_trade_id,
        long long account_id,
        const std::optional<Core::PositionSide> position_side,
        bool is_prediction_market_order = false,
        const std::string &clob_id = "", const std::string &outcome_name = "",
        const std::optional<MarkoutConfig> &markout_config = std::nullopt);

    nlohmann::json to_json() const;

    nlohmann::json to_ccxt_proxy_format() const;
    static Order from_json(const nlohmann::json &json);

    std::string get_order_api_symbol() const;
    bool uses_exchange_order_id_for_lookup() const;
    std::string get_order_lookup_id() const;

    OrderFillInfo from_order_res_normalized(
        Engine &engine, const OrderResNormalized &order_res);

    std::string get_key() const;

    std::string get_potentially_alt_routed_symbol() const;
    std::string get_potentially_alt_routed_base_asset() const;
    std::string get_potentially_alt_routed_quote_asset() const;

    std::string get_backtest_quote_asset() const;

    bool operator==(const Order &other) const {
        return this->id == other.id && this->symbol == other.symbol &&
               this->exchange_name == other.exchange_name &&
               this->market_type == other.market_type &&
               this->type == other.type && this->side == other.side &&
               this->status == other.status;
    }

    bool operator!=(const Order &other) const { return !(*this == other); }
};

}  // namespace Core