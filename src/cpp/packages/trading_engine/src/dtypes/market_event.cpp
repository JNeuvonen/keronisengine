#include "market_making/dtypes/market_event.h"

nlohmann::json Core::MarketEvent::to_json() const {
    nlohmann::json json;
    json["type"] = this->type;

    if (sizing_coeff.has_value()) {
        json["sizing_coeff"] = sizing_coeff.value().toString();
    }

    if (symbol.has_value()) {
        json["symbol"] = symbol.value();
    }

    if (exchange_name.has_value()) {
        json["exchange_name"] = exchange_name.value();
    }

    if (market_type.has_value()) {
        json["market_type"] = market_type.value();
    }

    if (price.has_value()) {
        json["price"] = price.value();
    }

    if (strategy_id.has_value()) {
        json["strategy_id"] = strategy_id.value();
    }

    if (order_book.has_value()) {
        json["order_book"] = order_book.value().to_json();
    }

    if (prediction.has_value()) {
        json["prediction"] = prediction.value();
    }

    if (aggr_trade.has_value()) {
        json["aggr_trade"] = aggr_trade.value().to_json();
    }

    if (bar_flush_info.has_value()) {
        json["bar_flush_info"] = bar_flush_info.value().to_json();
    }

    json["event_time"] = this->event_time;

    return json;
}