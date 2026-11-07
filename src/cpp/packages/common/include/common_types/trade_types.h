#pragma once

#include <nlohmann/json.hpp>
#include <string>

#include "mpreal.h"

using json = nlohmann::json;

struct BinanceLiveTrade {
    bool is_short_trade;
    long long kline_open_time_ms_on_entry;
    std::optional<long long> kline_open_time_ms_on_close;
    double open_price;
    std::optional<double> close_price;
    mpfr::mpreal base_size;
};

struct BinanceOrderFill {
    std::string status;
    std::string clientOrderId;
    mpfr::mpreal orig_qty;
    mpfr::mpreal exec_qty;
    double average_price;
    double average_price_after_fees;
    double filled_amount_quote;
    double commission;
};
