#include <catch2/catch_all.hpp>

#include "live_infra/exchange_specific/aster.h"
#include "market_making/dtypes/constants.h"
#include "market_making/dtypes/order_res.h"

namespace {

std::string make_order_trade_update(const std::string& status,
                                    const std::string& avg_price = "0",
                                    const std::string& limit_price = "100") {
    return R"({"e":"ORDER_TRADE_UPDATE","o":{"s":"XRPUSDT","c":"cli-1","i":42,"X":")" +
           status + R"(","ap":")" + avg_price + R"(","p":")" + limit_price +
           R"(","q":"1","z":"0.5","T":1700000005000}})";
}

}  // namespace

TEST_CASE("parse_order_trade_update rejects invalid payloads",
          "[aster][account_streams]") {
    REQUIRE(Live::AsterExchange::parse_order_trade_update("not json").empty());
    REQUIRE(Live::AsterExchange::parse_order_trade_update(
                R"({"e":"ACCOUNT_UPDATE"})")
                .empty());
    REQUIRE(Live::AsterExchange::parse_order_trade_update(
                R"({"e":"ORDER_TRADE_UPDATE"})")
                .empty());
}

TEST_CASE("parse_order_trade_update maps Aster execution status",
          "[aster][account_streams]") {
    SECTION("NEW and PARTIALLY_FILLED are open") {
        auto open_new = Live::AsterExchange::parse_order_trade_update(
            make_order_trade_update("NEW"));
        REQUIRE(open_new.size() == 1);
        REQUIRE(open_new[0].status == "open");

        auto open_partial = Live::AsterExchange::parse_order_trade_update(
            make_order_trade_update("PARTIALLY_FILLED"));
        REQUIRE(open_partial.size() == 1);
        REQUIRE(open_partial[0].status == "open");
    }

    SECTION("FILLED is closed") {
        auto filled = Live::AsterExchange::parse_order_trade_update(
            make_order_trade_update("FILLED", "1.25"));
        REQUIRE(filled.size() == 1);
        REQUIRE(filled[0].status == Core::ORDER_STATUS_NOT_WORKING);
    }

    SECTION("CANCELED and EXPIRED are canceled") {
        auto canceled = Live::AsterExchange::parse_order_trade_update(
            make_order_trade_update("CANCELED"));
        REQUIRE(canceled.size() == 1);
        REQUIRE(canceled[0].status == "canceled");

        auto expired = Live::AsterExchange::parse_order_trade_update(
            make_order_trade_update("EXPIRED"));
        REQUIRE(expired.size() == 1);
        REQUIRE(expired[0].status == "canceled");
    }

    SECTION("unknown status passes through") {
        auto rejected = Live::AsterExchange::parse_order_trade_update(
            make_order_trade_update("REJECTED"));
        REQUIRE(rejected.size() == 1);
        REQUIRE(rejected[0].status == "REJECTED");
    }
}

TEST_CASE("parse_order_trade_update maps fields and price fallback",
          "[aster][account_streams]") {
    SECTION("uses average price when non-zero") {
        auto res = Live::AsterExchange::parse_order_trade_update(
            make_order_trade_update("FILLED", "1.25", "100"));
        REQUIRE(res.size() == 1);
        REQUIRE(res[0].symbol == "XRPUSDT");
        REQUIRE(res[0].client_order_id == "cli-1");
        REQUIRE(res[0].exchange_order_id == "42");
        REQUIRE(res[0].price == mpfr::mpreal("1.25"));
        REQUIRE(res[0].orig_quantity == mpfr::mpreal("1"));
        REQUIRE(res[0].filled_quantity == mpfr::mpreal("0.5"));
        REQUIRE(res[0].timestamp == 1700000005000LL);
    }

    SECTION("falls back to limit price when average is zero") {
        auto res = Live::AsterExchange::parse_order_trade_update(
            make_order_trade_update("NEW", "0", "99.5"));
        REQUIRE(res.size() == 1);
        REQUIRE(res[0].price == mpfr::mpreal("99.5"));
    }
}
