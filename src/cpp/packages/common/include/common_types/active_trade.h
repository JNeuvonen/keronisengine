#pragma once

#include "mpreal.h"
#include "nlohmann/json.hpp"

using json = nlohmann::json;

struct ActiveTradeLimitOrder {
    int id;
    int active_trade_id;
    bool is_enter_order;
    mpfr::mpreal filled_qty;
    long long order_id;

    nlohmann::json to_json() const {
        return {{"id", id},
                {"active_trade_id", active_trade_id},
                {"is_enter_order", is_enter_order},
                {"filled_qty", filled_qty.toString()},
                {"order_id", order_id}};
    }
};

struct ActiveTrade {
    int id;
    int trade_ticker_id;
    mpfr::mpreal sizing_coeff_delta;
    mpfr::mpreal filled_size;
    mpfr::mpreal orig_size;
    mpfr::mpreal open_price;
    long long trade_open_time_ms;
    std::string exchange_id;
    std::string market_type;

    nlohmann::json to_json() const {
        return {{"id", id},
                {"sizing_coeff_delta", sizing_coeff_delta.toString()},
                {"trade_ticker_id", trade_ticker_id},
                {"filled_size", filled_size.toString()},
                {"orig_size", orig_size.toString()},
                {"open_price", open_price.toString()},
                {"trade_open_time_ms", trade_open_time_ms},
                {"exchange_id", exchange_id},
                {"market_type", market_type}};
    }

    static ActiveTrade from_json(const nlohmann::json& j) {
        ActiveTrade trade;
        trade.id = j.at("id").get<int>();
        trade.trade_ticker_id = j.at("trade_ticker_id").get<int>();
        trade.filled_size =
            mpfr::mpreal(j.at("filled_size").get<std::string>());
        trade.orig_size = mpfr::mpreal(j.at("orig_size").get<std::string>());
        trade.open_price = mpfr::mpreal(j.at("open_price").get<std::string>());
        trade.trade_open_time_ms = j.at("trade_open_time_ms").get<long long>();
        trade.exchange_id = j.at("exchange_id").get<std::string>();
        trade.market_type = j.at("market_type").get<std::string>();
        return trade;
    }
};