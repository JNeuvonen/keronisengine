#include <catch2/catch_all.hpp>

#include "live_infra/exchange_specific/hyperliquid.h"
#include "market_making/dtypes/constants.h"
#include "market_making/dtypes/order_res.h"

namespace {

std::string order_update(const std::string& status, const std::string& sz,
                         const std::string& orig_sz) {
    return R"({"channel":"orderUpdates","data":[{"order":{"coin":"XRP",)"
           R"("side":"B","limitPx":"0.5","sz":")" +
           sz + R"(","oid":42,"timestamp":1700000005000,"origSz":")" + orig_sz +
           R"("},"status":")" + status + R"(","statusTimestamp":1700000005000}]})";
}

}  // namespace

TEST_CASE("HL parse_order_trade_update ignores non-order channels",
          "[hyperliquid][account_streams]") {
    REQUIRE(Live::HyperliquidExchange::parse_order_trade_update("not json")
                .empty());
    REQUIRE(Live::HyperliquidExchange::parse_order_trade_update(
                R"({"channel":"subscriptionResponse","data":{}})")
                .empty());
    REQUIRE(Live::HyperliquidExchange::parse_order_trade_update(
                R"({"channel":"userFills","data":{"fills":[]}})")
                .empty());
}

TEST_CASE("HL parse_order_trade_update maps status + cumulative fill",
          "[hyperliquid][account_streams]") {
    SECTION("open with partial fill -> cumulative filled = origSz - sz") {
        auto r = Live::HyperliquidExchange::parse_order_trade_update(
            order_update("open", "0.4", "1.0"));
        REQUIRE(r.size() == 1);
        REQUIRE(r[0].symbol == "XRPUSDC");
        REQUIRE(r[0].exchange_order_id == "42");
        REQUIRE(r[0].status == "open");
        REQUIRE(r[0].price == mpfr::mpreal("0.5"));
        REQUIRE(r[0].orig_quantity == mpfr::mpreal("1.0"));
        REQUIRE(r[0].filled_quantity == mpfr::mpreal("0.6"));
        REQUIRE(r[0].timestamp == 1700000005000LL);
    }

    SECTION("filled -> NOT_WORKING, fully filled") {
        auto r = Live::HyperliquidExchange::parse_order_trade_update(
            order_update("filled", "0", "1.0"));
        REQUIRE(r.size() == 1);
        REQUIRE(r[0].status == Core::ORDER_STATUS_NOT_WORKING);
        REQUIRE(r[0].filled_quantity == mpfr::mpreal("1.0"));
    }

    SECTION("canceled variants map to canceled") {
        REQUIRE(Live::HyperliquidExchange::parse_order_trade_update(
                    order_update("canceled", "1.0", "1.0"))[0]
                    .status == "canceled");
        REQUIRE(Live::HyperliquidExchange::parse_order_trade_update(
                    order_update("marginCanceled", "1.0", "1.0"))[0]
                    .status == "canceled");
        REQUIRE(Live::HyperliquidExchange::parse_order_trade_update(
                    order_update("rejected", "1.0", "1.0"))[0]
                    .status == "canceled");
    }
}

TEST_CASE("HL parse_order_trade_update handles batched updates",
          "[hyperliquid][account_streams]") {
    std::string msg =
        R"({"channel":"orderUpdates","data":[)"
        R"({"order":{"coin":"XRP","side":"B","limitPx":"0.5","sz":"1","oid":1,"timestamp":1,"origSz":"1"},"status":"open"},)"
        R"({"order":{"coin":"BTC","side":"A","limitPx":"60000","sz":"0","oid":2,"timestamp":2,"origSz":"1"},"status":"filled"}]})";
    auto r = Live::HyperliquidExchange::parse_order_trade_update(msg);
    REQUIRE(r.size() == 2);
    REQUIRE(r[0].exchange_order_id == "1");
    REQUIRE(r[0].status == "open");
    REQUIRE(r[1].exchange_order_id == "2");
    REQUIRE(r[1].status == Core::ORDER_STATUS_NOT_WORKING);
}
