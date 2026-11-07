#include <catch2/catch_test_macros.hpp>

#include <string>

#include "parquet_utils.h"

// Bars moved from one parquet per day to one per month, and both layouts
// coexist while a tree is compacted in place. The pruning arithmetic is where
// that goes wrong quietly: a month file parsed as a single day is skipped for
// any window starting after the 1st, so a backtest silently loses the first
// weeks of every month rather than failing.
TEST_CASE("partition window spans day or month", "[partition_window]") {
    long long start = 0;
    long long span = 0;
    constexpr long long kDay = 86400000LL;

    SECTION("day partition spans one day") {
        REQUIRE(ParquetUtils::parse_partition_window(
            "/x/year=2026/month=08/day=10/2026-08-10.parquet", start, span));
        REQUIRE(span == kDay);
    }

    SECTION("month partition spans the whole month") {
        REQUIRE(ParquetUtils::parse_partition_window(
            "/x/year=2026/month=08/2026-08.parquet", start, span));
        REQUIRE(span == 31 * kDay);
    }

    SECTION("february length comes from the calendar") {
        REQUIRE(ParquetUtils::parse_partition_window(
            "/x/year=2026/month=02/2026-02.parquet", start, span));
        REQUIRE(span == 28 * kDay);

        REQUIRE(ParquetUtils::parse_partition_window(
            "/x/year=2024/month=02/2024-02.parquet", start, span));
        REQUIRE(span == 29 * kDay);
    }

    SECTION("december rolls into the next year") {
        REQUIRE(ParquetUtils::parse_partition_window(
            "/x/year=2025/month=12/2025-12.parquet", start, span));
        REQUIRE(span == 31 * kDay);
    }

    SECTION("a path with no partition is rejected") {
        REQUIRE_FALSE(ParquetUtils::parse_partition_window(
            "/x/some/other/file.parquet", start, span));
    }

    SECTION("year-only partition spans the whole year") {
        REQUIRE(ParquetUtils::parse_partition_window(
            "/x/year=2024/data.parquet", start, span));
        REQUIRE(span == 366 * kDay);  // 2024 is a leap year
        REQUIRE(ParquetUtils::parse_partition_window(
            "/x/year=2025/data.parquet", start, span));
        REQUIRE(span == 365 * kDay);
    }

    SECTION("a year-only file survives a window starting mid-year") {
        REQUIRE(ParquetUtils::parse_partition_window(
            "/x/alt/filings/sec/registrations/0000320193/parquets/year=2026/"
            "data.parquet",
            start, span));
        const long long window_start = start + 200 * kDay;
        REQUIRE_FALSE(start + span - 1 < window_start);
    }

    // The regression this change exists to prevent.
    SECTION("month file survives a window starting mid-month") {
        REQUIRE(ParquetUtils::parse_partition_window(
            "/x/year=2026/month=08/2026-08.parquet", start, span));
        const long long window_start = start + 20 * kDay;  // the 21st
        REQUIRE_FALSE(start + span - 1 < window_start);
    }
}
