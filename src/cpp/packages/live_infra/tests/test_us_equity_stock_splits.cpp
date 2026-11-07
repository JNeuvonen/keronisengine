#include <catch2/catch_all.hpp>

#include <cstdlib>
#include <filesystem>
#include <string>

#include "market_data/alt_data_manager.h"
#include "market_data/alt_data_types.h"
#include "market_data/us_equity_stock_splits_module.h"

namespace fs = std::filesystem;

using MarketData::AltDataManager;
using MarketData::AltDataSource;

namespace {
constexpr long long kDayMs = 86400000LL;
long long days(const char* iso_days) { return std::atoll(iso_days) * kDayMs; }
}  // namespace

// GDHG reverse-split three times -- 1:50, 1:25, 1:15 -- and multiplying its
// unadjusted filed share count by an adjusted price without correcting for that
// printed a $16.8B market cap on $1.3M of ADV. These cases pin the correction
// and, more importantly, pin that a split in the FUTURE of the as-of is
// invisible: the file on disk holds every split the ticker ever had, and
// applying a later one would make a historical market cap look more correct
// than it could possibly have been.
TEST_CASE("split ratios are point-in-time", "[alt_data][splits]") {
    const char* root = std::getenv("DATA_DIR_ROOT");
    if (root == nullptr) SKIP("DATA_DIR_ROOT unset");
    const std::string alt = std::string(root) + "/alt";
    if (!fs::exists(alt + "/corporate_actions/splits/GDHG.parquet"))
        SKIP("GDHG splits not on disk");

    auto& mgr = AltDataManager::instance();
    mgr.clear();
    REQUIRE(mgr.preload(AltDataSource::UsEquityStockSplits, {"GDHG", "AAPL"}) == 2);
    auto& splits = mgr.us_equity_stock_splits();

    // 2024-09-19, 2025-05-09, 2025-08-28
    const long long before_all = days("19000");   // 2022-01-08
    const long long after_first = days("20000");  // 2024-10-04
    const long long after_all = days("20400");    // 2025-11-08

    SECTION("a split after the as-of is invisible") {
        const auto visible = splits.splits_as_of("GDHG", before_all);
        REQUIRE(visible.has_value());
        REQUIRE(visible->empty());
    }

    SECTION("splits become visible as the as-of advances") {
        REQUIRE(splits.splits_as_of("GDHG", after_first)->size() == 1);
        REQUIRE(splits.splits_as_of("GDHG", after_all)->size() == 3);
    }

    SECTION("cumulative ratio composes across every visible split") {
        const auto r = splits.cumulative_ratio("GDHG", before_all, after_all);
        REQUIRE(r.has_value());
        // 0.02 * 0.04 * (1/15) == 1/18750
        REQUIRE(r.value() == Catch::Approx(1.0 / 18750.0).epsilon(1e-6));
    }

    SECTION("the to_ms bound excludes later splits") {
        const auto r =
            splits.cumulative_ratio("GDHG", before_all, after_first);
        REQUIRE(r.has_value());
        REQUIRE(r.value() == Catch::Approx(0.02).epsilon(1e-6));
    }

    SECTION("a window after all splits applies none of them") {
        const auto r = splits.cumulative_ratio("GDHG", after_all,
                                                  after_all + 30 * kDayMs);
        REQUIRE(r.value() == Catch::Approx(1.0));
    }

    SECTION("provisioned-with-no-splits differs from never-provisioned") {
        REQUIRE(splits.cumulative_ratio("AAPL", before_all, after_all)
                    .has_value());
        REQUIRE(!splits.cumulative_ratio("NEVER_PROVISIONED_XYZ", before_all,
                                            after_all)
                     .has_value());
        REQUIRE(splits.is_loaded("GDHG"));
        REQUIRE(!splits.is_loaded("NEVER_XYZ"));
    }

    mgr.clear();
}
