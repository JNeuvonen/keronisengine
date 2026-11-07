#include <catch2/catch_all.hpp>

#include <ctime>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "chrono_utils.h"
#include "datetime_utils.h"
#include "market_data/bar_types_constant.h"
#include "market_data/ohlcv_bar_meta.h"
#include "ohlcv_dataset_tick.h"
#include "parquet_utils.h"

namespace fs = std::filesystem;

static long long now_ms() { return DatetimeUtils::get_current_time_ms(); }

TEST_CASE("crypto opens the gate on any fetched bars", "[backfill_gate]") {
    MarketData::OhlcvBarMeta meta;
    meta.symbol = "BTCUSDT";
    meta.exchange_name = "binance";
    meta.market_type = "swap";
    REQUIRE(MarketData::backfill_allows_stream_ingest(meta, 1, 0, now_ms()));
    REQUIRE(MarketData::backfill_allows_stream_ingest(meta, 100, 0, now_ms()));
}

TEST_CASE("polygon needs the backfill CAUGHT UP, not merely productive",
          "[backfill_gate]") {
    MarketData::OhlcvBarMeta meta;
    meta.symbol = "ANY";
    meta.exchange_name = "polygon";
    meta.market_type = "stock";
    const long long now = now_ms();
    // Fetched plenty but still stranded in 2020: streaming here writes the
    // current month, the next run resumes from it, and the middle is orphaned.
    REQUIRE_FALSE(
        MarketData::backfill_allows_stream_ingest(meta, 100000, 1580000000000LL, now));
    // Caught up to the present.
    REQUIRE(MarketData::backfill_allows_stream_ingest(
        meta, 100000, now - 2LL * 24 * 60 * 60 * 1000, now));
    // Just outside the tolerated lag.
    REQUIRE_FALSE(MarketData::backfill_allows_stream_ingest(
        meta, 100000, now - 8LL * 24 * 60 * 60 * 1000, now));
}

TEST_CASE("backfill_allows_stream_ingest closed when no bars and no head",
          "[backfill_gate]") {
    MarketData::OhlcvBarMeta meta;
    meta.symbol = "EMPTY";
    meta.exchange_name = "polygon";
    meta.market_type = "stock";
    meta.last_storage_tick_close_time_ms = 0;
    REQUIRE_FALSE(MarketData::backfill_allows_stream_ingest(meta, 0, 0, now_ms()));
}

TEST_CASE("backfill_allows_stream_ingest opens on trustworthy head without fetch",
          "[backfill_gate]") {
    const char* data_root = std::getenv("DATA_DIR_ROOT");
    if (data_root == nullptr) {
        SKIP("DATA_DIR_ROOT not set");
    }

    MarketData::OhlcvBarMeta meta;
    meta.symbol = "GATE_HEALTHY_" + std::to_string(::getpid());
    meta.exchange_name = "polygon";
    meta.market_type = "stock";
    meta.bar_close_policy = MarketData::ONE_DAY_EXCHANGE_BAR;

    const fs::path parquets =
        fs::path(data_root) / "derived/bars/polygon/stock/exchange/86400000_ms" /
        meta.symbol / "parquets";
    fs::create_directories(parquets / "year=2020" / "month=03");

    REQUIRE(MarketData::backfill_allows_stream_ingest(
        meta, 0, now_ms() - 2LL * 24 * 60 * 60 * 1000, now_ms()));

    fs::remove_all(fs::path(data_root) / "derived/bars/polygon/stock/exchange/"
                                      "86400000_ms" / meta.symbol);
}

TEST_CASE("backfill_allows_stream_ingest rejects stream-only stub without fetch",
          "[backfill_gate]") {
    const char* data_root = std::getenv("DATA_DIR_ROOT");
    if (data_root == nullptr) {
        SKIP("DATA_DIR_ROOT not set");
    }

    const auto now =
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm = ChronoUtils::gmtime_safe(now);
    char current_month[64];
    std::snprintf(current_month, sizeof(current_month), "month=%02d",
                  tm.tm_mon + 1);
    const std::string current_year =
        "year=" + std::to_string(tm.tm_year + 1900);

    MarketData::OhlcvBarMeta meta;
    meta.symbol = "GATE_STUB_" + std::to_string(::getpid());
    meta.exchange_name = "polygon";
    meta.market_type = "stock";
    meta.bar_close_policy = MarketData::ONE_DAY_EXCHANGE_BAR;

    const fs::path symbol_root =
        fs::path(data_root) / "derived/bars/polygon/stock/exchange/86400000_ms" /
        meta.symbol;
    fs::create_directories(symbol_root / "parquets" / current_year /
                           current_month);

    REQUIRE(MarketData::has_only_current_month_partition(meta));
    REQUIRE_FALSE(MarketData::backfill_allows_stream_ingest(
        meta, 0, now_ms(), now_ms()));
    REQUIRE_FALSE(MarketData::backfill_allows_stream_ingest(meta, 1, 0, now_ms()));

    fs::remove_all(symbol_root);
}

TEST_CASE("read_last_tick_close_time_ms reads YYYY-MM.parquet head",
          "[backfill_gate]") {
    const char* data_root = std::getenv("DATA_DIR_ROOT");
    if (data_root == nullptr) {
        SKIP("DATA_DIR_ROOT not set");
    }

    MarketData::OhlcvBarMeta meta;
    meta.symbol = "GATE_MONTHLY_" + std::to_string(::getpid());
    meta.exchange_name = "polygon";
    meta.market_type = "stock";
    meta.bar_close_policy = MarketData::ONE_MIN_EXCHANGE_BAR;
    meta.last_storage_tick_close_time_ms = 0;

    const auto make_tick = [](int year, int month, int day, int hour) {
        std::tm tm{};
        tm.tm_year = year - 1900;
        tm.tm_mon = month - 1;
        tm.tm_mday = day;
        tm.tm_hour = hour;
        MarketData::OhlcvDatasetTick tick;
        tick.kline_open_time = static_cast<long long>(timegm(&tm)) * 1000LL;
        tick.kline_close_time = tick.kline_open_time + 60000;
        tick.open_price = 1.0;
        tick.close_price = 1.0;
        tick.high_price = 1.0;
        tick.low_price = 1.0;
        tick.volume = 1.0;
        return tick;
    };
    const auto older = make_tick(2026, 8, 15, 12);
    const auto later = make_tick(2026, 9, 14, 16);

    const fs::path symbol_root =
        fs::path(data_root) / "derived/bars/polygon/stock/exchange/60000_ms" /
        meta.symbol;
    const fs::path parquets = symbol_root / "parquets";
    fs::create_directories(parquets);
    REQUIRE(ParquetUtils::write_ohlcv_ticks_partitioned(
                parquets.string(), {older, later}) >= 0);

    bool saw_monthly_stem = false;
    for (const auto& e : fs::recursive_directory_iterator(parquets)) {
        if (e.is_regular_file() && e.path().extension() == ".parquet" &&
            e.path().stem().string().size() == 7) {
            saw_monthly_stem = true;
        }
    }
    REQUIRE(saw_monthly_stem);

    meta.read_last_tick_close_time_ms();
    REQUIRE(meta.last_storage_tick_close_time_ms == later.kline_close_time);

    fs::remove_all(symbol_root);
}
