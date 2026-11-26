#include <catch2/catch_all.hpp>

#include "lighter_markets.h"
#include "market_data/depth_history.h"
#include "market_data/lighter_order_book.h"

using MarketData::DepthHistory;
using MarketData::DepthSnapshot;
using MarketData::LighterBookMessage;
using MarketData::LighterOrderBook;

namespace {

// Trimmed from GET /api/v1/orderBooks.
const char *kOrderBooks = R"({"code":200,"order_books":[
 {"symbol":"ETH","market_id":0,"market_type":"perp","status":"active",
  "min_base_amount":"0.0020","min_quote_amount":"10.000000",
  "supported_size_decimals":4,"supported_price_decimals":2,"is_frozen":false},
 {"symbol":"BTC","market_id":1,"market_type":"perp","status":"active",
  "min_base_amount":"0.00007","min_quote_amount":"10.000000",
  "supported_size_decimals":5,"supported_price_decimals":1,"is_frozen":false},
 {"symbol":"OLD","market_id":77,"market_type":"perp","status":"inactive",
  "min_base_amount":"1","min_quote_amount":"10",
  "supported_size_decimals":0,"supported_price_decimals":4,"is_frozen":false},
 {"symbol":"LIT/USDC","market_id":2049,"market_type":"spot","status":"active",
  "min_base_amount":"1.00","min_quote_amount":"0.001000",
  "supported_size_decimals":2,"supported_price_decimals":4,"is_frozen":false}
]})";

std::string book_message(const std::string &type, long long nonce,
                         long long begin_nonce, const std::string &bids,
                         const std::string &asks) {
    return R"({"channel":"order_book:1","offset":5,"order_book":{"code":0,"asks":)" +
           asks + R"(,"bids":)" + bids + R"(,"offset":5,"nonce":)" +
           std::to_string(nonce) + R"(,"begin_nonce":)" +
           std::to_string(begin_nonce) +
           R"(},"timestamp":1794489987002,"type":")" + type + R"("})";
}

LighterBookMessage parse(const std::string &msg) {
    simdjson::dom::parser parser;
    auto parsed = LighterBookMessage::parse(msg, parser);
    REQUIRE(parsed.has_value());
    return *parsed;
}

const std::string kSnapshot = book_message(
    "subscribed/order_book", 100, 0,
    R"([{"price":"72596.4","size":"0.33772"},{"price":"72594.2","size":"0.00137"}])",
    R"([{"price":"72604.4","size":"0.02849"},{"price":"72604.6","size":"0.01162"}])");

}  // namespace

TEST_CASE("Lighter listing resolves perp market ids and symbol rules",
          "[lighter_market_data]") {
    const auto markets = LighterDirect::LighterMarkets::parse_order_books(kOrderBooks);
    REQUIRE(markets.size() == 3);

    const auto &btc = markets[1];
    REQUIRE(btc.symbol == "BTCUSDC");
    REQUIRE(btc.venue_symbol == "BTC");
    REQUIRE(btc.market_id == 1);
    REQUIRE(btc.is_active);
    REQUIRE_FALSE(markets[2].is_active);

    const auto info = LighterDirect::LighterMarkets::to_symbol_info(btc);
    REQUIRE(info.symbol == "BTCUSDC");
    REQUIRE(info.type == "swap");
    REQUIRE(info.quote == "USDC");
    REQUIRE(info.precision.price == Catch::Approx(0.1));
    REQUIRE(info.precision.amount == Catch::Approx(0.00001));
    REQUIRE(info.precision_int.price == 1);
    REQUIRE(info.precision_int.amount == 5);
    REQUIRE(info.limits.amount.min.toDouble() == Catch::Approx(0.00007));
    REQUIRE(info.limits.cost.min.toDouble() == Catch::Approx(10.0));

    REQUIRE(LighterDirect::LighterMarkets::parse_order_books("not json").empty());
}

TEST_CASE("Lighter book message parses snapshot and update",
          "[lighter_market_data]") {
    const auto snapshot = parse(kSnapshot);
    REQUIRE(snapshot.kind == LighterBookMessage::Kind::SNAPSHOT);
    REQUIRE(snapshot.market_id == 1);
    REQUIRE(snapshot.nonce == 100);
    REQUIRE(snapshot.exchange_ms == 1794489987002);
    REQUIRE(snapshot.bids.size() == 2);
    REQUIRE(snapshot.asks.front().price == Catch::Approx(72604.4));

    simdjson::dom::parser parser;
    REQUIRE_FALSE(LighterBookMessage::parse(
        R"({"session_id":"x","type":"connected"})", parser));
    REQUIRE_FALSE(LighterBookMessage::parse(
        R"({"channel":"ticker:1","nonce":1,"ticker":{"s":"BTC"},"type":"update/ticker"})",
        parser));
}

TEST_CASE("Lighter book applies chained deltas with absolute sizes",
          "[lighter_market_data]") {
    LighterOrderBook book;
    REQUIRE(book.apply(parse(book_message("update/order_book", 101, 100, "[]",
                                          "[]"))) ==
            LighterOrderBook::ApplyResult::NOT_READY);

    REQUIRE(book.apply(parse(kSnapshot)) ==
            LighterOrderBook::ApplyResult::SNAPSHOT_APPLIED);
    REQUIRE(book.is_ready());

    // Replace a bid size, delete the best ask, add a new ask level.
    const auto update = book_message(
        "update/order_book", 105, 100,
        R"([{"price":"72596.4","size":"1.00000"}])",
        R"([{"price":"72604.4","size":"0.00000"},{"price":"72605.0","size":"2.5"}])");
    REQUIRE(book.apply(parse(update)) == LighterOrderBook::ApplyResult::APPLIED);
    REQUIRE(book.nonce() == 105);

    const auto top = book.top(50, 2000, 1999);
    REQUIRE(top.is_valid);
    REQUIRE(top.sequence == 105);
    REQUIRE(top.bids.front().price == Catch::Approx(72596.4));
    REQUIRE(top.bids.front().quantity == Catch::Approx(1.0));
    REQUIRE(top.asks.size() == 2);
    REQUIRE(top.asks[0].price == Catch::Approx(72604.6));
    REQUIRE(top.asks[1].price == Catch::Approx(72605.0));
    REQUIRE(book.top(1, 2000, 1999).asks.size() == 1);
}

TEST_CASE("Lighter book resets on a nonce gap until the next snapshot",
          "[lighter_market_data]") {
    LighterOrderBook book;
    book.apply(parse(kSnapshot));
    REQUIRE(book.apply(parse(book_message("update/order_book", 110, 104, "[]",
                                          "[]"))) ==
            LighterOrderBook::ApplyResult::GAP);
    REQUIRE_FALSE(book.is_ready());
    REQUIRE(book.bid_count() == 0);
    REQUIRE_FALSE(book.top(50, 0, 0).is_valid);
    REQUIRE(book.apply(parse(book_message("update/order_book", 111, 110, "[]",
                                          "[]"))) ==
            LighterOrderBook::ApplyResult::NOT_READY);

    REQUIRE(book.apply(parse(kSnapshot)) ==
            LighterOrderBook::ApplyResult::SNAPSHOT_APPLIED);
    REQUIRE(book.bid_count() == 2);
}

TEST_CASE("Lighter book flags a crossed book as invalid depth",
          "[lighter_market_data]") {
    LighterOrderBook book;
    book.apply(parse(kSnapshot));
    const auto crossing = book_message(
        "update/order_book", 101, 100,
        R"([{"price":"72610.0","size":"1"}])", "[]");
    REQUIRE(book.apply(parse(crossing)) ==
            LighterOrderBook::ApplyResult::CROSSED);
    REQUIRE_FALSE(book.top(50, 0, 0).is_valid);
}

TEST_CASE("depth history returns the latest snapshot at or before a time",
          "[lighter_market_data]") {
    DepthHistory history;
    const std::string key = "BTCUSDC:lighter:swap";
    REQUIRE_FALSE(history.at(key, 1000).has_value());

    DepthSnapshot first;
    first.received_ms = 1000;
    first.sequence = 1;
    history.publish(key, first);
    DepthSnapshot second;
    second.received_ms = 1200;
    second.sequence = 2;
    history.publish(key, second);
    history.mark_invalid(key, 1500);

    REQUIRE_FALSE(history.at(key, 999).has_value());
    REQUIRE(history.at(key, 1000)->sequence == 1);
    REQUIRE(history.at(key, 1199)->sequence == 1);
    REQUIRE(history.at(key, 1200)->sequence == 2);
    REQUIRE(history.at(key, 1499)->is_valid);
    REQUIRE_FALSE(history.at(key, 1600)->is_valid);

    // Older than the retention window is dropped once newer data arrives.
    DepthSnapshot late;
    late.received_ms = 1000 + DepthHistory::RETENTION_MS + 1000;
    late.sequence = 3;
    history.publish(key, late);
    REQUIRE_FALSE(history.at(key, 1100).has_value());
    REQUIRE(history.at(key, late.received_ms)->sequence == 3);
}
