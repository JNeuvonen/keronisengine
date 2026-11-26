#include <catch2/catch_all.hpp>

#include "nlohmann/json.hpp"
#include "ohlcv_dataset_tick.h"

TEST_CASE("from_cpp_proxy_res reads kline fields given as strings or numbers",
          "[ohlcv][proxy]") {
    const auto json = nlohmann::json::parse(R"({"data": [
        [1700000000000, "294.72", "295.0", "294.0", "294.5", "12", 1700000059999, "3534.0", 7, "5", "1472.5", "0"],
        ["1700000060000", "294.5", "294.6", "294.4", "294.6", "3", "1700000119999", "883.8", "2", "1", "294.6", "0"]
    ]})");
    const auto ticks = MarketData::OhlcvDatasetTick::from_cpp_proxy_res(
        json, "binance", "swap", 60000);
    REQUIRE(ticks.size() == 2);
    CHECK(ticks[0].kline_open_time == 1700000000000LL);
    CHECK(ticks[0].close_price == 294.5);
    CHECK(ticks[0].number_of_trades == 7);
    CHECK(ticks[1].kline_open_time == 1700000060000LL);
    CHECK(ticks[1].number_of_trades == 2);
    CHECK(ticks[1].taker_buy_quote_volume == 294.6);
}
