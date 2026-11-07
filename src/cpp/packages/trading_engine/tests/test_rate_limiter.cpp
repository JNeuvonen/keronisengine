// Unit tests for RateLimiter's proactive paced fair scheduler
// (should_throttle) and its reactive cooldown. RateLimiter::instance() is a
// process-global singleton, so each case uses a distinct key to avoid bucket
// state bleeding across cases. rate=0 makes the token count fixed (no refill),
// which keeps the fairness/burst assertions independent of wall-clock timing.

#include <catch2/catch_test_macros.hpp>

#include <thread>

#include "rate_limiter.h"

using RC = RateLimiter::RequestClass;

TEST_CASE("should_throttle grants up to burst then throttles", "[rate_limiter]") {
    auto& rl = RateLimiter::instance();
    const std::string key = "test:burst";
    rl.set_pacing(key, /*rate=*/0.0, /*burst=*/3.0);

    // 3 tokens → 3 grants, 4th throttled. rate=0 so nothing refills between.
    REQUIRE_FALSE(rl.should_throttle(key, RC::OTHER));
    REQUIRE_FALSE(rl.should_throttle(key, RC::OTHER));
    REQUIRE_FALSE(rl.should_throttle(key, RC::OTHER));
    REQUIRE(rl.should_throttle(key, RC::OTHER));
}

TEST_CASE("a lone active class gets the whole budget (work-conserving)",
          "[rate_limiter]") {
    auto& rl = RateLimiter::instance();
    const std::string key = "test:wc";
    rl.set_pacing(key, 0.0, 4.0);

    // Only SM_SYNC ever polls → it should consume all 4 tokens, none reserved
    // for the idle classes.
    int grants = 0;
    for (int i = 0; i < 6; ++i) {
        if (!rl.should_throttle(key, RC::SM_SYNC)) grants++;
    }
    REQUIRE(grants == 4);
}

TEST_CASE("two contending classes split the budget fairly", "[rate_limiter]") {
    auto& rl = RateLimiter::instance();
    const std::string key = "test:fair";
    rl.set_pacing(key, 0.0, 2.0);  // exactly 2 tokens, no refill

    int sm = 0, bar = 0;
    // Alternate polls; both are "active" within the recency window, so the two
    // tokens must go one to each, not both to one.
    for (int i = 0; i < 4; ++i) {
        if (!rl.should_throttle(key, RC::SM_SYNC)) sm++;
        if (!rl.should_throttle(key, RC::BAR_BACKFILL)) bar++;
    }
    REQUIRE(sm == 1);
    REQUIRE(bar == 1);
}

TEST_CASE("an idle class yields its turn after the recency window",
          "[rate_limiter]") {
    auto& rl = RateLimiter::instance();
    const std::string key = "test:age";
    rl.set_pacing(key, 0.0, 2.0);

    // BAR takes one token, then goes idle past the 250ms window.
    REQUIRE_FALSE(rl.should_throttle(key, RC::BAR_BACKFILL));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    // SM is now the only active class → it gets the remaining token.
    REQUIRE_FALSE(rl.should_throttle(key, RC::SM_SYNC));
}

TEST_CASE("record_limit sets cooldown and drains the pacer", "[rate_limiter]") {
    auto& rl = RateLimiter::instance();
    const std::string key = "test:limit";
    rl.set_pacing(key, 0.0, 5.0);

    REQUIRE_FALSE(rl.is_limited(key));
    rl.record_limit(key, /*retry_after_s=*/2);
    REQUIRE(rl.is_limited(key));
    REQUIRE(rl.remaining_s(key) >= 1);
    // Tokens were drained, so even ignoring the cooldown the pacer throttles.
    REQUIRE(rl.should_throttle(key, RC::OTHER));
}
