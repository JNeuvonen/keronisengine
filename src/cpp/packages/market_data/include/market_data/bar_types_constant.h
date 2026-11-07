#pragma once

#include <vector>

#include "market_data/bar_close_policy.h"
#include "market_making/dtypes/def_only_structs.h"

namespace MarketData {

// Individual bar policy constants
const BarClosePolicy ONE_MIN_EXCHANGE_BAR =
    BarClosePolicy::create(false, false, true, true, 0, 0, 60000);
const BarClosePolicy FIVE_MIN_EXCHANGE_BAR =
    BarClosePolicy::create(false, false, true, true, 0, 0, 60000 * 5);
const BarClosePolicy FIFTEEN_MIN_EXCHANGE_BAR =
    BarClosePolicy::create(false, false, true, true, 0, 0, 900000);
const BarClosePolicy FOUR_HOURS_EXCHANGE_BAR =
    BarClosePolicy::create(false, false, true, true, 0, 0, 900000 * 4 * 4);
const BarClosePolicy ONE_DAY_EXCHANGE_BAR =
    BarClosePolicy::create(false, false, true, true, 0, 0, 86400000);

const std::vector<BarClosePolicy> DEFAULT_LIVE_STREAMED_BARS = {
    BarClosePolicy::create(false, false, true, false, 0, 0, 900000),  // 15min
};

const std::vector<BarClosePolicy> BACKTEST_ORDER_FILL_BARS = {
    BarClosePolicy::create(false, false, true, false, 0, 0, 2000,
                           true),  // 2 sec
};

const BarClosePolicy MS_100_TIME_BAR = BarClosePolicy::create(
    false, false, true, false, 0, 0, 100, false);  // 100 ms

const BarClosePolicy ONE_SEC_TIME_BAR = BarClosePolicy::create(
    false, false, true, false, 0, 0, 1000, false);  // 1 sec

const BarClosePolicy ONE_MIN_TIME_BAR = BarClosePolicy::create(
    false, false, true, false, 0, 0, 60000, false);  // 1 min

const BarClosePolicy FIVE_SEC_TIME_BAR = BarClosePolicy::create(
    false, false, true, false, 0, 0, 5000, false);  // 5 sec

const BarClosePolicy TEN_SEC_TIME_BAR = BarClosePolicy::create(
    false, false, true, false, 0, 0, 10000, false);  // 10 sec

const BarClosePolicy FIFTEEN_SEC_TIME_BAR = BarClosePolicy::create(
    false, false, true, false, 0, 0, 15000, false);  // 15 sec

const BarClosePolicy FIFTEEN_MINUTES_TIME_BAR = BarClosePolicy::create(
    false, false, true, false, 0, 0, 900000, false);  // 15 min

const std::vector<Core::ExchangeAndMarket> DEFAULT_LIVE_STREAMED_EXCHANGES = {
    {"binance", "swap"}, {"binance", "spot"},  {"bybit", "swap"},
    {"bybit", "spot"},   {"polygon", "stock"}, {"gateio", "spot"},
};

const std::vector<Core::ExchangeAndMarket> MARKET_DATA_EXCHANGES_OF_INTEREST = {
    {"binance", "swap"}, {"binance", "spot"},     {"bybit", "swap"},
    {"bybit", "spot"},   {"polygon", "stock"},    {"gateio", "spot"},
    {"aster", "swap"},   {"hyperliquid", "swap"},
};

}  // namespace MarketData