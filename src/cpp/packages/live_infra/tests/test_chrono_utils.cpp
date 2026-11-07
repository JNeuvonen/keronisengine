#include <catch2/catch_all.hpp>
#include <iostream>

#include "market_data/utils.h"

TEST_CASE("test_chrono_utils", "[test_market_open_time_ms]") {
    std::optional<long long> market_open_time_ms =
        MarketData::get_market_open_time_ms(1759387665000);
    REQUIRE(market_open_time_ms.has_value());
    std::cout << "market_open_time_ms: " << market_open_time_ms.value()
              << std::endl;

    std::optional<long long> market_close_time_ms =
        MarketData::get_market_close_time_ms(1759387665000);
    REQUIRE(market_close_time_ms.has_value());
    std::cout << "market_close_time_ms: " << market_close_time_ms.value()
              << std::endl;
}