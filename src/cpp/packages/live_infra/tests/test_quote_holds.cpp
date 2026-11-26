#include <catch2/catch_all.hpp>

#include <cstring>

#include "live_infra/quote_holds.h"

using Live::QuoteHolds;

namespace {

QuoteHolds::Row row(const char* type, nlohmann::json payload = nlohmann::json::object()) {
    return {type, std::move(payload)};
}

bool reason_is(const char* got, const char* want) {
    return got != nullptr && std::strcmp(got, want) == 0;
}

}  // namespace

TEST_CASE("QuoteHolds: newest row per key decides, scoped by strategy", "[quote_holds]") {
    // One source still pulling all, one resumed, one pulling strategy 10 only.
    const auto pulls = QuoteHolds::pulls({row(Core::ENGINE_SIGNAL_RESUME_QUOTES),
                                          row(Core::ENGINE_SIGNAL_PULL_QUOTES, {{"strategy_id", 10}})});
    CHECK_FALSE(pulls.all);
    CHECK(pulls.strategy_ids.count(10) == 1);
    CHECK(reason_is(QuoteHolds::pull_reason(pulls, 10, true, true, false),
                    Core::QuotesPullReason::PULL_SIGNAL));
    CHECK(QuoteHolds::pull_reason(pulls, 9, false, true, false) == nullptr);

    const auto all = QuoteHolds::pulls({row(Core::ENGINE_SIGNAL_PULL_QUOTES)});
    CHECK(all.all);
    CHECK(reason_is(QuoteHolds::pull_reason(all, 9, false, true, false),
                    Core::QuotesPullReason::PULL_SIGNAL));
}

TEST_CASE("QuoteHolds: a stale guard pulls real money only; shutdown pulls all", "[quote_holds]") {
    const QuoteHolds::Pulls none;
    CHECK(reason_is(QuoteHolds::pull_reason(none, 9, false, false, false),
                    Core::QuotesPullReason::GUARD_STALE));
    CHECK(QuoteHolds::pull_reason(none, 10, true, false, false) == nullptr);
    CHECK(reason_is(QuoteHolds::pull_reason(none, 10, true, true, true),
                    Core::QuotesPullReason::SHUTDOWN));
}
