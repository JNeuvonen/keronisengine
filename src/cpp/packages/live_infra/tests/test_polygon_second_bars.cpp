#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <filesystem>
#include <string>
#include <unistd.h>

#include "market_data/polygon_second_bars.h"

using MarketData::PolygonSecondBarStore;
using MarketData::SecondBar;

namespace {

constexpr long long OCT14_0800_UTC = 1'791'964'800'000LL;
constexpr long long OCT14_2359_UTC = 1'792'022'340'000LL;
constexpr long long OCT15_0800_UTC = 1'792'051'200'000LL;
constexpr long long OCT16_1330_UTC = 1'792'157'400'000LL;

SecondBar bar(long long time_ms, double close) {
    return {time_ms, close, close + 1, close - 1, close, 100, close, 3};
}

std::vector<std::pair<std::string, SecondBar>> one(const std::string& symbol,
                                                   long long time_ms,
                                                   double close) {
    return {{symbol, bar(time_ms, close)}};
}

}  // namespace

TEST_CASE("a session sits in one day number in summer and winter",
          "[polygon_second_bars]") {
    CHECK(PolygonSecondBarStore::session_day(OCT14_0800_UTC) ==
          PolygonSecondBarStore::session_day(OCT14_2359_UTC));
    CHECK(PolygonSecondBarStore::session_day(OCT15_0800_UTC) ==
          PolygonSecondBarStore::session_day(OCT14_0800_UTC) + 1);
    // 2026-12-01 09:00 UTC (04:00 EST) and 2026-12-02 00:59 UTC (19:59 EST).
    CHECK(PolygonSecondBarStore::session_day(1'796'115'600'000LL) ==
          PolygonSecondBarStore::session_day(1'796'173'140'000LL));
    CHECK(PolygonSecondBarStore::session_day(1'796'202'000'000LL) ==
          PolygonSecondBarStore::session_day(1'796'115'600'000LL) + 1);
}

TEST_CASE("bars stay sorted and a repeated second replaces the bar",
          "[polygon_second_bars]") {
    auto& store = PolygonSecondBarStore::get_instance();
    store.clear();
    store.add(one("AAPL", OCT14_0800_UTC + 2000, 2));
    store.add(one("AAPL", OCT14_0800_UTC, 1));
    store.add(one("AAPL", OCT14_0800_UTC + 2000, 5));
    store.add(one("MSFT", OCT14_0800_UTC + 1000, 9));

    const auto all = store.snapshot({"AAPL", "MSFT", "NONE"}, 0);
    REQUIRE(all.size() == 3);
    REQUIRE(all[0].bars.size() == 2);
    CHECK(all[0].bars[0].close == 1);
    CHECK(all[0].bars[1].close == 5);
    CHECK(all[2].bars.empty());
    CHECK(store.snapshot({"AAPL"}, OCT14_0800_UTC + 1)[0].bars.size() == 1);
    CHECK(store.stats().bars == 3);
    CHECK(store.stats().symbols == 2);
    store.clear();
}

TEST_CASE("an hour holds exactly its bars, sorted by symbol",
          "[polygon_second_bars]") {
    auto& store = PolygonSecondBarStore::get_instance();
    store.clear();
    store.add(one("MSFT", OCT14_0800_UTC, 1));
    store.add(one("AAPL", OCT14_0800_UTC + 3'599'000, 2));
    store.add(one("AAPL", OCT14_0800_UTC + 3'600'000, 3));
    store.add(one("AAPL", OCT14_0800_UTC - 1000, 4));

    const auto hour = store.hour(OCT14_0800_UTC);
    REQUIRE(hour.size() == 2);
    CHECK(hour[0].symbol == "AAPL");
    REQUIRE(hour[0].bars.size() == 1);
    CHECK(hour[0].bars[0].close == 2);
    CHECK(hour[1].symbol == "MSFT");
    store.clear();
}

TEST_CASE("only the latest sessions stay in memory", "[polygon_second_bars]") {
    auto& store = PolygonSecondBarStore::get_instance();
    store.clear();
    store.add(one("AAPL", OCT14_0800_UTC, 1));
    store.add(one("OLD", OCT14_2359_UTC, 1));
    store.add(one("AAPL", OCT15_0800_UTC, 2));
    store.add(one("AAPL", OCT16_1330_UTC, 3));
    store.keep_latest_sessions(2);

    const auto stats = store.stats();
    CHECK(stats.sessions == 2);
    CHECK(stats.bars == 2);
    CHECK(stats.symbols == 1);
    const auto aapl = store.snapshot({"AAPL"}, 0)[0].bars;
    REQUIRE(aapl.size() == 2);
    CHECK(aapl[0].open_time_ms == OCT15_0800_UTC);
    store.clear();
}

TEST_CASE("hour files read back what was written, held bars win on merge",
          "[polygon_second_bars]") {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("second_bars_" + std::to_string(::getpid()));
    const auto path =
        PolygonSecondBarStore::hour_file_path(dir.string(), OCT14_0800_UTC);
    CHECK(path == dir.string() +
                      "/derived/bars/polygon/stock/exchange/1000_ms_hourly/"
                      "date=2026-10-14/hour=08.parquet");

    std::vector<MarketData::SymbolSecondBars> written{
        {"AAPL", {bar(OCT14_0800_UTC, 1), bar(OCT14_0800_UTC + 1000, 2)}},
        {"BRK.B", {bar(OCT14_0800_UTC + 5000, 3)}}};
    REQUIRE(PolygonSecondBarStore::write_hour_file(path, written, "stream"));

    MarketData::SecondBarsBySymbol loaded;
    REQUIRE(PolygonSecondBarStore::read_hour_file(path, loaded));
    REQUIRE(loaded["AAPL"].size() == 2);
    CHECK(loaded["AAPL"][1].close == 2);
    CHECK(loaded["AAPL"][1].high == 3);
    CHECK(loaded["AAPL"][1].trades == 3);
    CHECK(loaded["BRK.B"][0].open_time_ms == OCT14_0800_UTC + 5000);

    auto& store = PolygonSecondBarStore::get_instance();
    store.clear();
    store.add(one("AAPL", OCT14_0800_UTC + 1000, 7));
    store.merge(std::move(loaded));
    const auto aapl = store.snapshot({"AAPL"}, 0)[0].bars;
    REQUIRE(aapl.size() == 2);
    CHECK(aapl[0].close == 1);
    CHECK(aapl[1].close == 7);
    CHECK(store.stats().bars == 3);
    store.clear();
    std::filesystem::remove_all(dir);
}

TEST_CASE("live readers get every bar once and resnapshot after a gap",
          "[polygon_second_bars]") {
    auto& store = PolygonSecondBarStore::get_instance();
    store.clear();
    const auto start = store.since(0).latest_sequence;
    store.add(one("AAPL", OCT14_0800_UTC, 1));
    store.add({{"MSFT", bar(OCT14_0800_UTC, 2)}, {"AAPL", bar(OCT14_0800_UTC + 1000, 3)}});

    const auto all = store.since(start);
    CHECK(all.complete);
    CHECK(all.latest_sequence == start + 2);
    REQUIRE(all.bars.size() == 3);
    CHECK(all.bars[2].first == "AAPL");
    CHECK(all.bars[2].second.close == 3);
    const auto rest = store.since(start + 1);
    CHECK(rest.bars.size() == 2);
    CHECK(store.since(start + 2).bars.empty());
    CHECK(store.since(start + 2).complete);
    // A sequence the store never issued: the recorder restarted.
    CHECK_FALSE(store.since(start + 50).complete);
    store.clear();
    CHECK_FALSE(store.since(start).complete);
}

TEST_CASE("wire formats carry raw 64-byte bars", "[polygon_second_bars]") {
    const auto snapshot = PolygonSecondBarStore::encode_snapshot(
        {{"AAPL", {bar(OCT14_0800_UTC, 1), bar(OCT14_0800_UTC + 1000, 2)}},
         {"X", {}}});
    CHECK(snapshot.size() == 4 + (2 + 4 + 4 + 2 * 64) + (2 + 1 + 4));
    SecondBar decoded;
    std::memcpy(&decoded, snapshot.data() + 4 + 2 + 4 + 4 + 64, 64);
    CHECK(decoded.open_time_ms == OCT14_0800_UTC + 1000);
    CHECK(decoded.close == 2);

    MarketData::SecondBarsSince since;
    since.latest_sequence = 7;
    since.bars = {{"MSFT", bar(OCT14_0800_UTC, 5)}};
    const auto live = PolygonSecondBarStore::encode_since(since);
    CHECK(live.size() == 8 + 1 + 4 + 2 + 4 + 64);
    std::uint64_t sequence = 0;
    std::memcpy(&sequence, live.data(), 8);
    CHECK(sequence == 7);
    CHECK(live[8] == 1);
}

TEST_CASE("the recorder reads hour files the nightly REST job writes",
          "[polygon_second_bars]") {
    // Written by data_library.polygon_second_bars.PolygonSecondBars.write_hours.
    MarketData::SecondBarsBySymbol loaded;
    REQUIRE(PolygonSecondBarStore::read_hour_file(
        std::string(SOURCE_ROOT) +
            "/packages/live_infra/tests/fixtures/polygon_second_bars/"
            "written_by_python.parquet",
        loaded));
    REQUIRE(loaded.size() == 2);
    REQUIRE(loaded["BRK.B"].size() == 1);
    const auto& brk = loaded["BRK.B"][0];
    CHECK(brk.open_time_ms == OCT14_0800_UTC);
    CHECK(brk.low == 0.5);
    CHECK(brk.vwap == 1.25);
    CHECK(brk.trades == 4);
    CHECK(loaded["AAPL"][0].volume == 7);
}
