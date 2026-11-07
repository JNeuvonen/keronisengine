#include <iostream>
#include <thread>

#include "catch2/catch_all.hpp"
#include "market_data/bar_close_policy.h"
#include "market_data/ohlcv_bar_meta.h"
#include "market_data/utils.h"

TEST_CASE("Test market data utils", "[test_bar_type_parsing]") {
    auto bar_type = MarketData::parse_bar_type_from_path(
        "/derived/bars/binance/swap/time/900000_ms/IOTAUSDT/parquets");

    try {
        const auto& derived_bars_base_dir =
            MarketData::get_derived_bars_base_dir("IOTAUSDT", "binance", "swap",
                                                  bar_type, true);
        std::cout << derived_bars_base_dir << std::endl;
    } catch (const std::exception& e) {
        std::cout << "Error: " << e.what() << std::endl;
    }
}
TEST_CASE("Extended trading hours window", "[test_extended_trading_hours]") {
    // 2026-08-20 is a Thursday; EDT is UTC-4.
    const long long thursday_0359_et = 1787212740000LL;  // 07:59 UTC
    const long long thursday_0400_et = 1787212800000LL;  // 08:00 UTC
    const long long thursday_1959_et = 1787270340000LL;  // 23:59 UTC
    const long long thursday_2000_et = 1787270400000LL;  // 2026-08-21 00:00 UTC

    REQUIRE_FALSE(
        MarketData::is_time_ms_within_extended_trading_hours(thursday_0359_et));
    REQUIRE(
        MarketData::is_time_ms_within_extended_trading_hours(thursday_0400_et));
    REQUIRE(
        MarketData::is_time_ms_within_extended_trading_hours(thursday_1959_et));
    REQUIRE_FALSE(
        MarketData::is_time_ms_within_extended_trading_hours(thursday_2000_et));

    // 2026-08-22 is a Saturday — closed all day even at midday ET.
    const long long saturday_midday_et = 1787414400000LL;
    REQUIRE_FALSE(MarketData::is_time_ms_within_extended_trading_hours(
        saturday_midday_et));

    // 2026-01-15 is a Thursday in EST (UTC-5): 09:00 UTC is 04:00 ET, so the
    // window must follow the DST offset rather than a fixed UTC hour.
    const long long january_0400_et = 1768467600000LL;
    const long long january_0359_et = january_0400_et - 60000LL;
    REQUIRE(
        MarketData::is_time_ms_within_extended_trading_hours(january_0400_et));
    REQUIRE_FALSE(
        MarketData::is_time_ms_within_extended_trading_hours(january_0359_et));
}

TEST_CASE("Venue kline ceiling", "[test_max_klines_to_keep]") {
    MarketData::OhlcvBarMeta polygon_1m;
    polygon_1m.exchange_name = "polygon";
    polygon_1m.market_type = "stock";
    polygon_1m.bar_close_policy.is_time_bar = true;
    polygon_1m.bar_close_policy.is_exchange_bar = true;
    polygon_1m.bar_close_policy.time_ms = 60000;
    REQUIRE(MarketData::get_max_klines_to_keep(polygon_1m) == 5000);

    MarketData::OhlcvBarMeta polygon_1d = polygon_1m;
    polygon_1d.bar_close_policy.time_ms = 86400000;
    REQUIRE(MarketData::get_max_klines_to_keep(polygon_1d) == 1000);

    // Crypto venues keep today's behaviour: no venue ceiling.
    MarketData::OhlcvBarMeta binance_1m = polygon_1m;
    binance_1m.exchange_name = "binance";
    binance_1m.market_type = "swap";
    REQUIRE(MarketData::get_max_klines_to_keep(binance_1m) == 0);
}
