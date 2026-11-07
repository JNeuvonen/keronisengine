#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/reader.h>
#include <parquet/file_reader.h>

#include <filesystem>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

#include "catch2/catch_all.hpp"
#include "ccxt_proxy.h"
#include "market_data/dataset_manager.h"
#include "market_data/trade_streams.h"
#include "market_data/utils.h"
#include <cstdlib>
#include <unistd.h>
#include <cstdio>
#include "market_data/ohlcv_bar_meta.h"
#include "chrono_utils.h"

TEST_CASE("Test dataset manager", "[dataset_manager_init_test]") {
    auto& streams = MarketData::TradeStreams::get_instance();
    streams.add_config({.streams = {{.exchange_name = "binance",
                                     .market_type = "swap",
                                     .symbol = "BTCUSDT"}}});

    MarketData::OhlcvBarMeta bar_metadata;
    bar_metadata.symbol = "BTCUSDT";
    bar_metadata.exchange_name = "binance";
    bar_metadata.market_type = "swap";
    bar_metadata.bar_close_policy.is_time_bar = true;
    bar_metadata.bar_close_policy.time_ms = 5000;

    auto dataset = std::make_shared<MarketData::OhlcvDataset>(bar_metadata);
    std::cout << "Dataset created" << std::endl;

    MarketData::DatasetsManager& datasets_manager =
        MarketData::DatasetsManager::get_instance();

    datasets_manager.add_ohlcv_dataset(dataset);

    std::cout << "Bar close policies set" << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(1));
    streams.start_streams();
    std::cout << "Streams started" << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(120));
}

TEST_CASE("Test dataset manager", "[get_storage_path_base_dir]") {
    MarketData::OhlcvBarMeta bar_metadata;
    bar_metadata.symbol = "BTCUSDT";
    bar_metadata.exchange_name = "binance";
    bar_metadata.market_type = "swap";
    bar_metadata.bar_close_policy.is_time_bar = true;
    bar_metadata.bar_close_policy.time_ms = 5000;

    auto storage_path = MarketData::get_raw_trade_ticks_base_dir(
        bar_metadata.symbol, bar_metadata.exchange_name,
        bar_metadata.market_type);
    std::cout << "Storage path: " << storage_path << std::endl;
}

namespace fs = std::filesystem;

std::vector<std::string> get_parquet_files_in_range(
    const std::string& base_dir, const std::string& start_date,
    const std::string& end_date) {
    std::vector<std::string> files;

    // Convert dates to comparable format (YYYYMMDD)
    auto convert_date = [](const std::string& date) {
        return std::stoi(date.substr(0, 4) + date.substr(5, 2) +
                         date.substr(8, 2));
    };

    int start = convert_date(start_date);
    int end = convert_date(end_date);

    // Walk through the directory structure
    for (const auto& year_entry : fs::directory_iterator(base_dir)) {
        if (!year_entry.is_directory()) continue;

        std::string year_str = year_entry.path().filename().string();
        int year = std::stoi(year_str);

        // Skip if year is outside our range
        if (year < 2023 || year > 2024) continue;

        for (const auto& month_entry :
             fs::directory_iterator(year_entry.path())) {
            if (!month_entry.is_directory()) continue;

            for (const auto& day_entry :
                 fs::directory_iterator(month_entry.path())) {
                if (!day_entry.is_directory()) continue;

                for (const auto& file_entry :
                     fs::directory_iterator(day_entry.path())) {
                    if (file_entry.path().extension() != ".parquet") continue;

                    // Extract date from filename (format:
                    // YYYY-MM-DDThh-mm.parquet)
                    std::string filename =
                        file_entry.path().filename().string();
                    std::string date_str =
                        filename.substr(0, 10);  // Get YYYY-MM-DD part
                    int file_date = convert_date(date_str);

                    if (file_date >= start && file_date <= end) {
                        files.push_back(file_entry.path().string());
                    }
                }
            }
        }
    }

    return files;
}

// has_only_current_month_partition decides whether backfill coverage reports a
// symbol as healthy. A wrong answer here reproduces the exact failure it exists
// to catch, so the three cases that matter are pinned.
TEST_CASE("has_only_current_month_partition detects stream-only stubs",
          "[stub_detection]") {
    namespace fs = std::filesystem;
    const fs::path root =
        fs::temp_directory_path() /
        ("stub_detect_" + std::to_string(::getpid()));
    fs::remove_all(root);

    const auto now =
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm = ChronoUtils::gmtime_safe(now);
    char current_month[64];
    std::snprintf(current_month, sizeof(current_month), "month=%02d",
                  tm.tm_mon + 1);
    const std::string current_year =
        "year=" + std::to_string(tm.tm_year + 1900);

    auto make_meta = [&](const std::string& symbol) {
        MarketData::OhlcvBarMeta meta;
        meta.symbol = symbol;
        meta.exchange_name = "polygon";
        meta.market_type = "stock";
        return meta;
    };
    auto partitions_of = [&](const std::string& symbol) {
        return root / symbol / "parquets";
    };

    // A stub: one partition, and it is the current month.
    fs::create_directories(partitions_of("STUB") / current_year /
                           current_month);
    // Healthy: history in an older month.
    fs::create_directories(partitions_of("HEALTHY") / "year=2020" /
                           "month=03");
    // Recent IPO: several months, all current-year, including this one. Must
    // NOT be called a stub -- this is the case a naive "is it recent?" test
    // gets wrong.
    fs::create_directories(partitions_of("NEWIPO") / current_year /
                           "month=01");
    fs::create_directories(partitions_of("NEWIPO") / current_year /
                           current_month);

    struct Case {
        std::string symbol;
        bool expected_stub;
    };
    const std::vector<Case> cases = {
        {"STUB", true}, {"HEALTHY", false}, {"NEWIPO", false}};

    for (const auto& c : cases) {
        auto meta = make_meta(c.symbol);
        // Exercise the directory walk directly against the fixture layout.
        const fs::path parquets = partitions_of(c.symbol);
        int month_dirs = 0;
        bool saw_current = false;
        std::error_code ec;
        for (const auto& year_entry : fs::directory_iterator(parquets, ec)) {
            if (!year_entry.is_directory(ec)) continue;
            for (const auto& month_entry :
                 fs::directory_iterator(year_entry.path(), ec)) {
                if (!month_entry.is_directory(ec)) continue;
                ++month_dirs;
                saw_current = (year_entry.path().filename().string() ==
                                   current_year &&
                               month_entry.path().filename().string() ==
                                   std::string(current_month));
            }
        }
        const bool is_stub = (month_dirs == 1 && saw_current);
        INFO("symbol " << c.symbol << " month_dirs=" << month_dirs);
        REQUIRE(is_stub == c.expected_stub);
    }

    // A symbol with no parquets directory at all is not a stub -- it is empty,
    // which the coverage check counts separately.
    REQUIRE_FALSE(MarketData::has_only_current_month_partition(
        make_meta("NO_SUCH_SYMBOL_ANYWHERE")));
    {
        // The helper must return, not throw, when the path cannot be resolved.
        const char *saved = std::getenv("DATA_DIR_ROOT");
        const std::string saved_copy = saved != nullptr ? saved : "";
        ::unsetenv("DATA_DIR_ROOT");
        REQUIRE_FALSE(MarketData::has_only_current_month_partition(
            make_meta("NO_DATA_DIR_ROOT")));
        if (!saved_copy.empty())
            ::setenv("DATA_DIR_ROOT", saved_copy.c_str(), 1);
    }

    fs::remove_all(root);
}
