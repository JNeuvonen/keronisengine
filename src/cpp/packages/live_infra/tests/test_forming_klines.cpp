#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>

#include "market_data/forming_klines.h"
#include "market_data/market_data_streams.h"

using MarketData::FormingKlineStore;

namespace {

std::string kline(const std::string& symbol, const std::string& interval,
                  long long open_ms, const std::string& close, bool closed) {
    return R"({"e":"kline","E":1,"s":")" + symbol + R"(","k":{"t":)" +
           std::to_string(open_ms) + R"(,"T":)" + std::to_string(open_ms + 59'999) +
           R"(,"s":")" + symbol + R"(","i":")" + interval +
           R"(","o":"100","c":")" + close +
           R"(","h":"110","l":"90","v":"2","n":7,"x":)" +
           (closed ? "true" : "false") +
           R"(,"q":"210","V":"1","Q":"105","B":"0"}})";
}

}  // namespace

TEST_CASE("the store keeps the latest message per kline", "[forming_klines]") {
    auto& store = FormingKlineStore::get_instance();
    store.clear();
    store.put("binance", "swap", kline("BTCUSDT", "1m", 60'000, "101", false), 1);
    store.put("binance", "swap", kline("BTCUSDT", "1m", 60'000, "102", false), 2);
    store.put("binance", "swap", kline("BTCUSDT", "1d", 0, "103", false), 3);
    store.put("binance", "spot", kline("BTCUSDT", "1m", 60'000, "104", false), 4);
    store.put("binance", "swap", R"({"result":null,"id":1})", 5);
    CHECK(store.size() == 3);

    const auto swap_minute = store.read("binance", "swap", "1m", {}, 0);
    REQUIRE(swap_minute.klines.size() == 1);
    const auto& latest = swap_minute.klines[0];
    CHECK(latest.symbol == "BTCUSDT");
    CHECK(latest.bar.close == 102);
    CHECK(latest.bar.open_time_ms == 60'000);
    CHECK(latest.bar.vwap == 105);
    CHECK(latest.bar.trades == 7);
    CHECK(latest.received_ms == 2);
    CHECK_FALSE(latest.closed);

    CHECK(store.read("binance", "swap", "1d", {}, 0).klines[0].bar.close == 103);
    CHECK(store.read("binance", "spot", "1m", {}, 0).klines[0].bar.close == 104);
    CHECK(store.read("binance", "spot", "1d", {}, 0).klines.empty());
}

TEST_CASE("readers get klines updated after their sequence",
          "[forming_klines]") {
    auto& store = FormingKlineStore::get_instance();
    store.clear();
    store.put("binance", "swap", kline("BTCUSDT", "1m", 0, "1", false), 1);
    store.put("binance", "swap", kline("ETHUSDT", "1m", 0, "2", false), 1);
    const auto first = store.read("binance", "swap", "1m", {}, 0);
    CHECK(first.klines.size() == 2);

    store.put("binance", "swap", kline("ETHUSDT", "1m", 0, "3", true), 2);
    const auto next = store.read("binance", "swap", "1m", {}, first.latest_sequence);
    REQUIRE(next.klines.size() == 1);
    CHECK(next.klines[0].symbol == "ETHUSDT");
    CHECK(next.klines[0].closed);
    CHECK(store.read("binance", "swap", "1m", {}, next.latest_sequence)
              .klines.empty());

    // A sequence ahead of the store means the recorder restarted.
    CHECK(store.read("binance", "swap", "1m", {}, 1'000).klines.size() == 2);
    CHECK(store.read("binance", "swap", "1m", {"BTCUSDT", "XRPUSDT"}, 0)
              .klines.size() == 1);
}

TEST_CASE("the forming wire format carries raw 64-byte bars",
          "[forming_klines]") {
    auto& store = FormingKlineStore::get_instance();
    store.clear();
    store.put("binance", "swap", kline("BTCUSDT", "1m", 60'000, "101", false), 9);
    const auto klines = store.read("binance", "swap", "1m", {}, 0);
    const auto wire = FormingKlineStore::encode(klines);
    REQUIRE(wire.size() == 8 + 4 + 2 + 7 + 1 + 8 + 64);

    std::uint64_t sequence = 0;
    std::memcpy(&sequence, wire.data(), 8);
    CHECK(sequence == klines.latest_sequence);
    CHECK(wire.substr(14, 7) == "BTCUSDT");
    CHECK(wire[21] == 0);
    long long received_ms = 0;
    std::memcpy(&received_ms, wire.data() + 22, 8);
    CHECK(received_ms == 9);
    MarketData::SecondBar bar;
    std::memcpy(&bar, wire.data() + 30, 64);
    CHECK(bar.open_time_ms == 60'000);
    CHECK(bar.close == 101);
}

TEST_CASE("only a process that opts in keeps forming klines",
          "[forming_klines]") {
    auto& store = FormingKlineStore::get_instance();
    auto& streams = MarketData::MarketDataStreams::get_instance();
    simdjson::dom::parser parser;
    MarketData::MarketDataMsg msg;
    msg.exchange_name = "binance";
    msg.market_type = "swap";
    msg.msg = kline("BTCUSDT", "1m", 60'000, "101", false);

    store.clear();
    streams.parse_binance_ohlcv_message(msg, parser);
    CHECK(store.size() == 0);

    streams.set_keep_forming_klines(true);
    streams.parse_binance_ohlcv_message(msg, parser);
    streams.set_keep_forming_klines(false);
    CHECK(store.size() == 1);
    store.clear();
}
