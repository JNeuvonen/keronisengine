#include <catch2/catch_all.hpp>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>

#include "chrono_utils.h"
#include "market_data/dataset_manager.h"
#include "market_data/ohlcv_bar_meta.h"
#include "market_data/ohlcv_dataset.h"
#include "ohlcv_dataset_tick.h"

namespace fs = std::filesystem;

namespace {

std::shared_ptr<MarketData::OhlcvDataset> make_test_dataset(
    const std::string& symbol_suffix, bool init_sync_done = true) {
    MarketData::OhlcvBarMeta meta;
    meta.symbol = "DIRTY_SYNC_" + symbol_suffix + "_" +
                    std::to_string(::getpid());
    meta.exchange_name = "binance";
    meta.market_type = "swap";
    meta.bar_close_policy.is_time_bar = true;
    meta.bar_close_policy.time_ms = 60000;

    auto dataset = std::make_shared<MarketData::OhlcvDataset>(
        meta, ChronoUtils::make_timepoint(2000, 1, 1, 0, 0, 0),
        /*read_data_on_init=*/false,
        /*can_fetch_from_proxy=*/false);
    if (init_sync_done) {
        dataset->mark_init_sync_done();
    }
    return dataset;
}

MarketData::OhlcvDatasetTick make_closed_tick(
    const MarketData::OhlcvBarMeta& meta, long long open_ms) {
    MarketData::OhlcvDatasetTick tick;
    tick.kline_open_time = open_ms;
    tick.kline_close_time = open_ms + meta.bar_close_policy.time_ms;
    tick.number_of_trades = 0;
    tick.open_price = 1.0;
    tick.close_price = 1.0;
    tick.high_price = 1.0;
    tick.low_price = 1.0;
    tick.volume = 1.0;
    return tick;
}

int count_parquet_files(const fs::path& root) {
    if (!fs::exists(root)) {
        return 0;
    }
    int count = 0;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file() &&
            entry.path().extension() == ".parquet") {
            ++count;
        }
    }
    return count;
}

}  // namespace

TEST_CASE("add_new_kline marks disk dirty on append", "[disk_dirty_sync]") {
    if (std::getenv("DATA_DIR_ROOT") == nullptr) {
        SKIP("DATA_DIR_ROOT not set");
    }

    auto dataset = make_test_dataset("append");
    REQUIRE_FALSE(dataset->is_disk_dirty());

    dataset->add_new_kline(make_closed_tick(dataset->meta, 1700000000000LL));
    REQUIRE(dataset->is_disk_dirty());
}

TEST_CASE("rejected add_new_kline does not mark disk dirty",
          "[disk_dirty_sync]") {
    if (std::getenv("DATA_DIR_ROOT") == nullptr) {
        SKIP("DATA_DIR_ROOT not set");
    }

    auto gated = make_test_dataset("gated", /*init_sync_done=*/false);
    gated->add_new_kline(make_closed_tick(gated->meta, 1700000000000LL));
    REQUIRE_FALSE(gated->is_disk_dirty());

    auto dataset = make_test_dataset("duplicate");
    auto tick = make_closed_tick(dataset->meta, 1700000000000LL);
    dataset->add_new_kline(tick);
    REQUIRE(dataset->is_disk_dirty());
    dataset->add_new_kline(tick);
    REQUIRE(dataset->is_disk_dirty());
}

TEST_CASE("sync_to_disk clears dirty after successful write",
          "[disk_dirty_sync]") {
    if (std::getenv("DATA_DIR_ROOT") == nullptr) {
        SKIP("DATA_DIR_ROOT not set");
    }

    auto dataset = make_test_dataset("flush");
    dataset->add_new_kline(make_closed_tick(dataset->meta, 1700000000000LL));
    REQUIRE(dataset->is_disk_dirty());

    dataset->sync_to_disk();
    REQUIRE_FALSE(dataset->is_disk_dirty());
    REQUIRE(count_parquet_files(dataset->get_parquets_path()) > 0);

    fs::remove_all(dataset->meta.get_symbol_base_directory_path());
}

TEST_CASE("sync_to_disk on clean dataset is a no-op", "[disk_dirty_sync]") {
    if (std::getenv("DATA_DIR_ROOT") == nullptr) {
        SKIP("DATA_DIR_ROOT not set");
    }

    auto dataset = make_test_dataset("clean_sync");
    REQUIRE_FALSE(dataset->is_disk_dirty());

    dataset->sync_to_disk();
    REQUIRE_FALSE(dataset->is_disk_dirty());
    REQUIRE(count_parquet_files(dataset->get_parquets_path()) == 0);

    fs::remove_all(dataset->meta.get_symbol_base_directory_path());
}

TEST_CASE("new bar after flush requires another sync", "[disk_dirty_sync]") {
    if (std::getenv("DATA_DIR_ROOT") == nullptr) {
        SKIP("DATA_DIR_ROOT not set");
    }

    auto dataset = make_test_dataset("resync");
    dataset->add_new_kline(make_closed_tick(dataset->meta, 1700000000000LL));
    dataset->sync_to_disk();
    REQUIRE_FALSE(dataset->is_disk_dirty());

    dataset->add_new_kline(make_closed_tick(dataset->meta, 1700000060000LL));
    REQUIRE(dataset->is_disk_dirty());
    dataset->sync_to_disk();
    REQUIRE_FALSE(dataset->is_disk_dirty());

    fs::remove_all(dataset->meta.get_symbol_base_directory_path());
}

TEST_CASE("sync_ohlcv_datasets_to_disk skips clean datasets",
          "[disk_dirty_sync]") {
    if (std::getenv("DATA_DIR_ROOT") == nullptr) {
        SKIP("DATA_DIR_ROOT not set");
    }

    auto& manager = MarketData::DatasetsManager::get_instance();
    auto clean = make_test_dataset("mgr_clean");
    auto dirty = make_test_dataset("mgr_dirty");
    manager.add_ohlcv_dataset(clean);
    manager.add_ohlcv_dataset(dirty);

    dirty->add_new_kline(make_closed_tick(dirty->meta, 1700000000000LL));
    REQUIRE(dirty->is_disk_dirty());
    REQUIRE_FALSE(clean->is_disk_dirty());

    manager.sync_ohlcv_datasets_to_disk();

    REQUIRE_FALSE(dirty->is_disk_dirty());
    REQUIRE_FALSE(clean->is_disk_dirty());
    REQUIRE(count_parquet_files(clean->get_parquets_path()) == 0);
    REQUIRE(count_parquet_files(dirty->get_parquets_path()) > 0);

    fs::remove_all(clean->meta.get_symbol_base_directory_path());
    fs::remove_all(dirty->meta.get_symbol_base_directory_path());
}
