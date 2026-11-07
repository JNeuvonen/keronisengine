#include <catch2/catch_all.hpp>
#include <arrow/compute/api.h>
#include <arrow/io/api.h>
#include <arrow/table.h>
#include <parquet/arrow/reader.h>
#include <parquet/arrow/writer.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>

#include "chrono_utils.h"
#include "market_data/ohlcv_bar_meta.h"
#include "market_data/ohlcv_dataset.h"
#include "ohlcv_dataset_tick.h"

namespace fs = std::filesystem;

namespace {

constexpr long long kMinuteMs = 60000;

std::shared_ptr<MarketData::OhlcvDataset> make_dataset(const std::string& tag) {
    MarketData::OhlcvBarMeta meta;
    meta.symbol = "READHIST_" + tag + "_" + std::to_string(::getpid());
    meta.exchange_name = "binance";
    meta.market_type = "swap";
    meta.bar_close_policy.is_time_bar = true;
    meta.bar_close_policy.time_ms = kMinuteMs;
    auto ds = std::make_shared<MarketData::OhlcvDataset>(
        meta, ChronoUtils::make_timepoint(2000, 1, 1, 0, 0, 0),
        /*read_data_on_init=*/false, /*can_fetch_from_proxy=*/false);
    ds->mark_init_sync_done();
    return ds;
}

MarketData::OhlcvDatasetTick tick_at(long long open_ms) {
    MarketData::OhlcvDatasetTick t;
    t.kline_open_time = open_ms;
    t.kline_close_time = open_ms + kMinuteMs;
    t.open_price = t.close_price = t.high_price = t.low_price = 1.0;
    t.volume = 1.0;
    t.number_of_trades = 0;
    return t;
}

std::chrono::system_clock::time_point tp(long long ms) {
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
}

}  // namespace

TEST_CASE("history reload combines archive integer and recorder timestamp files",
          "[ohlcv_dataset][read_history]") {
    auto ds = make_dataset("mixed_timestamps");
    const long long first = ChronoUtils::to_ms_timestamp(
        ChronoUtils::make_timepoint(2024, 3, 5, 0, 0, 0));
    const long long second = ChronoUtils::to_ms_timestamp(
        ChronoUtils::make_timepoint(2024, 4, 5, 0, 0, 0));
    ds->add_new_kline(tick_at(first), true);
    ds->add_new_kline(tick_at(second), true);
    ds->sync_to_disk();
    const auto path = fs::path(ds->get_base_directory_path()) / "parquets" /
                      "year=2024/month=03/2024-03.parquet";
    auto input = arrow::io::ReadableFile::Open(path.string()).ValueOrDie();
    auto reader = parquet::arrow::OpenFile(input, arrow::default_memory_pool()).ValueOrDie();
    std::shared_ptr<arrow::Table> table;
    REQUIRE(reader->ReadTable(&table).ok());
    reader.reset();
    REQUIRE(input->Close().ok());
    for (const auto* name : {"kline_open_time_ms", "kline_close_time_ms"}) {
        const int column = table->schema()->GetFieldIndex(name);
        const auto values = arrow::compute::Cast(table->column(column), arrow::int64())
                                .ValueOrDie().chunked_array();
        table = table->SetColumn(column, table->field(column)->WithType(arrow::int64()), values)
                    .ValueOrDie();
    }
    auto output = arrow::io::FileOutputStream::Open(path.string()).ValueOrDie();
    REQUIRE(parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), output, 1024).ok());
    REQUIRE(output->Close().ok());
    const auto history = ds->read_full_history_from_disk(tp(first), tp(second + kMinuteMs));
    REQUIRE(history.size() == 2);
    CHECK(history[0].kline_open_time == first);
    CHECK(history[1].kline_open_time == second);
    CHECK(history[0].close_price == 1.0);
    ds->reread_from_disk();
    CHECK(ds->get_kline_count() == 2);
    fs::remove_all(ds->get_base_directory_path());
}

// The window bound is the whole point of this accessor. scan_partitions prunes
// by FILE start time, so a month partition comes back whole -- in a backtest
// that is the simulation's future, and features built from it look excellent
// rather than wrong. These cases pin the row-level cut.
TEST_CASE("read_full_history_from_disk respects the window",
          "[ohlcv_dataset][read_history]") {
    auto ds = make_dataset("window");
    const long long base = ChronoUtils::to_ms_timestamp(
        ChronoUtils::make_timepoint(2024, 3, 5, 0, 0, 0));

    // 600 consecutive minutes, all in one monthly partition.
    for (int i = 0; i < 600; ++i) {
        ds->add_new_kline(tick_at(base + i * kMinuteMs), true);
    }
    ds->sync_to_disk();

    SECTION("bars at or after to_exclusive are never returned") {
        const long long cut = base + 300 * kMinuteMs;
        const auto got = ds->read_full_history_from_disk(tp(base), tp(cut));
        REQUIRE(!got.empty());
        for (const auto& t : got) {
            REQUIRE(t.kline_close_time <= cut);
        }
        // A bar still open at the cut has not happened yet.
        REQUIRE(got.back().kline_close_time <= cut);
    }

    SECTION("from_inclusive trims the front") {
        const long long from = base + 100 * kMinuteMs;
        const auto got = ds->read_full_history_from_disk(tp(from),
                                                    tp(base + 200 * kMinuteMs));
        REQUIRE(!got.empty());
        REQUIRE(got.front().kline_open_time >= from);
    }

    SECTION("an inverted or empty window returns nothing") {
        REQUIRE(ds->read_full_history_from_disk(tp(base + kMinuteMs), tp(base))
                    .empty());
        REQUIRE(ds->read_full_history_from_disk(tp(base), tp(base)).empty());
    }

    SECTION("reading does not disturb the live dataset") {
        const long long before = ds->get_kline_count();
        (void)ds->read_full_history_from_disk(tp(base), tp(base + 50 * kMinuteMs));
        REQUIRE(ds->get_kline_count() == before);
    }

    SECTION("results are sorted and free of duplicates") {
        const auto got =
            ds->read_full_history_from_disk(tp(base), tp(base + 600 * kMinuteMs));
        REQUIRE(got.size() > 1);
        for (std::size_t i = 1; i < got.size(); ++i) {
            REQUIRE(got[i - 1].kline_open_time < got[i].kline_open_time);
        }
    }

    fs::remove_all(ds->get_base_directory_path());
}
