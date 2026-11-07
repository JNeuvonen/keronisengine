#include "market_making/dtypes/order_res.h"

#include "ccxt_utils.h"
#include "market_making/dtypes/constants.h"

nlohmann::json Core::OrderResFees::to_json() const {
    nlohmann::json json;
    json["currency"] = this->currency;
    json["amount"] = this->amount.toString();
    return json;
}

Core::OrderResFees Core::OrderResFees::from_json(const nlohmann::json &json) {
    Core::OrderResFees fees;
    fees.currency = json["currency"].get<std::string>();
    fees.amount = mpfr::mpreal(json["amount"].get<std::string>());
    return fees;
}

nlohmann::json Core::OrderResNormalized::to_json() const {
    nlohmann::json json;
    json["symbol"] = this->symbol;
    json["status"] = this->status;
    json["client_order_id"] = this->client_order_id;
    json["exchange_order_id"] = this->exchange_order_id;
    json["price"] = this->price.toString();
    json["orig_quantity"] = this->orig_quantity.toString();
    json["filled_quantity"] = this->filled_quantity.toString();
    json["timestamp"] = this->timestamp;
    json["last_updated_timestamp"] = this->last_updated_timestamp;
    json["last_trade_timestamp"] = this->last_trade_timestamp;
    if (this->fee.has_value()) {
        json["fee"] = this->fee.value().to_json();
    }
    if (!this->taker_order_id.empty()) {
        json["taker_order_id"] = this->taker_order_id;
    }
    return json;
}

void Core::OrderResNormalized::normalize_status() {
    if (this->status == "open") {
        this->status = ORDER_STATUS_WORKING;
    } else if (this->status == "closed") {
        this->status = ORDER_STATUS_NOT_WORKING;
    } else if (this->status == "canceled") {
        this->status = ORDER_STATUS_NOT_WORKING;
    } else if (this->status == "expired") {
        this->status = ORDER_STATUS_NOT_WORKING;
    } else if (this->status == "rejected") {
        this->status = ORDER_STATUS_NOT_WORKING;
    } else {
        this->status = ORDER_STATUS_NOT_WORKING;
    }
}

Core::OrderResNormalized
Core::OrderResNormalized::from_ccxt_user_data_stream_msg(
    const nlohmann::json &order_json) {
    Core::OrderResNormalized res;
    res.symbol =
        CCXTUtils::convert_ccxt_symbol(order_json["symbol"].get<std::string>());

    std::string client_order_id =
        order_json["clientOrderId"].get<std::string>();

    if (client_order_id.length() >= 2 && client_order_id.substr(0, 2) == "t-") {
        client_order_id = client_order_id.substr(2);
    }

    res.client_order_id = client_order_id;
    res.exchange_order_id = order_json["id"].get<std::string>();
    res.status = order_json["status"].get<std::string>();
    if (order_json.contains("average") && !order_json["average"].is_null()) {
        res.price = mpfr::mpreal(order_json["average"].get<double>());
    } else {
        res.price = mpfr::mpreal(order_json["price"].get<double>());
    }
    res.orig_quantity = mpfr::mpreal(order_json["amount"].get<double>());
    res.filled_quantity = mpfr::mpreal(order_json["filled"].get<double>());
    res.timestamp = order_json["timestamp"].get<long long>();

    if (order_json.contains("lastTradeTimestamp") &&
        !order_json["lastTradeTimestamp"].is_null()) {
        res.last_trade_timestamp =
            order_json["lastTradeTimestamp"].get<long long>();
    }

    if (order_json.contains("lastUpdateTimestamp") &&
        !order_json["lastUpdateTimestamp"].is_null()) {
        res.last_updated_timestamp =
            order_json["lastUpdateTimestamp"].get<long long>();
    }

    if (order_json.contains("fee") && order_json["fee"].is_object() &&
        order_json["fee"].contains("currency") &&
        order_json["fee"].contains("cost") &&
        !order_json["fee"]["currency"].is_null()) {
        res.fee = OrderResFees{
            .currency = order_json["fee"]["currency"].get<std::string>(),
            .amount = mpfr::mpreal(order_json["fee"]["cost"].get<double>()),
        };
    }

    if (order_json.contains("taker_order_id") &&
        !order_json["taker_order_id"].is_null()) {
        res.taker_order_id = order_json["taker_order_id"].get<std::string>();
    }

    res.normalize_status();
    return res;
}