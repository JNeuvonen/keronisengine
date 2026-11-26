#include <catch2/catch_test_macros.hpp>

#include "market_data/normalized_trade.h"
#include "simdjson.h"

TEST_CASE("binance swap @trade keeps the match time T beside the event time E", "[normalized_trade]") {
    simdjson::dom::parser parser;
    const simdjson::padded_string msg(std::string(
        R"({"e":"trade","E":1794826694123,"T":1794826694121,"s":"AKEUSDT","t":42,"p":"0.0012","q":"1000","X":"MARKET","m":true})"));
    const auto trade = MarketData::NormalizedParsedTrade::from_simdjson(parser.parse(msg).value(), "binance", "swap");
    CHECK(trade.timestamp == 1794826694123);
    CHECK(trade.trade_time_ms == 1794826694121);
    CHECK(trade.trade_id == "42");
}
