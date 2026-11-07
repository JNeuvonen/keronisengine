#include <catch2/catch_all.hpp>

#include <cstdlib>
#include <filesystem>
#include <string>

#include "market_data/alt_data_manager.h"
#include "market_data/alt_data_types.h"

namespace fs = std::filesystem;

using MarketData::AltDataManager;
using MarketData::AltDataSource;

namespace {
constexpr long long kDayMs = 86400000LL;
constexpr long long kDecision = 1760011200000LL;  // 2025-10-09 12:00 UTC
}  // namespace

// Expected values were computed independently with polars from the same files.
TEST_CASE("funding settled sums [from, to) and refuses uncovered windows",
          "[alt_data][binance_swap]") {
    const char* root = std::getenv("DATA_DIR_ROOT");
    if (root == nullptr) SKIP("DATA_DIR_ROOT unset");
    if (!fs::exists(std::string(root) + "/raw/funding/binance/swap/BTCUSDT"))
        SKIP("BTCUSDT funding not on disk");

    auto& mgr = AltDataManager::instance();
    mgr.clear();
    REQUIRE(mgr.preload(AltDataSource::BinanceSwapFunding, {"BTCUSDT"}) == 1);
    const auto& funding = mgr.binance_swap_funding();

    SECTION("three 8h settlements in the 24h window before 12:00") {
        const auto total = funding.settled("BTCUSDT", kDecision - kDayMs, kDecision);
        REQUIRE(total.has_value());
        REQUIRE(total.value() == Catch::Approx(0.0001209).epsilon(1e-9));
    }

    SECTION("a settlement stamped exactly at `to` is excluded, at `from` included") {
        // 1759939200002 is the first settlement in the window above.
        const auto upto = funding.settled("BTCUSDT", kDecision - kDayMs, 1759939200002LL);
        REQUIRE(upto.value() == Catch::Approx(0.0));
        const auto from = funding.settled("BTCUSDT", 1759939200002LL, 1759939200003LL);
        REQUIRE(from.value() != Catch::Approx(0.0));
    }

    SECTION("before the first settlement there is no history, not zero") {
        REQUIRE(!funding.settled("BTCUSDT", 1500000000000LL, 1500086400000LL).has_value());
    }

    SECTION("never provisioned is nullopt") {
        REQUIRE(!funding.settled("NEVERUSDT", kDecision - kDayMs, kDecision).has_value());
    }

    SECTION("selection average sums (asof minus seven days, asof] and divides by seven") {
        REQUIRE(funding.selection_average("BTCUSDT", kDecision).value() ==
                Catch::Approx(0.00024091428571428576).epsilon(1e-9));
        REQUIRE_FALSE(funding.selection_average("NEVERUSDT", kDecision));
        REQUIRE_FALSE(funding.selection_average("BTCUSDT", 1500000000000LL));
    }
}

TEST_CASE("long/short ratio latest is as-of and bounded by age",
          "[alt_data][binance_swap]") {
    const char* root = std::getenv("DATA_DIR_ROOT");
    if (root == nullptr) SKIP("DATA_DIR_ROOT unset");
    if (!fs::exists(std::string(root) + "/raw/long_short_ratio/binance/swap/BTCUSDT"))
        SKIP("BTCUSDT long/short ratio not on disk");

    auto& mgr = AltDataManager::instance();
    mgr.clear();
    auto& ratio = mgr.binance_swap_account_long_short_ratio();
    ratio.request_history_from_ms(kDecision - 3 * kDayMs);
    REQUIRE(mgr.preload(AltDataSource::BinanceSwapAccountLongShortRatio, {"BTCUSDT"}) == 1);

    SECTION("the row stamped at the decision is visible at the decision") {
        const auto row = ratio.latest("BTCUSDT", kDecision, kDayMs);
        REQUIRE(row.has_value());
        REQUIRE(row->timestamp_ms == kDecision);
        REQUIRE(row->value == Catch::Approx(0.77203791));
    }

    SECTION("one millisecond earlier sees the previous row") {
        const auto row = ratio.latest("BTCUSDT", kDecision - 1, kDayMs);
        REQUIRE(row->timestamp_ms == kDecision - 300000);
        REQUIRE(row->value == Catch::Approx(0.77306673));
    }

    SECTION("rows older than max_age are not returned") {
        REQUIRE(!ratio.latest("BTCUSDT", kDecision, 0).has_value());
        REQUIRE(ratio.latest("BTCUSDT", kDecision, 1).has_value());
    }

    SECTION("history before load_from_ms is not loaded") {
        REQUIRE(!ratio.latest("BTCUSDT", kDecision - 4 * kDayMs, kDayMs).has_value());
    }
    mgr.clear();
}
