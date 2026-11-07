#include <cmath>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "strategies/common/order_posting_policy.h"

using namespace OrderPosting;
using Catch::Approx;

namespace {

Policy ladder(double first, double step_bp, int rungs, double size_jitter = 0) {
    Policy policy;
    policy.price = Ladder{.first = first, .step_bp = step_bp, .rungs = rungs, .size_jitter = size_jitter};
    return policy;
}

double total(const Plan& plan) {
    double sum = 0;
    for (const auto& post : plan.posts) sum += post.qty;
    return sum;
}

Input selling(double remaining, double bid, double ask, long long now_ms = 1'000'000) {
    Input in;
    in.side = Side::SELL;
    in.remaining_qty = remaining;
    in.quote = {bid, ask, (bid + ask) / 2};
    in.now_ms = now_ms;
    in.seed = 7;
    return in;
}

}  // namespace

TEST_CASE("a single-rung ladder rests the whole remainder at the price", "[order-posting-policy]") {
    const auto plan = OrderPostingPolicy::plan(ladder(102, 0, 1), selling(2.0, 100, 100.1));
    REQUIRE(plan.posts.size() == 1);
    CHECK(plan.posts[0].price == Approx(102));
    CHECK(plan.posts[0].qty == Approx(2.0));
    CHECK(plan.next_rung == 1);
    CHECK(plan.cancels.empty());

    // Resting and fully covered, the pointer still moves past it.
    auto covered = selling(2.0, 100, 100.1);
    covered.live = {{1, 102, 2.0, 0, 0, 0, false}};
    const auto rested = OrderPostingPolicy::plan(ladder(102, 0, 1), covered);
    CHECK(rested.posts.empty());
    CHECK(rested.next_rung == 1);
}

TEST_CASE("the opening batch posts the nearest 50 rungs of an even split", "[order-posting-policy]") {
    const auto plan = OrderPostingPolicy::plan(ladder(100.5, 1, 120), selling(120.0, 100, 100.1));
    REQUIRE(plan.posts.size() == 50);
    for (int k = 0; k < 50; ++k) {
        CHECK(plan.posts[k].level == k);
        CHECK(plan.posts[k].price == Approx(100.5 * (1 + k * 1e-4)));
        CHECK(plan.posts[k].qty == Approx(1.0));
    }
    CHECK(plan.next_rung == 50);
}

TEST_CASE("far rungs rest at once", "[order-posting-policy]") {
    const auto plan = OrderPostingPolicy::plan(ladder(105, 50, 10), selling(10.0, 100, 100.1));
    REQUIRE(plan.posts.size() == 10);
    CHECK(plan.posts[9].price == Approx(105 * 1.045));
    CHECK(plan.next_rung == 10);
    CHECK(OrderPostingPolicy::plan(ladder(130, 0, 1), selling(10.0, 100, 100.1)).posts.size() == 1);
}

TEST_CASE("no rungs without a last price", "[order-posting-policy]") {
    auto in = selling(10.0, 100, 100.1);
    in.quote.last.reset();
    CHECK(OrderPostingPolicy::plan(ladder(101, 10, 5), in).posts.empty());
}

TEST_CASE("rung sizes vary within the jitter and add up to the remainder", "[order-posting-policy]") {
    const auto plan = OrderPostingPolicy::plan(ladder(100.5, 1, 5, 0.3), selling(10.0, 100, 100.1));
    REQUIRE(plan.posts.size() == 5);
    CHECK(plan.posts[0].qty >= 2 * 0.7);
    CHECK(plan.posts[0].qty <= 2 * 1.3);
    CHECK(plan.posts[0].qty != Approx(plan.posts[1].qty));
    CHECK(total(plan) == Approx(10.0));
    const auto again = OrderPostingPolicy::plan(ladder(100.5, 1, 5, 0.3), selling(10.0, 100, 100.1));
    for (int k = 0; k < 5; ++k) CHECK(again.posts[k].qty == plan.posts[k].qty);
}

TEST_CASE("after the opening batch a fill posts the next rung one gap later", "[order-posting-policy]") {
    auto policy = ladder(100.5, 1, 5);
    policy.gap_lo_ms = policy.gap_hi_ms = 30'000;
    auto in = selling(8.0, 100, 100.1);
    in.next_rung = 3;
    in.live = {{2, 100.5 * 1.0001, 2, 1, 0, 0, false}, {3, 100.5 * 1.0002, 2, 2, 0, 0, false}};
    in.last_post_ms = in.now_ms - 10'000;
    CHECK(OrderPostingPolicy::plan(policy, in).posts.empty());
    in.last_post_ms = in.now_ms - 30'000;
    const auto plan = OrderPostingPolicy::plan(policy, in);
    REQUIRE(plan.posts.size() == 1);
    CHECK(plan.posts[0].level == 3);
    CHECK(plan.posts[0].qty == Approx(2.0));
    CHECK(plan.next_rung == 4);
    CHECK(plan.cancels.empty());
}

TEST_CASE("a rung waits until the entry has filled enough to cover it", "[order-posting-policy]") {
    auto in = selling(1.0, 100, 100.1);
    in.incoming_qty = 7;
    auto plan = OrderPostingPolicy::plan(ladder(100.5, 1, 4), in);
    CHECK(plan.posts.empty());
    CHECK(plan.next_rung == 0);
    in.remaining_qty = 2;
    in.incoming_qty = 6;
    plan = OrderPostingPolicy::plan(ladder(100.5, 1, 4), in);
    REQUIRE(plan.posts.size() == 1);
    CHECK(plan.posts[0].qty == Approx(2.0));
}

TEST_CASE("a rung the market is through is skipped and its share moves on", "[order-posting-policy]") {
    const auto plan = OrderPostingPolicy::plan(ladder(100.5, 10, 3), selling(3.0, 100.6, 100.7));
    REQUIRE(plan.posts.size() == 2);
    CHECK(plan.posts[0].level == 1);
    CHECK(plan.posts[0].qty == Approx(1.5));
    CHECK(plan.next_rung == 3);
}

TEST_CASE("size beyond the laid-out rungs re-posts the farthest larger", "[order-posting-policy]") {
    auto in = selling(3.0, 100, 100.1);
    in.next_rung = 2;
    in.live = {{1, 100.5, 1, 0, 0, 0, false}, {2, 100.5 * 1.0001, 1, 1, 0, 0, false}};
    auto plan = OrderPostingPolicy::plan(ladder(100.5, 1, 2), in);
    CHECK(plan.cancels == std::vector<unsigned long long>{2});
    CHECK(plan.posts.empty());
    in.live.pop_back();
    plan = OrderPostingPolicy::plan(ladder(100.5, 1, 2), in);
    REQUIRE(plan.posts.size() == 1);
    CHECK(plan.posts[0].level == 1);
    CHECK(plan.posts[0].qty == Approx(2.0));
}

TEST_CASE("a move by whole steps keeps the orders it overlaps", "[order-posting-policy]") {
    auto in = selling(3.0, 100, 100.1);
    in.round = [](double price) { return std::round(price * 100) / 100; };
    in.live = {{1, 100.5, 1, 0, 0, 0, false}, {2, 100.6, 1, 1, 0, 0, false}};
    const auto plan = OrderPostingPolicy::plan(ladder(100.6, 10, 3), in);
    CHECK(plan.cancels == std::vector<unsigned long long>{1});
    // The cancel in flight still counts as exposed: nothing is over-posted.
    CHECK(plan.posts.size() == 2);
    CHECK(total(plan) == Approx(1.0));
}

TEST_CASE("no rung is smaller than the venue minimum", "[order-posting-policy]") {
    auto in = selling(1.0, 100, 100.1);
    in.min_qty = 0.5;
    const auto plan = OrderPostingPolicy::plan(ladder(100.5, 1, 10), in);
    REQUIRE(plan.posts.size() == 2);
    CHECK(plan.posts[0].qty == Approx(0.5));
    CHECK(plan.posts[1].qty == Approx(0.5));
}

TEST_CASE("rungs above a shrunken remainder are shed farthest first", "[order-posting-policy]") {
    auto in = selling(3.0, 100, 100.1);
    in.next_rung = 2;
    in.live = {{1, 100.5, 2, 0, 0, 0, false}, {2, 100.5 * 1.0001, 2, 1, 0, 0, false}};
    const auto plan = OrderPostingPolicy::plan(ladder(100.5, 1, 2), in);
    CHECK(plan.cancels == std::vector<unsigned long long>{2});
    CHECK(plan.posts.empty());
}

TEST_CASE("a marketable rule takes inside its cap and re-anchors when told to",
          "[order-posting-policy]") {
    Policy policy;
    policy.price = Marketable{100, 50, false, std::nullopt};
    auto plan = OrderPostingPolicy::plan(policy, selling(1.0, 99.8, 99.9));
    REQUIRE(plan.posts.size() == 1);
    CHECK(plan.posts[0].price == Approx(99.5));

    plan = OrderPostingPolicy::plan(policy, selling(1.0, 99.0, 99.1));
    CHECK(plan.beyond_cap);
    CHECK(plan.posts.empty());

    policy.price = Marketable{100, 50, true, std::nullopt};
    plan = OrderPostingPolicy::plan(policy, selling(1.0, 99.0, 99.1));
    REQUIRE(plan.anchor);
    CHECK(*plan.anchor == Approx(99.0));
    REQUIRE(plan.posts.size() == 1);
    CHECK(plan.posts[0].price == Approx(99.0 * 0.995));
}

TEST_CASE("a touch rule joins our side and re-prices only once the order is old",
          "[order-posting-policy]") {
    Policy policy;
    policy.price = Touch{5000, std::nullopt};
    auto in = selling(1.0, 100, 100.2);
    in.live = {{1, 100.3, 1.0, 0, in.now_ms - 1000, 0, false}};
    CHECK(OrderPostingPolicy::plan(policy, in).cancels.empty());
    in.live[0].placed_ms = in.now_ms - 6000;
    CHECK(OrderPostingPolicy::plan(policy, in).cancels == std::vector<unsigned long long>{1});
    in.live.clear();
    const auto plan = OrderPostingPolicy::plan(policy, in);
    REQUIRE(plan.posts.size() == 1);
    CHECK(plan.posts[0].price == Approx(100.2));
}

TEST_CASE("draws are deterministic and stay in range", "[order-posting-policy]") {
    for (std::uint64_t salt = 0; salt < 1000; ++salt) {
        const double x = OrderPostingPolicy::draw(11, salt, 2, 350, 1000);
        CHECK(x >= 350);
        CHECK(x <= 1000);
        CHECK(x == OrderPostingPolicy::draw(11, salt, 2, 350, 1000));
    }
    CHECK(OrderPostingPolicy::draw(11, 1, 2, 0, 1) != OrderPostingPolicy::draw(12, 1, 2, 0, 1));
}

TEST_CASE("rounding is applied before comparing, so sub-tick targets do not churn",
          "[order-posting-policy]") {
    auto in = selling(2.0, 100, 100.1);
    in.round = [](double price) { return std::round(price * 10) / 10; };
    in.live = {{1, 102.0, 2.0, 0, 0, 0, false}};
    const auto plan = OrderPostingPolicy::plan(ladder(102.04, 0, 1), in);
    CHECK(plan.cancels.empty());
    CHECK(plan.posts.empty());
}
