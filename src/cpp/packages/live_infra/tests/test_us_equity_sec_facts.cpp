#include <catch2/catch_all.hpp>

#include <cstdlib>
#include <filesystem>
#include <string>

#include "market_data/alt_data_manager.h"
#include "market_data/alt_data_types.h"
#include "market_data/us_equity_sec_facts_module.h"

namespace fs = std::filesystem;

using MarketData::AltDataManager;
using MarketData::AltDataSource;

namespace {
constexpr const char* kAppleCik = "0000320193";
}  // namespace

// Cross-language parity, which is the only thing that makes this module safe to
// serve a model trained through the Python path. Expected values come from
// SECData.facts_as_of(profile=LATENCY_CONSERVATIVE) on the same tree; if the
// engine disagrees the backtest is measuring a different strategy than the one
// that would trade.
//
// The amendment rule is what these cases really pin. The same (tag, period_end)
// is refiled with a different value, and a reverse split restates prior-period
// share counts onto the new basis, so taking the newest observation regardless
// of filing time reads a correction that had not happened yet.
TEST_CASE("SEC facts match the Python point-in-time path",
          "[alt_data][sec_facts]") {
    const char* root = std::getenv("DATA_DIR_ROOT");
    if (root == nullptr) SKIP("DATA_DIR_ROOT unset");
    const std::string cik_dir =
        std::string(root) + "/alt/filings/sec/facts/" + kAppleCik;
    if (!fs::exists(cik_dir)) SKIP("AAPL facts not on disk");

    auto& mgr = AltDataManager::instance();
    mgr.clear();
    REQUIRE(mgr.preload(AltDataSource::UsEquitySecFacts, {kAppleCik}) == 1);
    auto& facts = mgr.us_equity_sec_facts();

    struct Case {
        long long as_of_ms;
        double shares;
        long long filed_ts_ms;
    };
    // From data_library.alt_data.sec.pit.SECData.facts_as_of, verbatim.
    const std::vector<Case> cases = {
        {1717372800000LL, 15334082000.0, 1714694400000LL},  // 2024-06-03
        {1740960000000LL, 15022073000.0, 1738281600000LL},  // 2025-03-03
        {1767571200000LL, 14776353000.0, 1761868800000LL},  // 2026-01-05
    };

    SECTION("shares outstanding matches Python at each as-of") {
        for (const auto& c : cases) {
            const auto obs = facts.shares_outstanding_as_of(kAppleCik, c.as_of_ms);
            REQUIRE(obs.has_value());
            REQUIRE(obs->shares == Catch::Approx(c.shares).epsilon(1e-9));
            REQUIRE(obs->filed_ts_ms == c.filed_ts_ms);
        }
    }

    SECTION("the value moves backwards in time as the as-of moves back") {
        // Strictly increasing share counts going back is an AAPL-specific fact
        // (buybacks), and it is a cheap way to catch an as-of that is not
        // actually binding: without one, every case would return the same row.
        double previous = 0.0;
        for (auto it = cases.rbegin(); it != cases.rend(); ++it) {
            const auto obs = facts.shares_outstanding_as_of(kAppleCik, it->as_of_ms);
            REQUIRE(obs.has_value());
            REQUIRE(obs->shares > previous);
            previous = obs->shares;
        }
    }

    SECTION("a filing inside the latency pad is not yet visible") {
        // kFactsDefaultMs after the filing it is visible; one millisecond
        // before that boundary it must not be.
        const auto& c = cases.front();
        const long long pad = 86400000LL;
        REQUIRE(facts.shares_outstanding_as_of(kAppleCik, c.filed_ts_ms + pad)
                    ->filed_ts_ms == c.filed_ts_ms);
        const auto just_before =
            facts.shares_outstanding_as_of(kAppleCik, c.filed_ts_ms + pad - 1);
        REQUIRE(just_before.has_value());
        REQUIRE(just_before->filed_ts_ms < c.filed_ts_ms);
    }

    SECTION("an unprovisioned CIK is not an issuer with no facts") {
        REQUIRE(!facts.shares_outstanding_as_of("0000000000", 1767571200000LL)
                     .has_value());
        REQUIRE(!facts.is_loaded("0000000000"));
        REQUIRE(facts.is_loaded(kAppleCik));
    }

    mgr.clear();
}
