#include "live_infra/types.h"
#include "market_making/discretionary_accounts.h"
#include <algorithm>
#include <functional>
#include <vector>

#include <catch2/catch_all.hpp>

#include "strategies/discretionary/commands.h"
#include "strategies/discretionary/execution.h"

using namespace Discretionary;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinRel;

namespace {

Command parse(const std::string& op, const std::string& payload) {
    return CommandParser::parse(op, nlohmann::json::parse(payload));
}

}  // namespace

TEST_CASE("discretionary commands parse into typed ops", "[discretionary]") {
    const auto order = std::get<PlaceOrder>(parse("place_order",
        R"({"signal_id": 41, "qty": 0.5, "price": 70000, "reduce_only": true,
            "expires_ms": 60000})"));
    CHECK(*order.trade == 41);
    CHECK_FALSE(order.open);
    CHECK(order.reduce_only);
    CHECK(*order.size.qty == 0.5);
    CHECK(*order.expires_ms == 60000);

    const auto limit = std::get<PlaceOrder>(parse("place_order",
        R"({"symbol": "BTCUSDT", "side": "sell", "notional": 500, "price": 70000,
            "setup_id": 3, "ev_expectation": 1.5, "tp": 65000})"));
    CHECK(limit.open->side == Side::SELL);
    CHECK(limit.open->setup_id == 3);
    CHECK(limit.open->tp->price == 65000);
    CHECK(limit.open->tp->rungs == 1);
    CHECK(limit.open->tp->size_jitter == 0.3);
    CHECK(limit.open->tp->gap_lo_ms == 5000);
    CHECK(limit.open->tp->gap_hi_ms == 15000);

    const auto accumulate = std::get<Execution>(parse("open_trade",
        R"({"symbol": "ETHUSDT", "side": "buy", "notional": 1000, "style": "accumulate",
            "clip_usd": 100, "every_s": 30, "clip_style": "passive", "stop": {"price": 3000, "clip_usd": 400},
            "setup_id": 2, "ev_expectation": -0.5, "account_id": 7})"));
    CHECK(accumulate.kind == ExecutionKind::OPEN);
    CHECK(accumulate.clip_style == Style::PASSIVE);
    CHECK(accumulate.clip_usd == 100);
    CHECK(accumulate.every_ms == 30000);
    CHECK(accumulate.clip_jitter == 0.3);
    CHECK(accumulate.every_jitter == 0.5);
    CHECK(accumulate.ttl_ms == 3'600'000);
    CHECK(accumulate.open->stop->price == 3000);
    const auto& armed = *accumulate.open->stop;
    CHECK(armed.cross_bps == 30);
    CHECK(armed.every_ms == 3000);
    CHECK(armed.every_jitter == 0.5);
    CHECK(armed.clip_usd == 400);
    CHECK(armed.clip_jitter == 0.3);
    CHECK(armed.clip_style == Style::TAKE);
    CHECK(armed.clip_life_hi_ms == 0);
    CHECK(accumulate.open->ev_pct == -0.5);
    CHECK(*accumulate.open->account_id == 7);
    CHECK(accumulate.cap_bps == 10);
    CHECK(accumulate.max_slippage_bps == 100);
    CHECK(accumulate.clip_life_hi_ms == 0);

    const auto tuned = std::get<Execution>(parse("reduce_trade",
        R"({"signal_id": 9, "fraction": 1, "style": "accumulate", "clip_usd": 500, "every_s": 5,
            "cross_bps": 5, "max_slippage_bps": 40, "clip_jitter": 0.2, "every_jitter": 0.7,
            "ttl_s": 600, "clip_life_s": [20, 40]})"));
    CHECK(tuned.cap_bps == 5);
    CHECK(tuned.max_slippage_bps == 40);
    CHECK(tuned.clip_jitter == 0.2);
    CHECK(tuned.every_jitter == 0.7);
    CHECK(tuned.ttl_ms == 600'000);
    CHECK(tuned.clip_life_lo_ms == 20000);
    CHECK(tuned.clip_life_hi_ms == 40000);

    const auto hyperliquid = std::get<Execution>(parse("open_trade",
        R"({"symbol": "BTC", "side": "sell", "qty": 0.01, "exchange": "hyperliquid",
            "setup_id": 2, "ev_expectation": 1})"));
    CHECK(hyperliquid.open->symbol == "BTC");
    CHECK(*hyperliquid.open->exchange == "hyperliquid");

    const auto add = std::get<Execution>(parse("add_to_trade",
        R"({"signal_id": 41, "risk_pct": 0.5})"));
    CHECK(add.kind == ExecutionKind::ADD);
    CHECK(*add.trade == 41);

    const auto close = std::get<Execution>(parse("close_trade", R"({"signal_id": 41})"));
    CHECK(close.kind == ExecutionKind::CLOSE);
    CHECK(close.until_flat);
    CHECK(close.style == Style::ACCUMULATE);
    CHECK(close.clip_style == Style::TAKE);
    CHECK(close.clip_usd == kCloseClipUsd);
    CHECK(close.every_ms == kCloseEveryMs);
    CHECK(close.cap_bps == kCloseCrossBps);
    CHECK(close.ttl_ms == kUntilFlatTtlMs);

    const auto paced = std::get<Execution>(parse("close_trade",
        R"({"signal_id": 41, "clip_usd": 800, "every_s": 10, "cross_bps": 15})"));
    CHECK(paced.until_flat);
    CHECK(paced.clip_usd == 800);
    CHECK(paced.every_ms == 10000);
    CHECK(paced.cap_bps == 15);

    const auto capped = std::get<Execution>(parse("close_trade", R"({"signal_id": 41, "cap_bps": 20})"));
    CHECK_FALSE(capped.until_flat);
    CHECK(capped.style == Style::TAKE);
    CHECK(capped.cap_bps == 20);

    const auto brackets = std::get<SetBrackets>(parse("set_brackets",
        R"({"signal_id": 41, "stop": null, "tp": 72000})"));
    CHECK(brackets.stop.has_value());
    CHECK_FALSE(brackets.stop->has_value());
    CHECK((*brackets.tp)->policy.price == 72000);
    CHECK((*brackets.tp)->price_only);

    const auto ladder = std::get<SetBrackets>(parse("set_brackets",
        R"({"signal_id": 41, "tp": {"price": 72000, "rungs": 120, "step_bp": 25,
            "size_jitter": 0.2, "gap_s": 5},
            "stop": {"price": 65000, "cross_bps": 80, "clip_usd": 2000, "clip_jitter": 0.2,
            "every_s": 2, "every_jitter": 0.4, "clip_style": "passive",
            "clip_life_s": [1, 3]}})"));
    const auto& tp = (*ladder.tp)->policy;
    CHECK_FALSE((*ladder.tp)->price_only);
    CHECK(tp.rungs == 120);
    CHECK(tp.step_bp == 25);
    CHECK(tp.size_jitter == 0.2);
    CHECK(tp.gap_lo_ms == 5000);
    CHECK(tp.gap_hi_ms == 5000);
    const auto& stop = (*ladder.stop)->policy;
    CHECK(stop.cross_bps == 80);
    CHECK(stop.clip_usd == 2000);
    CHECK(stop.clip_jitter == 0.2);
    CHECK(stop.every_ms == 2000);
    CHECK(stop.every_jitter == 0.4);
    CHECK(stop.clip_style == Style::PASSIVE);
    CHECK(stop.clip_life_lo_ms == 1000);
    CHECK(stop.clip_life_hi_ms == 3000);

    const auto alias = std::get<SetBrackets>(parse("set_brackets", R"({"signal_id": 41, "stop_cap_bps": 40})"));
    CHECK(*alias.stop_cap_bps == 40);

    const auto cancel = std::get<CancelAllOrders>(parse("cancel_all_orders",
        R"({"signal_id": 41, "filter": "reduce_only"})"));
    CHECK(*cancel.trade == 41);
    CHECK(cancel.filter == OrderFilter::REDUCE_ONLY);
}

TEST_CASE("discretionary commands reject what they cannot run", "[discretionary]") {
    const auto refuses = [](const std::string& op, const std::string& payload,
                            const std::string& message) {
        CHECK_THROWS_WITH(parse(op, payload), ContainsSubstring(message));
    };
    const std::string trade = R"("setup_id": 1, "ev_expectation": 1, )";
    refuses("place_order", "{" + trade + R"("symbol": "BTCUSDT", "side": "buy", "price": 1})",
            "exactly one of qty");
    refuses("place_order", "{" + trade + R"("symbol": "BTCUSDT", "side": "buy", "qty": 1, "notional": 5, "price": 1})",
            "exactly one of qty");
    refuses("place_order", "{" + trade + R"("symbol": "BTCUSDT", "side": "long", "qty": 1, "price": 1})",
            "side must be");
    refuses("place_order", "{" + trade + R"("symbol": "BTCUSDT", "side": "buy", "qty": -1, "price": 1})",
            "qty must be positive");
    refuses("place_order", R"({"qty": 1, "price": 1, "reduce_only": true})",
            "a reduce-only order needs signal_id");
    refuses("open_trade", R"({"symbol": "BTCUSDT", "side": "buy", "qty": 1, "ev_expectation": 1})",
            "setup_id must be an integer");
    refuses("open_trade", R"({"symbol": "BTCUSDT", "side": "buy", "qty": 1, "setup_id": 1})",
            "ev_expectation must be a number");
    refuses("open_trade", "{" + trade + R"("symbol": "BTCUSDT", "side": "buy", "risk_pct": 1})",
            "risk_pct needs stop");
    refuses("open_trade", "{" + trade + R"("symbol": "BTCUSDT", "side": "buy", "fraction": 0.5})",
            "fraction");
    refuses("reduce_trade", R"({"signal_id": 4, "fraction": 1.5})", "fraction");
    refuses("reduce_trade", R"({"signal_id": 4, "risk_pct": 1})", "risk_pct");
    refuses("open_trade", "{" + trade + R"("symbol": "BTCUSDT", "side": "buy", "qty": 1, "style": "twap"})",
            "twap was replaced by accumulate");
    refuses("open_trade", "{" + trade + R"("symbol": "BTCUSDT", "side": "buy", "qty": 1, "style": "accumulate"})",
            "accumulate needs clip_usd");
    const std::string acc = R"("signal_id": 9, "fraction": 1, "style": "accumulate", "clip_usd": 100, "every_s": 10, )";
    refuses("reduce_trade", R"({"signal_id": 9, "fraction": 1, "style": "accumulate", "clip_usd": 100})",
            "accumulate needs every_s");
    refuses("reduce_trade", "{" + acc + R"("clip_jitter": 0})", "clip_jitter must be 0.1 to 0.9");
    refuses("reduce_trade", "{" + acc + R"("every_jitter": 0.95})", "every_jitter must be 0.1 to 0.9");
    refuses("reduce_trade", R"({"signal_id": 9, "fraction": 1, "style": "accumulate", "clip_usd": 100, "every_s": 1})",
            "every_s must be at least 2");
    refuses("reduce_trade", "{" + acc + R"("ttl_s": 5})", "ttl_s must be 10 to 86400");
    refuses("reduce_trade", "{" + acc + R"("max_slippage_bps": 2000})", "max_slippage_bps must be 1 to 1000");
    refuses("reduce_trade", "{" + acc + R"("clip_life_s": 0.5})", "clip_life_s must be at least 1");
    refuses("reduce_trade", "{" + acc + R"("cross_bps": 500})", "cross_bps must be at most 200");
    refuses("reduce_trade", "{" + acc + R"("cap_bps": 5})", "accumulate prices clips by cross_bps");
    refuses("reduce_trade", "{" + acc + R"("deadline_ms": 60000})", "accumulate ends by ttl_s");
    refuses("reduce_trade", "{" + acc + R"("duration_ms": 60000})", "duration_ms is a twap field");
    refuses("reduce_trade", R"({"signal_id": 9, "fraction": 1, "cap_bps": 500})", "cap_bps must be at most 200");
    refuses("add_to_trade", R"({"qty": 1})", "signal_id must be an integer");
    refuses("close_trade", R"({"signal_id": 4, "qty": 1})", "give no size");
    refuses("close_trade", R"({"signal_id": 4, "ttl_s": 60})", "works until flat");
    refuses("close_trade", R"({"signal_id": 4, "max_slippage_bps": 50})", "works until flat");
    refuses("flatten_all", R"({"cap_bps": 500})", "at most 200");
    refuses("cancel_order", R"({"order_id": 1, "command_id": 2})", "exactly one of order_id");
    refuses("replace_order", R"({"order_id": 1})", "price or qty");
    refuses("set_brackets", R"({"signal_id": 4})", "null clears");
    refuses("set_brackets", R"({"signal_id": 4, "tp": {"price": 1, "rungs": 3}})", "needs step_bp");
    refuses("set_brackets", R"({"signal_id": 4, "tp": {"price": 1, "rungs": 501, "step_bp": 1}})",
            "rungs must be 1 to 500");
    refuses("set_brackets", R"({"signal_id": 4, "tp": {"price": 1, "size_jitter": 0.95}})",
            "size_jitter must be 0.1 to 0.9");
    refuses("set_brackets", R"({"signal_id": 4, "tp": {"price": 1, "gap_s": [9, 3]}})",
            "low must not exceed high");
    for (const char* gone : {"levels", "clip_usd", "ttl_s"}) {
        refuses("set_brackets", std::string(R"({"signal_id": 4, "tp": {"price": 1, ")") + gone + R"(": 3}})",
                "was replaced");
    }
    refuses("set_brackets", R"({"signal_id": 4, "stop": "low"})", "stop must be a price or an object");
    for (const char* gone : {"cap_bps", "gap_s", "escalate_s", "deadline_s"}) {
        refuses("set_brackets",
                std::string(R"({"signal_id": 4, "stop": {"price": 1, "clip_usd": 100, ")") + gone + R"(": 30}})",
                "was replaced");
    }
    refuses("set_brackets", R"({"signal_id": 4, "stop": {"price": 1}})", "a stop needs clip_usd");
    refuses("set_brackets", R"({"signal_id": 4, "stop": {"price": 1, "clip_usd": 0}})", "clip_usd must be positive");
    refuses("set_brackets", R"({"signal_id": 4, "stop": {"price": 1, "clip_usd": [100, 200]}})",
            "stop clip_usd is one number");
    refuses("set_brackets", R"({"signal_id": 4, "stop": {"price": 1, "clip_usd": 100, "every_s": 0.5}})",
            "every_s must be at least 1");
    refuses("set_brackets", R"({"signal_id": 4, "stop": {"price": 1, "clip_usd": 100, "cross_bps": 500}})",
            "at most 200");
    refuses("set_brackets", R"({"signal_id": 4, "stop": {"price": 1, "clip_usd": 100, "clip_style": "accumulate"}})",
            "clip_style must be take or passive");
    refuses("open_trade", "{" + trade + R"("symbol": "BTCUSDT", "side": "buy", "qty": 1, "stop": 60000})",
            "a new stop needs clip_usd");
    refuses("open_trade", "{" + trade + R"("symbol": "BTCUSDT", "side": "buy", "qty": 1, "stop_cap_bps": 20})",
            "stop_cap_bps needs stop");
    refuses("launch", R"({})", "unknown op");
}

TEST_CASE("taking orders are bounded by the cap and the operator's limit",
          "[discretionary]") {
    CHECK_THAT(ExecutionMath::bound(Side::BUY, 100, 50, std::nullopt), WithinRel(100.5));
    CHECK_THAT(ExecutionMath::bound(Side::SELL, 100, 50, std::nullopt), WithinRel(99.5));
    CHECK(ExecutionMath::bound(Side::BUY, 100, 50, 100.2) == 100.2);
    CHECK(ExecutionMath::bound(Side::SELL, 100, 50, 99.8) == 99.8);
    CHECK(ExecutionMath::within(Side::BUY, 100.4, 100.5));
    CHECK_FALSE(ExecutionMath::within(Side::SELL, 99.4, 99.5));

    const Quote book{.bid = 99.9, .ask = 100.1, .last = 100};
    CHECK(*ExecutionMath::touch(Side::BUY, book) == 100.1);
    CHECK(*ExecutionMath::join(Side::SELL, book) == 100.1);
    CHECK(ExecutionMath::crosses(Side::BUY, 100.1, book));
    CHECK_FALSE(ExecutionMath::crosses(Side::BUY, 100.0, book));
    const Quote last_only{.last = 100};
    CHECK(*ExecutionMath::touch(Side::BUY, last_only) == 100);
}

TEST_CASE("an accumulation releases clips on a randomized rhythm until its ttl", "[discretionary]") {
    const AccumulationPlan plan{.start_ms = 1000, .ttl_ms = 600'000, .clip_usd = 500, .clip_jitter = 0.3,
                                .every_ms = 10'000, .every_jitter = 0.5, .seed = 42};
    CHECK(ExecutionMath::accumulate_release_ms(plan, 0) == 1000);
    CHECK(ExecutionMath::accumulate_clip_at(plan, 999) == -1);
    CHECK(ExecutionMath::accumulate_clip_at(plan, 1000) == 0);
    std::vector<long long> gaps;
    for (int k = 1; k < 200; ++k) {
        const auto gap = ExecutionMath::accumulate_release_ms(plan, k) - ExecutionMath::accumulate_release_ms(plan, k - 1);
        CHECK(gap >= 4'999);
        CHECK(gap <= 15'001);
        gaps.push_back(gap);
        const double clip = ExecutionMath::accumulate_clip_usd(plan, k);
        CHECK(clip >= 350);
        CHECK(clip <= 650);
        const auto release = ExecutionMath::accumulate_release_ms(plan, k);
        if (release < plan.start_ms + plan.ttl_ms) {
            CHECK(ExecutionMath::accumulate_clip_at(plan, release) == k);
            CHECK(ExecutionMath::accumulate_clip_at(plan, release - 1) == k - 1);
        }
    }
    CHECK(std::adjacent_find(gaps.begin(), gaps.end(), std::not_equal_to<>()) != gaps.end());

    // Nothing is released at or after the end of the ttl.
    const int last = ExecutionMath::accumulate_clip_at(plan, 10'000'000);
    CHECK(ExecutionMath::accumulate_release_ms(plan, last) < plan.start_ms + plan.ttl_ms);
    CHECK(ExecutionMath::accumulate_release_ms(plan, last + 1) >= plan.start_ms + plan.ttl_ms);

    // Seeded: the same command keeps its rhythm, another differs.
    auto other = plan;
    other.seed = 43;
    CHECK(ExecutionMath::accumulate_release_ms(plan, 7) == ExecutionMath::accumulate_release_ms(plan, 7));
    CHECK(ExecutionMath::accumulate_release_ms(plan, 7) != ExecutionMath::accumulate_release_ms(other, 7));
}

TEST_CASE("an accumulation refuses clips the venue would not take", "[discretionary]") {
    CHECK_FALSE(ExecutionMath::clip_problem(500, 0.3, 100));
    CHECK_FALSE(ExecutionMath::clip_problem(50, 0.3, 0));
    CHECK_THAT(*ExecutionMath::clip_problem(120, 0.3, 100),
               ContainsSubstring("smallest clip $84 is under the venue minimum $100"));
}

TEST_CASE("fat-finger bounds refuse oversized and far-through orders", "[discretionary]") {
    CHECK_FALSE(ExecutionMath::notional_problem(1, 100'000));
    const auto big = ExecutionMath::notional_problem(4, 60'000);
    REQUIRE(big);
    CHECK_THAT(*big, ContainsSubstring("$240,000 is above the $100,000 per-command limit"));

    const Quote book{.bid = 99.9, .ask = 100, .last = 100};
    CHECK_FALSE(ExecutionMath::cross_problem(Side::BUY, 101.9, book));
    CHECK_FALSE(ExecutionMath::cross_problem(Side::BUY, 50, book));
    CHECK_THAT(*ExecutionMath::cross_problem(Side::BUY, 110, book), ContainsSubstring("10% through"));
    CHECK_FALSE(ExecutionMath::cross_problem(Side::SELL, 98, book));
    CHECK(ExecutionMath::cross_problem(Side::SELL, 90, book));
    CHECK_FALSE(ExecutionMath::cross_problem(Side::SELL, 500, book));

    CHECK(ExecutionMath::bracket_near(60, 100));
    CHECK_FALSE(ExecutionMath::bracket_near(40, 100));
    CHECK_FALSE(ExecutionMath::bracket_near(160, 100));
}

TEST_CASE("entries size by qty, notional or risk to the stop", "[discretionary]") {
    CHECK(*ExecutionMath::entry_qty({.qty = 2}, 50, 1000, std::nullopt) == 2);
    CHECK(*ExecutionMath::entry_qty({.notional = 500}, 50, 1000, std::nullopt) == 10);
    // 1% of 10000 NAV lost over a 5-point stop distance.
    CHECK(*ExecutionMath::entry_qty({.risk_pct = 1}, 100, 10000, 95.0) == 20);
    CHECK_FALSE(ExecutionMath::entry_qty({.risk_pct = 1}, 100, 10000, std::nullopt));
    CHECK(ExecutionMath::reduce_qty({.fraction = 0.25}, 8) == 2);
    CHECK(ExecutionMath::reduce_qty({.qty = 20}, 8) == 8);
}

TEST_CASE("a remainder under half a size step is flat", "[discretionary]") {
    // Fills summing to 40275.700000000004 against a 40275.7 exit leave 7e-12.
    CHECK(ExecutionMath::is_dust(40275.700000000004 - 40275.7, 0.1));
    CHECK(ExecutionMath::is_dust(40275.700000000004 - 40275.7, 0));
    CHECK(ExecutionMath::is_dust(0.04, 0.1));
    CHECK_FALSE(ExecutionMath::is_dust(0.1, 0.1));
    CHECK_FALSE(ExecutionMath::is_dust(1e-9, 0));
}

TEST_CASE("stops sit on the losing side and trigger on a cross", "[discretionary]") {
    CHECK(ExecutionMath::stop_valid(true, 95, 100));
    CHECK_FALSE(ExecutionMath::stop_valid(true, 105, 100));
    CHECK(ExecutionMath::stop_valid(false, 105, 100));
    CHECK(ExecutionMath::stop_hit(true, 95, 95));
    CHECK_FALSE(ExecutionMath::stop_hit(true, 95, 95.1));
    CHECK(ExecutionMath::stop_hit(false, 105, 105.5));
}

TEST_CASE("discretionary accounts isolate capital rates and durable settlements", "[discretionary][accounts]") {
    auto& accounts = Core::DiscretionaryAccounts::instance();
    accounts.acknowledge(accounts.pending());
    accounts.configure({{.id=100, .paper=true, .enabled=true, .capital=1000, .rate_10s=1},
                        {.id=200, .paper=true, .enabled=true, .capital=5000, .rate_10s=1},
                        {.id=300, .paper=false, .enabled=true, .capital=0}});
    REQUIRE(accounts.get(100)->capital == 1000);
    REQUIRE(accounts.get(200)->capital == 5000);
    REQUIRE_FALSE(accounts.get(300)->paper);
    CHECK(accounts.reserve_order(100, 1000));
    CHECK_FALSE(accounts.reserve_order(100, 1001));
    CHECK(accounts.reserve_order(200, 1001));
    CHECK(accounts.reserve_order(100, 11001));
    accounts.settle(18446744073709550000ULL, 100, 25);
    accounts.settle(18446744073709550000ULL, 100, 25);
    const auto checkpoint = accounts.pending();
    REQUIRE(checkpoint.size() == 1);
    REQUIRE(checkpoint.at("18446744073709550000").at("account_id") == 100);
    CHECK(accounts.get(100)->capital == 1000);
    accounts.acknowledge(checkpoint);
    CHECK(accounts.pending().empty());
    accounts.restore(checkpoint);
    REQUIRE(accounts.pending() == checkpoint);
    accounts.configure({{.id=100, .paper=true, .enabled=true, .capital=1025},
                        {.id=200, .paper=true, .enabled=true, .capital=5000}});
    accounts.acknowledge(checkpoint);
    CHECK(accounts.get(100)->capital == 1025);
    CHECK(accounts.get(200)->capital == 5000);
    CHECK_FALSE(accounts.get(999));
    accounts.configure({});
}

TEST_CASE("account settlement checkpoints retain required trading state fields", "[discretionary][accounts]") {
    Live::TradingState state;
    state.discretionary_settlements={{"18446744073709550000",{{"account_id",1},{"pnl",25},{"signal_id",100}}}};
    const auto json=nlohmann::json(state);
    CHECK(json.get<Live::TradingState>()==state);
    auto legacy=json;
    legacy.erase("discretionary_settlements");
    CHECK(legacy.get<Live::TradingState>().discretionary_settlements.empty());
    legacy.erase("orders");
    CHECK_THROWS(legacy.get<Live::TradingState>());
    CHECK_THROWS(Core::DiscretionaryAccounts::instance().restore({{"invalid-id",{{"account_id",1},{"pnl",1}}}}));
}
