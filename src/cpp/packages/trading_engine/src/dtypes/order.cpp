#include "market_making/dtypes/order.h"

#include "generic_utils.h"
#include "market_making/dtypes/constants.h"
#include "market_making/dtypes/enums.h"
#include "market_making/dtypes/order_res.h"
#include "market_making/dtypes/symbol_precision.h"
#include "market_making/engine.h"
#include "str_math.h"
#include "uuid.h"

Core::Order Core::Order::create(
    const std::string &symbol, OrderType type, OrderSide side,
    const mpfr::mpreal &price, const std::optional<Signal> &prediction,
    const mpfr::mpreal &quantity, const std::string &exchange_name,
    const std::string &market_type, int strategy_id, long long expires_on_ms,
    const mpfr::mpreal &sizing_coeff_delta,
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
    std::optional<unsigned long long> hedge_for_trade_id, long long account_id,
    const std::optional<Core::PositionSide> position_side,
    bool is_prediction_market_order, const std::string &clob_id,
    const std::string &outcome_name,
    const std::optional<Core::MarkoutConfig> &markout_config) {
    Order order;
    order.status = ORDER_STATUS_WORKING;
    order.symbol = symbol;
    order.type = type;
    order.side = side;
    order.price = price;
    order.quantity = quantity;
    order.filled_quantity = 0;
    order.exchange_name = exchange_name;
    order.exchange_enum = exchange_from_str(exchange_name);
    order.prediction = prediction;
    order.market_type = market_type;
    order.market_enum = market_from_str(market_type);
    order.strategy_id = strategy_id;
    order.id = Common::generate_timestamp_uuid();
    order.client_order_id = exchange_name == "gateio"
                                ? Common::hash_to_26char(order.id)
                                : Common::hash_to_32bit(order.id);
    order.create_timestamp = timestamp;
    order.expires_on_ms = expires_on_ms;
    order.sizing_coeff_delta = sizing_coeff_delta;
    order.trade_id = trade_id;
    order.reduce_only = reduce_only;
    order.quote_asset = quote_asset;
    order.base_asset = base_asset;
    order.allowed_to_bypass_risk_engine = allowed_to_bypass_risk_engine;
    order.is_trade_close_order = is_trade_close_order;
    order.signal_id = prediction.has_value() ? prediction.value().signal_id : 0;
    order.type_internal = type_internal;

    if (is_alt_market_routed) {
        if (!route_order_to_alt_symbol.has_value() ||
            !routed_order_base_asset.has_value() ||
            !routed_order_quote_asset.has_value()) {
            throw std::runtime_error(
                "is_alt_market_routed is true but route_order_to_alt_symbol, "
                "routed_order_base_asset, or routed_order_quote_asset is not "
                "set");
        }
    }
    order.is_alt_market_routed = is_alt_market_routed;
    order.route_order_to_alt_symbol = route_order_to_alt_symbol;
    order.routed_order_base_asset = routed_order_base_asset;
    order.routed_order_quote_asset = routed_order_quote_asset;
    order.margin_auto_borrow = margin_auto_borrow;
    order.margin_auto_repay = margin_auto_repay;
    order.derivatives_settle_currency = derivatives_settle_currency;
    order.kv_cache = kv_cache;
    order.hedge_for_trade_id = hedge_for_trade_id;
    order.account_id = account_id;
    order.position_side = position_side;
    order.is_prediction_market_order = is_prediction_market_order;
    order.clob_id = clob_id;
    order.outcome_name = outcome_name;
    order.markout_config = markout_config;
    return order;
}

static Core::OrderTypeInternal get_order_type_internal(
    const std::string &type_internal_str) {
    return Core::order_type_internal_from_str(type_internal_str);
}

static std::string get_order_type_internal_str(
    const Core::OrderTypeInternal &type_internal) {
    return Core::order_type_internal_to_str(type_internal);
}

static std::string get_position_side_str(
    const Core::PositionSide &position_side) {
    if (position_side == Core::PositionSide::LONG_SIDE) {
        return "LONG";
    } else if (position_side == Core::PositionSide::SHORT_SIDE) {
        return "SHORT";
    }

    throw std::runtime_error("Invalid position side: " +
                             std::to_string(position_side));
}

static Core::PositionSide get_position_side(
    const std::string &position_side_str) {
    if (position_side_str == "LONG") {
        return Core::PositionSide::LONG_SIDE;
    } else if (position_side_str == "SHORT") {
        return Core::PositionSide::SHORT_SIDE;
    }
    throw std::runtime_error("Invalid position side: " + position_side_str);
}

static std::string get_order_type_str(const Core::OrderType &type) {
    if (type == Core::OrderType::LIMIT) {
        return "limit";
    } else if (type == Core::OrderType::MARKET) {
        return "market";
    } else if (type == Core::OrderType::LIMIT_MAKER) {
        return "limit_maker";
    } else if (type == Core::OrderType::FILL_OR_KILL) {
        return "fill_or_kill";
    } else if (type == Core::OrderType::IMMEDIATE_OR_CANCEL) {
        return "immediate_or_cancel";
    }
    throw std::runtime_error("Invalid order type: " + std::to_string(type));
}

Core::OrderType get_order_type(const std::string &type_str) {
    if (type_str == "limit") {
        return Core::OrderType::LIMIT;
    } else if (type_str == "market") {
        return Core::OrderType::MARKET;
    } else if (type_str == "limit_maker") {
        return Core::OrderType::LIMIT_MAKER;
    } else if (type_str == "fill_or_kill") {
        return Core::OrderType::FILL_OR_KILL;
    } else if (type_str == "immediate_or_cancel") {
        return Core::OrderType::IMMEDIATE_OR_CANCEL;
    }
    throw std::runtime_error("Invalid order type: " + type_str);
}

nlohmann::json Core::Order::to_json() const {
    nlohmann::json json;
    json["id"] = this->id;
    json["id_str"] = this->id_str;
    json["symbol"] = this->symbol;
    json["type"] = get_order_type_str(this->type);
    json["side"] = this->side == OrderSide::BUY ? "BUY" : "SELL";
    json["price"] = this->price_str;
    json["quantity"] = this->quantity_str;
    json["filled_quantity"] = StrMath::to_fixed_string(
        this->filled_quantity,
        SymbolPrecisionAccess::getInstance().get_qty_precision(
            this->symbol, this->exchange_name, this->market_type));
    json["client_order_id"] = this->client_order_id;

    if (this->prediction.has_value()) {
        json["prediction"] = this->prediction.value().to_json();
    }

    json["exchange_name"] = this->exchange_name;
    json["market_type"] = this->market_type;
    json["post_only"] = this->post_only;
    json["create_timestamp"] = this->create_timestamp;
    json["cancel_timestamp"] = this->cancel_timestamp;
    json["last_fill_timestamp"] = this->last_fill_timestamp;
    json["ack_create_timestamp"] = this->ack_create_timestamp;
    json["ack_state_removal"] = this->ack_state_removal;
    json["exchange_order_id"] = this->exchange_order_id;
    json["expires_on_ms"] = this->expires_on_ms;
    json["strategy_id"] = this->strategy_id;
    json["sizing_coeff_delta"] = this->sizing_coeff_delta.toString();
    json["trade_id"] = this->trade_id.has_value()
                           ? std::to_string(this->trade_id.value())
                           : "";
    json["reduce_only"] = this->reduce_only;
    json["quote_asset"] = this->quote_asset;
    json["base_asset"] = this->base_asset;
    json["account_id"] = this->account_id;
    json["status"] = this->status;
    json["is_order_creation_acknowledged"] =
        this->is_order_creation_acknowledged;
    json["last_cancel_request_time_ms"] = this->last_cancel_request_time_ms;
    json["cancel_retries"] = this->cancel_retries;
    json["first_cancel_request_time_ms"] = this->first_cancel_request_time_ms;
    json["is_trade_close_order"] = this->is_trade_close_order;
    json["allowed_to_bypass_risk_engine"] = this->allowed_to_bypass_risk_engine;
    json["signal_id"] = this->signal_id;
    json["type_internal"] = get_order_type_internal_str(this->type_internal);
    json["is_alt_market_routed"] = this->is_alt_market_routed;
    json["margin_auto_borrow"] = this->margin_auto_borrow;
    json["margin_auto_repay"] = this->margin_auto_repay;
    json["derivatives_settle_currency"] = this->derivatives_settle_currency;

    if (this->route_order_to_alt_symbol.has_value()) {
        json["route_order_to_alt_symbol"] =
            this->route_order_to_alt_symbol.value();
    }

    if (this->routed_order_base_asset.has_value()) {
        json["routed_order_base_asset"] = this->routed_order_base_asset.value();
    }
    if (this->routed_order_quote_asset.has_value()) {
        json["routed_order_quote_asset"] =
            this->routed_order_quote_asset.value();
    }

    auto fills_json = nlohmann::json::array();
    for (const auto &fill : this->fills) {
        fills_json.push_back(fill.to_json());
    }
    json["fills"] = fills_json;

    json["kv_cache"] = this->kv_cache;

    if (this->hedge_for_trade_id.has_value()) {
        json["hedge_for_trade_id"] = this->hedge_for_trade_id.value();
    }

    if (this->position_side.has_value()) {
        json["position_side"] =
            get_position_side_str(this->position_side.value());
    }

    json["is_prediction_market_order"] = this->is_prediction_market_order;
    json["clob_id"] = this->clob_id;
    json["outcome_name"] = this->outcome_name;
    json["server_sending_deadline_ms"] = this->server_sending_deadline_ms;
    json["is_polymarket_order_res_success"] = this->is_polymarket_order_res_success;

    if (this->markout_config.has_value()) {
        nlohmann::json mc;
        mc["duration_ms"] = this->markout_config->duration_ms;
        mc["interval_ms"] = this->markout_config->interval_ms;
        json["markout_config"] = mc;
    }

    return json;
}

nlohmann::json Core::Order::to_ccxt_proxy_format() const {
    auto res = this->to_json();
    if (this->type == OrderType::LIMIT) {
        res["type"] = "limit";
    } else if (this->type == OrderType::MARKET) {
        res["type"] = "market";
    } else if (this->type == OrderType::LIMIT_MAKER) {
        res["type"] = "limit_maker";
    } else if (this->type == OrderType::FILL_OR_KILL) {
        res["type"] = "fill_or_kill";
    } else if (this->type == OrderType::IMMEDIATE_OR_CANCEL) {
        res["type"] = "immediate_or_cancel";
    }

    if (this->side == OrderSide::BUY) {
        res["side"] = "buy";
    } else if (this->side == OrderSide::SELL) {
        res["side"] = "sell";
    }

    if (!this->is_prediction_market_order) {
        res["symbol"] = this->base_asset + "/" + this->quote_asset +
                        (this->market_type == "swap"
                             ? ":" + this->derivatives_settle_currency
                             : "");
    }

    if (this->is_alt_market_routed) {
        res["route_order_to_alt_symbol"] =
            this->routed_order_base_asset.value() + "/" +
            this->routed_order_quote_asset.value() +
            (this->market_type == "swap"
                 ? ":" + this->derivatives_settle_currency
                 : "");
    }

    res["price"] = this->price_str;
    res["quantity"] = this->quantity_str;

    if (this->position_side.has_value()) {
        res["position_side"] =
            get_position_side_str(this->position_side.value());
    }

    return res;
}

Core::Order Core::Order::from_json(const nlohmann::json &json) {
    Order order;
    order.id = json["id"].get<unsigned long long>();
    order.id_str = std::to_string(order.id);
    order.symbol = json["symbol"].get<std::string>();
    order.type = get_order_type(json["type"].get<std::string>());
    order.side = json["side"] == "BUY" ? OrderSide::BUY : OrderSide::SELL;
    order.price = mpfr::mpreal(json["price"].get<std::string>());
    order.price_str = json["price"].get<std::string>();
    order.quantity = mpfr::mpreal(json["quantity"].get<std::string>());
    order.quantity_str = json["quantity"].get<std::string>();
    order.filled_quantity =
        mpfr::mpreal(json["filled_quantity"].get<std::string>());
    order.sizing_coeff_delta =
        mpfr::mpreal(json["sizing_coeff_delta"].get<std::string>());
    order.client_order_id = json["client_order_id"].get<std::string>();
    order.trade_id = json["trade_id"].is_null() ||
                             json["trade_id"].get<std::string>().empty()
                         ? std::nullopt
                         : std::make_optional(std::stoull(
                               json["trade_id"].get<std::string>()));
    order.expires_on_ms = json["expires_on_ms"].get<long long>();
    order.reduce_only = json["reduce_only"].get<bool>();
    order.post_only = json["post_only"].get<bool>();
    order.strategy_id = json["strategy_id"].get<int>();
    order.prediction =
        !json.contains("prediction") || json["prediction"].is_null()
            ? std::nullopt
            : std::make_optional(Signal::from_json(json["prediction"]));
    order.cancel_reason = std::nullopt;
    order.exchange_name = json["exchange_name"].get<std::string>();
    order.create_timestamp = json["create_timestamp"].get<long long>();
    order.cancel_timestamp = json["cancel_timestamp"].get<long long>();
    order.last_fill_timestamp = json["last_fill_timestamp"].get<long long>();
    order.ack_create_timestamp = json["ack_create_timestamp"].get<long long>();
    order.ack_state_removal = json["ack_state_removal"].get<long long>();
    order.quote_asset = json["quote_asset"].get<std::string>();
    order.market_type = json["market_type"].get<std::string>();
    order.exchange_enum = exchange_from_str(order.exchange_name);
    order.market_enum = market_from_str(order.market_type);
    order.exchange_order_id = json["exchange_order_id"].get<std::string>();
    order.base_asset = json["base_asset"].get<std::string>();
    order.account_id = json["account_id"].get<long long>();
    order.status = json["status"].get<std::string>();
    order.is_order_creation_acknowledged =
        json["is_order_creation_acknowledged"].get<bool>();
    order.cancel_retries = json["cancel_retries"].get<long long>();
    order.last_cancel_request_time_ms =
        json["last_cancel_request_time_ms"].get<long long>();
    order.first_cancel_request_time_ms =
        json["first_cancel_request_time_ms"].get<long long>();
    order.is_trade_close_order = json["is_trade_close_order"].get<bool>();
    order.allowed_to_bypass_risk_engine =
        json["allowed_to_bypass_risk_engine"].get<bool>();

    order.signal_id = json.contains("signal_id")
                          ? json["signal_id"].get<unsigned long long>()
                          : 0;
    order.type_internal =
        json.contains("type_internal")
            ? get_order_type_internal(json["type_internal"].get<std::string>())
            : OrderTypeInternal::DEFAULT;
    order.is_alt_market_routed = json.contains("is_alt_market_routed")
                                     ? json["is_alt_market_routed"].get<bool>()
                                     : false;

    if (json.contains("route_order_to_alt_symbol")) {
        order.route_order_to_alt_symbol =
            json["route_order_to_alt_symbol"].get<std::string>();
    }

    if (json.contains("routed_order_base_asset")) {
        order.routed_order_base_asset =
            json["routed_order_base_asset"].get<std::string>();
    }
    if (json.contains("routed_order_quote_asset")) {
        order.routed_order_quote_asset =
            json["routed_order_quote_asset"].get<std::string>();
    }

    order.margin_auto_borrow = json.contains("margin_auto_borrow")
                                   ? json["margin_auto_borrow"].get<bool>()
                                   : false;
    order.margin_auto_repay = json.contains("margin_auto_repay")
                                  ? json["margin_auto_repay"].get<bool>()
                                  : false;
    order.derivatives_settle_currency =
        json.contains("derivatives_settle_currency")
            ? json["derivatives_settle_currency"].get<std::string>()
            : "USDT";

    auto fills_json = json["fills"];
    for (const auto &fill_json : fills_json) {
        order.fills.push_back(OrderFillInfo::from_json(fill_json));
    }

    if (json.contains("kv_cache") && !json["kv_cache"].is_null()) {
        order.kv_cache =
            json["kv_cache"].get<std::unordered_map<std::string, double>>();
    } else {
        order.kv_cache = {};
    }
    if (json.contains("hedge_for_trade_id") &&
        !json["hedge_for_trade_id"].is_null()) {
        order.hedge_for_trade_id = std::make_optional<unsigned long long>(
            json["hedge_for_trade_id"].get<unsigned long long>());
    } else {
        order.hedge_for_trade_id = std::nullopt;
    }

    if (json.contains("position_side") && !json["position_side"].is_null()) {
        order.position_side =
            get_position_side(json["position_side"].get<std::string>());
    } else {
        order.position_side = std::nullopt;
    }

    order.is_prediction_market_order =
        json.contains("is_prediction_market_order")
            ? json["is_prediction_market_order"].get<bool>()
            : false;
    order.clob_id =
        json.contains("clob_id") ? json["clob_id"].get<std::string>() : "";
    order.outcome_name = json.contains("outcome_name")
                             ? json["outcome_name"].get<std::string>()
                             : "";
    order.server_sending_deadline_ms =
        json.contains("server_sending_deadline_ms")
            ? json["server_sending_deadline_ms"].get<long long>()
            : 0;
    order.is_polymarket_order_res_success =
        json.contains("is_polymarket_order_res_success")
            ? json["is_polymarket_order_res_success"].get<bool>()
            : false;

    if (json.contains("markout_config") && !json["markout_config"].is_null()) {
        Core::MarkoutConfig mc;
        mc.duration_ms = json["markout_config"]["duration_ms"].get<long long>();
        mc.interval_ms = json["markout_config"]["interval_ms"].get<long long>();
        order.markout_config = mc;
    }

    return order;
}

Core::OrderFillInfo Core::Order::from_order_res_normalized(
    Core::Engine &engine, const OrderResNormalized &order_res) {
    OrderFillInfo fill_info;
    fill_info.symbol = this->symbol;
    fill_info.strategy_id = this->strategy_id;
    fill_info.exchange_name = this->exchange_name;
    fill_info.market_type = this->market_type;
    fill_info.order_fill_timestamp = order_res.last_trade_timestamp;
    fill_info.order_created_timestamp = this->create_timestamp;
    fill_info.order_id = this->id;
    fill_info.side = this->side;
    fill_info.account_id = this->account_id;
    const auto &filled_qty =
        engine.round_quantity(this->get_order_api_symbol(), this->exchange_name,
                              this->market_type, order_res.filled_quantity);
    mpfr::mpreal fill_delta = filled_qty - this->filled_quantity;
    fill_info.sizing_coeff_delta =
        (fill_delta / this->quantity) * this->sizing_coeff_delta;
    fill_info.price =
        engine.round_price(this->get_order_api_symbol(), this->exchange_name,
                           this->market_type, order_res.price);
    fill_info.fill_amount = filled_qty;
    fill_info.trade_id = this->trade_id;
    fill_info.orig_order_full_size = this->quantity;
    fill_info.orig_order_sizing_coeff_delta = this->sizing_coeff_delta;
    fill_info.order_fill_id = Common::generate_timestamp_uuid();
    fill_info.fee = order_res.fee;
    fill_info.prediction = this->prediction;
    fill_info.kv_cache = this->kv_cache;
    fill_info.hedge_for_trade_id = this->hedge_for_trade_id;
    fill_info.hedge_symbol = this->hedge_for_trade_id.has_value()
                                 ? this->route_order_to_alt_symbol
                                 : std::nullopt;
    fill_info.trade_creation_type =
        Core::trade_creation_type_for(this->type_internal);

    if (fill_info.fee.has_value()) {
        std::string fee_currency = fill_info.fee.value().currency;
        std::string base_asset = this->base_asset;
        CommonUtils::to_lowercase(fee_currency);
        CommonUtils::to_lowercase(base_asset);
        if (fee_currency == base_asset) {
            fill_info.is_fee_paid_in_base_asset = true;
        }
    }

    fill_info.is_maker = order_res.is_maker;
    fill_info.is_order_cancelled_before_fill = (this->cancel_timestamp > 0);
    fill_info.order_cancel_timestamp = this->cancel_timestamp;
    fill_info.markout_config = this->markout_config;

    if (!order_res.taker_order_id.empty()) {
        fill_info.taker_order_id = order_res.taker_order_id;
        fill_info.is_polymarket_fill_confirmed = false;
    }

    if (this->is_prediction_market_order) {
        fill_info.clob_token_id = this->clob_id;
    }

    return fill_info;
}

std::string Core::Order::get_key() const {
    return this->exchange_name + ":" + this->market_type + ":" + this->symbol;
}

std::string Core::Order::get_potentially_alt_routed_symbol() const {
    if (this->route_order_to_alt_symbol.has_value()) {
        return this->route_order_to_alt_symbol.value();
    }
    return this->symbol;
}

std::string Core::Order::get_potentially_alt_routed_base_asset() const {
    if (this->routed_order_base_asset.has_value()) {
        return this->routed_order_base_asset.value();
    }
    return this->base_asset;
}

std::string Core::Order::get_potentially_alt_routed_quote_asset() const {
    if (this->routed_order_quote_asset.has_value()) {
        return this->routed_order_quote_asset.value();
    }
    return this->quote_asset;
}

std::string Core::Order::get_order_api_symbol() const {
    if (this->route_order_to_alt_symbol) {
        if (!this->routed_order_base_asset.has_value() ||
            !this->routed_order_quote_asset.has_value()) {
            throw std::runtime_error(
                "routed_order_base_asset or routed_order_quote_asset is not "
                "set");
        }

        return this->routed_order_base_asset.value() +
               this->routed_order_quote_asset.value();
    }
    return this->symbol;
}

bool Core::Order::uses_exchange_order_id_for_lookup() const {
    // Polymarket signs/routes orders by exchange-assigned id. Every CEX
    // (Binance, Bybit, OKX, Bitget, Hyperliquid, Coinbase, Aster) keys lookups
    // off the clientOrderId we generated — the proxy's query-order handler
    // passes the URL `client_order_id` param straight to the CEX as the
    // client-id field, so we must hand it our local client_order_id.
    return this->is_prediction_market_order;
}

std::string Core::Order::get_order_lookup_id() const {
    if (this->uses_exchange_order_id_for_lookup()) {
        return this->exchange_order_id;
    }
    return this->client_order_id;
}

std::string Core::Order::get_backtest_quote_asset() const {
    if (exchange_name == "binance" && market_type == "swap") {
        return "USDC";
    }
    return this->quote_asset;
}