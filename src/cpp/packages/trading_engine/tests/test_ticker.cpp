// Unit tests for Core::TradeTicker (src/ticker.cpp).
//
// TradeTicker is the most complex piece of the engine — it owns order
// lifecycle state, signal buffers, rate-limit counters, and polymarket
// market data. Phase 1 focuses on:
//   - static pure helpers
//   - construction invariants
//   - stateless accessors (empty-state returns)
//   - signal buffer semantics
//   - ack_create_order state transitions
//   - rate-limit counter bookkeeping
//
// Deferred to phase 2+: mark_order_to_be_created (engine interaction),
// erase_order_from_state, on_order_filled_ack, polymarket bba/trade
// flow, hedge order detection.

#include <catch2/catch_test_macros.hpp>
#include <chrono>

#include "market_making/dtypes/order.h"
#include "market_making/dtypes/prediction.h"
#include "market_making/ticker.h"
#include "test_helpers.h"

namespace {

Core::Signal make_test_signal(unsigned long long signal_id,
                               const mpfr::mpreal& strength,
                               const std::string& symbol = "BTCUSDT") {
    Core::Signal sig;
    sig.signal_id = signal_id;
    sig.signal_strength = strength;
    sig.prediction = 0.5f;
    sig.price = 50000.0;
    sig.event_time_ms = 1700000000000;
    sig.bar_close_time_ms = 1700000000000;
    sig.strategy_id = 1;
    sig.symbol = symbol;
    return sig;
}

Core::Order make_simple_order() {
    return Core::Order::create(
        "BTCUSDT", Core::OrderType::LIMIT, Core::OrderSide::BUY,
        mpfr::mpreal("50000"), std::nullopt, mpfr::mpreal("0.1"), "binance",
        "swap", 1, 0, mpfr::mpreal("0"), std::nullopt, false, 1700000000000,
        "USDT", "BTC", false, false, Core::OrderTypeInternal::DEFAULT, false,
        std::nullopt, std::nullopt, std::nullopt, false, false, "USDT", {},
        std::nullopt, 1, std::nullopt);
}

}  // namespace

// ──────────────────────────────────────────────────────────────────────
// Section — static pure helpers
// ──────────────────────────────────────────────────────────────────────

TEST_CASE(
    "TradeTicker::get_order_cancel_key uses client_order_id for CEX orders",
    "[ticker][static]") {
    TestHelpers::ensure_minimal_engine();
    auto order = make_simple_order();
    order.exchange_order_id = "EXCH-1";  // Present but ignored for non-PM

    // Non-polymarket: always client_order_id regardless of exchange_order_id.
    // (ticker.cpp:212-217.)
    REQUIRE(Core::TradeTicker::get_order_cancel_key(order) ==
            order.client_order_id);
}

TEST_CASE(
    "TradeTicker::get_order_cancel_key prefers exchange_order_id for "
    "polymarket orders",
    "[ticker][static][polymarket]") {
    TestHelpers::ensure_minimal_engine();
    auto order = make_simple_order();
    order.is_prediction_market_order = true;
    order.exchange_order_id = "EXCH-PM";

    REQUIRE(Core::TradeTicker::get_order_cancel_key(order) == "EXCH-PM");

    // ...but falls back to client_order_id when exchange_order_id is empty.
    order.exchange_order_id = "";
    REQUIRE(Core::TradeTicker::get_order_cancel_key(order) ==
            order.client_order_id);
}

TEST_CASE("TradeTicker::get_clob_price_key composes clob_id and price",
          "[ticker][static][polymarket]") {
    auto key = Core::TradeTicker::get_clob_price_key("0xabc", "0.42");
    REQUIRE(key.find("0xabc") != std::string::npos);
    REQUIRE(key.find("0.42") != std::string::npos);
}

// ──────────────────────────────────────────────────────────────────────
// Section — construction invariants
// ──────────────────────────────────────────────────────────────────────

TEST_CASE("TradeTicker ctor populates identity fields",
          "[ticker][construction]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker("BTCUSDT", "USDT", "BTC", 42);

    REQUIRE(ticker->symbol == "BTCUSDT");
    REQUIRE(ticker->quote_asset == "USDT");
    REQUIRE(ticker->base_asset == "BTC");
    REQUIRE(ticker->strategy_id == 42);
}

TEST_CASE("TradeTicker ctor builds exchange_market_map from exchange_and_markets",
          "[ticker][construction]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();

    REQUIRE(ticker->exchange_and_markets.size() == 1);
    REQUIRE(ticker->exchange_market_map.size() == 1);
    REQUIRE(ticker->exchange_market_map.count("binance:swap") == 1);
    REQUIRE(ticker->exchange_market_map.at("binance:swap").account_id == 1);
}

TEST_CASE("TradeTicker fresh ctor has zero active orders and trades",
          "[ticker][construction][accessors]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();

    REQUIRE(ticker->get_num_active_orders() == 0);
    REQUIRE(ticker->get_num_active_orders_no_lock() == 0);
    REQUIRE(ticker->get_num_active_trades() == 0);
    REQUIRE(ticker->get_num_active_orders_and_trades() == 0);
}

TEST_CASE("TradeTicker fresh ctor has empty signals and counters",
          "[ticker][construction]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();

    REQUIRE(ticker->signals.empty());
    REQUIRE_FALSE(ticker->curr_signal.has_value());
    REQUIRE_FALSE(ticker->prev_signal.has_value());
    REQUIRE(ticker->order_count.load() == 0);
    REQUIRE(ticker->active_trade_count.load() == 0);
    REQUIRE(ticker->unacked_enter_order_count.load() == 0);
    REQUIRE(ticker->unacked_exit_order_count.load() == 0);
    REQUIRE_FALSE(ticker->is_signal_actionable.load());
}

// ──────────────────────────────────────────────────────────────────────
// Section — order book state (empty)
// ──────────────────────────────────────────────────────────────────────

TEST_CASE("TradeTicker::{bid,ask}_order_exists_on_book return false for empty",
          "[ticker][accessors][book]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();

    REQUIRE_FALSE(ticker->bid_order_exists_on_book(50000.0));
    REQUIRE_FALSE(ticker->ask_order_exists_on_book(50000.0));

    // Polymarket-style clob price key lookup also returns false.
    REQUIRE_FALSE(ticker->bid_order_exists_on_book(std::string("0xabc:0.5")));
    REQUIRE_FALSE(ticker->ask_order_exists_on_book(std::string("0xabc:0.5")));
}

TEST_CASE("TradeTicker::get_num_enter_orders returns 0 for fresh ticker",
          "[ticker][accessors]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();

    REQUIRE(ticker->get_num_enter_orders() == 0);
    REQUIRE(ticker->get_num_enter_orders("binance", "swap") == 0);
}

// ──────────────────────────────────────────────────────────────────────
// Section — signal buffer semantics
// ──────────────────────────────────────────────────────────────────────

TEST_CASE("TradeTicker::add_signal appends and tracks curr/prev",
          "[ticker][signals]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();
    auto strategy = TestHelpers::make_test_strategy();

    auto sig1 = make_test_signal(1, mpfr::mpreal("0.5"));
    ticker->add_signal(sig1, *strategy);
    REQUIRE(ticker->signals.size() == 1);
    REQUIRE(ticker->curr_signal.has_value());
    REQUIRE(ticker->curr_signal->signal_id == 1);
    REQUIRE_FALSE(ticker->prev_signal.has_value());

    auto sig2 = make_test_signal(2, mpfr::mpreal("0.3"));
    ticker->add_signal(sig2, *strategy);
    REQUIRE(ticker->signals.size() == 2);
    REQUIRE(ticker->curr_signal->signal_id == 2);
    REQUIRE(ticker->prev_signal.has_value());
    REQUIRE(ticker->prev_signal->signal_id == 1);
}

TEST_CASE("TradeTicker::add_signal with non-zero strength sets actionable",
          "[ticker][signals]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();
    auto strategy = TestHelpers::make_test_strategy();

    ticker->add_signal(make_test_signal(1, mpfr::mpreal("0.5")), *strategy);
    REQUIRE(ticker->is_signal_actionable.load());
}

TEST_CASE("TradeTicker::add_signal with zero strength clears actionable",
          "[ticker][signals]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();
    auto strategy = TestHelpers::make_test_strategy();

    ticker->add_signal(make_test_signal(1, mpfr::mpreal("0.5")), *strategy);
    REQUIRE(ticker->is_signal_actionable.load());

    // Zero-strength signal disarms the flag — this is the state-exit path.
    ticker->add_signal(make_test_signal(2, mpfr::mpreal("0")), *strategy);
    REQUIRE_FALSE(ticker->is_signal_actionable.load());
}

TEST_CASE("TradeTicker::add_signal with max_signals collapses when over cap",
          "[ticker][signals]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();
    auto strategy = TestHelpers::make_test_strategy();

    // Semantics (ticker.cpp:1150-1161): when size exceeds max_signals,
    // clear to just [latest]. Accumulates from there.
    // Trace: pushes produce sizes 1,2,3,4(→collapse to [4]), then 2 = [4,5].
    for (int i = 1; i <= 5; ++i) {
        ticker->add_signal(make_test_signal(i, mpfr::mpreal("0.1")),
                           *strategy, /*max_signals=*/3);
    }
    REQUIRE(ticker->signals.size() == 2);
    REQUIRE(ticker->signals.back().signal_id == 5);

    // Exactly max_signals+1 triggers one collapse; one more push gives size=2.
    auto ticker2 = TestHelpers::make_test_ticker();
    for (int i = 1; i <= 4; ++i) {
        ticker2->add_signal(make_test_signal(i, mpfr::mpreal("0.1")),
                            *strategy, /*max_signals=*/3);
    }
    // 1,2,3,4 → collapse at size=4 → [4]. Size = 1.
    REQUIRE(ticker2->signals.size() == 1);
    REQUIRE(ticker2->signals.back().signal_id == 4);
}

TEST_CASE("TradeTicker::add_signal_by_model stores per-model signal",
          "[ticker][signals]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();
    auto strategy = TestHelpers::make_test_strategy();

    auto sig_a = make_test_signal(100, mpfr::mpreal("0.7"));
    auto sig_b = make_test_signal(200, mpfr::mpreal("-0.3"));
    ticker->add_signal_by_model("model_a", sig_a, *strategy);
    ticker->add_signal_by_model("model_b", sig_b, *strategy);

    auto got_a = ticker->get_signal_by_model("model_a");
    REQUIRE(got_a.has_value());
    REQUIRE(got_a->signal_id == 100);

    auto got_b = ticker->get_signal_by_model("model_b");
    REQUIRE(got_b.has_value());
    REQUIRE(got_b->signal_id == 200);
}

TEST_CASE("TradeTicker::get_signal_by_model returns nullopt for unknown model",
          "[ticker][signals]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();

    auto got = ticker->get_signal_by_model("never_added");
    REQUIRE_FALSE(got.has_value());
}

TEST_CASE("TradeTicker::get_last_signal returns most recently added",
          "[ticker][signals]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();
    auto strategy = TestHelpers::make_test_strategy();

    // Empty state
    REQUIRE_FALSE(ticker->get_last_signal().has_value());

    ticker->add_signal(make_test_signal(1, mpfr::mpreal("0.1")), *strategy);
    ticker->add_signal(make_test_signal(2, mpfr::mpreal("0.2")), *strategy);

    auto last = ticker->get_last_signal();
    REQUIRE(last.has_value());
    REQUIRE(last->signal_id == 2);
}

TEST_CASE(
    "TradeTicker::get_last_actionable_signal only returns when actionable",
    "[ticker][signals]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();
    auto strategy = TestHelpers::make_test_strategy();

    // Non-zero strength → actionable
    ticker->add_signal(make_test_signal(1, mpfr::mpreal("0.5")), *strategy);
    auto actionable = ticker->get_last_actionable_signal();
    REQUIRE(actionable.has_value());
    REQUIRE(actionable->signal_id == 1);

    // Zero-strength signal clears the flag → still returns nullopt.
    ticker->add_signal(make_test_signal(2, mpfr::mpreal("0")), *strategy);
    REQUIRE_FALSE(ticker->get_last_actionable_signal().has_value());
}

TEST_CASE("TradeTicker::get_prediction reads curr_signal",
          "[ticker][signals]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();
    auto strategy = TestHelpers::make_test_strategy();

    REQUIRE_FALSE(ticker->get_prediction().has_value());

    ticker->add_signal(make_test_signal(42, mpfr::mpreal("0.5")), *strategy);
    auto pred = ticker->get_prediction();
    REQUIRE(pred.has_value());
    REQUIRE(pred->signal_id == 42);
}

// ──────────────────────────────────────────────────────────────────────
// Section — ack_create_order state transitions
// ──────────────────────────────────────────────────────────────────────

TEST_CASE("TradeTicker::ack_create_order no-op when order not in state",
          "[ticker][lifecycle]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();
    auto order = make_simple_order();

    // Order was never added to ticker's orders vector.
    // Expectation: early-return, no throw.
    REQUIRE_NOTHROW(
        ticker->ack_create_order(Core::Engine::getInstance(), order));
    REQUIRE(ticker->orders.empty());
}

TEST_CASE("TradeTicker::ack_create_order sets ack flag on existing order",
          "[ticker][lifecycle]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();
    auto order = make_simple_order();
    ticker->orders.push_back(order);
    REQUIRE_FALSE(ticker->orders.front().is_order_creation_acknowledged);

    ticker->ack_create_order(Core::Engine::getInstance(), order);
    REQUIRE(ticker->orders.front().is_order_creation_acknowledged);
    // ack_create_timestamp reflects engine.get_time(). In backtest mode
    // with a fresh engine, time starts at 0 — just assert the write path
    // fired (flag above) rather than a specific timestamp value.
    REQUIRE(ticker->orders.front().ack_create_timestamp ==
            Core::Engine::getInstance().get_time());
}

TEST_CASE("TradeTicker::ack_create_order is idempotent on already-acked",
          "[ticker][lifecycle]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();
    auto order = make_simple_order();
    order.is_order_creation_acknowledged = true;
    order.ack_create_timestamp = 42;
    ticker->orders.push_back(order);

    // Already-acked: early return, timestamp should not change.
    ticker->ack_create_order(Core::Engine::getInstance(), order);
    REQUIRE(ticker->orders.front().ack_create_timestamp == 42);
}

// ──────────────────────────────────────────────────────────────────────
// Section — rate-limit counters
// ──────────────────────────────────────────────────────────────────────

TEST_CASE("TradeTicker::record_enter_order_sent increments 10s window",
          "[ticker][rate-limit]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();

    REQUIRE(ticker->get_recent_enter_orders_count_10s() == 0);
    ticker->record_enter_order_sent();
    ticker->record_enter_order_sent();
    ticker->record_enter_order_sent();
    REQUIRE(ticker->get_recent_enter_orders_count_10s() == 3);

    // Independent counter for exits.
    REQUIRE(ticker->get_recent_exit_orders_count_10s() == 0);
    ticker->record_exit_order_sent();
    REQUIRE(ticker->get_recent_exit_orders_count_10s() == 1);
    // Enter count should be unaffected.
    REQUIRE(ticker->get_recent_enter_orders_count_10s() == 3);
}

TEST_CASE(
    "TradeTicker::get_recent_enter_orders_count honors arbitrary window",
    "[ticker][rate-limit]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();

    ticker->record_enter_order_sent();
    // 1-second and 60-second windows both see the just-recorded event.
    REQUIRE(ticker->get_recent_enter_orders_count(std::chrono::seconds(1)) ==
            1);
    REQUIRE(ticker->get_recent_enter_orders_count(std::chrono::seconds(60)) ==
            1);
    // Zero-second window never matches (cutoff == now, pushed event <= cutoff).
    REQUIRE(ticker->get_recent_enter_orders_count(std::chrono::seconds(0)) ==
            0);
}

// ──────────────────────────────────────────────────────────────────────
// Section — is_cancel_ready_to_be_sent per-venue gating
// ──────────────────────────────────────────────────────────────────────

TEST_CASE(
    "TradeTicker::is_cancel_ready_to_be_sent waits for the Binance create ack",
    "[ticker][cancel-readiness]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();
    auto order = make_simple_order();

    REQUIRE_FALSE(ticker->is_cancel_ready_to_be_sent(order));
    order.is_order_creation_acknowledged = true;
    REQUIRE(ticker->is_cancel_ready_to_be_sent(order));
}

TEST_CASE(
    "TradeTicker::is_cancel_ready_to_be_sent blocks Aster without creation ack",
    "[ticker][cancel-readiness][aster]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();
    auto order = make_simple_order();
    order.exchange_name = "aster";
    order.exchange_enum = Core::Exchange::ASTER;
    order.exchange_order_id = "EXCH-42";
    order.is_order_creation_acknowledged = false;

    // Ticker.cpp:112-117: Aster requires BOTH ack AND exchange_order_id.
    REQUIRE_FALSE(ticker->is_cancel_ready_to_be_sent(order));
}

TEST_CASE(
    "TradeTicker::is_cancel_ready_to_be_sent blocks Aster without "
    "exchange_order_id",
    "[ticker][cancel-readiness][aster]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();
    auto order = make_simple_order();
    order.exchange_name = "aster";
    order.exchange_enum = Core::Exchange::ASTER;
    order.is_order_creation_acknowledged = true;
    order.exchange_order_id = "";  // missing → cannot cancel on Aster

    REQUIRE_FALSE(ticker->is_cancel_ready_to_be_sent(order));
}

TEST_CASE(
    "TradeTicker::is_cancel_ready_to_be_sent permits Aster when ack + "
    "exchange_order_id both present",
    "[ticker][cancel-readiness][aster]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();
    auto order = make_simple_order();
    order.exchange_name = "aster";
    order.exchange_enum = Core::Exchange::ASTER;
    order.is_order_creation_acknowledged = true;
    order.exchange_order_id = "EXCH-42";

    REQUIRE(ticker->is_cancel_ready_to_be_sent(order));
}

TEST_CASE(
    "TradeTicker::is_cancel_ready_to_be_sent gates polymarket on "
    "is_polymarket_order_res_success",
    "[ticker][cancel-readiness][polymarket]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();
    auto order = make_simple_order();
    order.is_prediction_market_order = true;

    order.is_polymarket_order_res_success = false;
    REQUIRE_FALSE(ticker->is_cancel_ready_to_be_sent(order));

    order.is_polymarket_order_res_success = true;
    REQUIRE(ticker->is_cancel_ready_to_be_sent(order));
}

// ──────────────────────────────────────────────────────────────────────
// Section — is_cancel_already_sent TTL semantics
// ──────────────────────────────────────────────────────────────────────

TEST_CASE("TradeTicker::is_cancel_already_sent returns false for unknown key",
          "[ticker][cancel-tracking]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();

    REQUIRE_FALSE(ticker->is_cancel_already_sent(std::string("missing-key")));
}

TEST_CASE("TradeTicker::is_cancel_already_sent returns true for fresh entry",
          "[ticker][cancel-tracking]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();
    auto order = make_simple_order();

    ticker->inject_pending_cancel(order.client_order_id, order,
                                   std::chrono::steady_clock::now());
    REQUIRE(ticker->is_cancel_already_sent(order.client_order_id));
}

TEST_CASE(
    "TradeTicker::is_cancel_already_sent expires CEX entries after 5s",
    "[ticker][cancel-tracking]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();
    auto order = make_simple_order();

    // Stale entry — 6s older than now. CEX TTL is 5000ms (ticker.cpp:1519).
    auto stale = std::chrono::steady_clock::now() - std::chrono::seconds(6);
    ticker->inject_pending_cancel(order.client_order_id, order, stale);

    REQUIRE_FALSE(ticker->is_cancel_already_sent(order.client_order_id));
    // TTL expiration also deletes the map entry — second check stays false.
    REQUIRE_FALSE(ticker->is_cancel_already_sent(order.client_order_id));
}

TEST_CASE(
    "TradeTicker::is_cancel_already_sent uses 1s TTL when prediction market",
    "[ticker][cancel-tracking][polymarket]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();
    ticker->is_trading_on_prediction_market.store(true);
    auto order = make_simple_order();

    // 1.5s back — beyond PM 1000ms TTL but inside the CEX 5000ms TTL.
    // Confirms the ticker uses the PM window when its flag is set.
    auto stale = std::chrono::steady_clock::now() -
                 std::chrono::milliseconds(1500);
    ticker->inject_pending_cancel(order.client_order_id, order, stale);

    REQUIRE_FALSE(ticker->is_cancel_already_sent(order.client_order_id));
}

// ──────────────────────────────────────────────────────────────────────
// Section — Polymarket outcome state (BBA, trade flow, inventory)
// ──────────────────────────────────────────────────────────────────────

TEST_CASE("TradeTicker::read_outcome_bba returns nullopt before any update",
          "[ticker][polymarket][bba]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();

    // local_receive_time_ms starts at 0 — read guards against it
    // (ticker.cpp:1084) and returns nullopt.
    REQUIRE_FALSE(ticker->read_outcome_bba(0).has_value());
    REQUIRE_FALSE(ticker->read_outcome_bba(1).has_value());
}

TEST_CASE("TradeTicker outcome BBA update + read roundtrip",
          "[ticker][polymarket][bba]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();

    ticker->update_outcome_best_bid_ask(/*outcome_index=*/0, 0.42, 0.44,
                                        1700000001000);
    auto snap = ticker->read_outcome_bba(0);
    REQUIRE(snap.has_value());
    REQUIRE(snap->best_bid == 0.42);
    REQUIRE(snap->best_ask == 0.44);
    REQUIRE(snap->timestamp_ms == 1700000001000);
    REQUIRE(snap->local_receive_time_ms > 0);  // wall-clock populated

    // Second outcome stays untouched.
    REQUIRE_FALSE(ticker->read_outcome_bba(1).has_value());
}

TEST_CASE(
    "TradeTicker outcome BBA update with out-of-range index is a no-op",
    "[ticker][polymarket][bba]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();

    ticker->update_outcome_best_bid_ask(-1, 0.5, 0.5, 1);        // neg
    ticker->update_outcome_best_bid_ask(99, 0.5, 0.5, 1);        // > max

    // All valid indices remain unpopulated; no crash.
    for (int i = 0; i < Core::TradeTicker::MAX_POLYMARKET_OUTCOMES; ++i) {
        REQUIRE_FALSE(ticker->read_outcome_bba(i).has_value());
    }
    // Out-of-range reads also return nullopt.
    REQUIRE_FALSE(ticker->read_outcome_bba(-1).has_value());
    REQUIRE_FALSE(ticker->read_outcome_bba(99).has_value());
}

TEST_CASE("TradeTicker outcome trade flow accumulates buy + sell independently",
          "[ticker][polymarket][trade-flow]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();

    ticker->update_outcome_trade_flow(0, /*is_buy=*/true, 10.0, 0.5, 1000);
    ticker->update_outcome_trade_flow(0, /*is_buy=*/true, 5.0, 0.5, 1100);
    ticker->update_outcome_trade_flow(0, /*is_buy=*/false, 7.0, 0.5, 1200);

    auto snap = ticker->read_outcome_trade_flow(0);
    REQUIRE(snap.has_value());
    REQUIRE(snap->buy_volume == 15.0);
    REQUIRE(snap->buy_count == 2);
    REQUIRE(snap->sell_volume == 7.0);
    REQUIRE(snap->sell_count == 1);
    REQUIRE(snap->last_trade_price == 0.5);
    REQUIRE(snap->last_trade_ts == 1200);
}

TEST_CASE("TradeTicker outcome trade flow with out-of-range index ignored",
          "[ticker][polymarket][trade-flow]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();

    ticker->update_outcome_trade_flow(-1, true, 10.0, 0.5, 1000);
    ticker->update_outcome_trade_flow(99, true, 10.0, 0.5, 1000);
    // No assertion needed — just no crash. All valid indices stay at defaults.
    auto snap = ticker->read_outcome_trade_flow(0);
    REQUIRE(snap.has_value());
    REQUIRE(snap->buy_volume == 0.0);
    REQUIRE(snap->buy_count == 0);
}

TEST_CASE("TradeTicker outcome exchange inventory roundtrip",
          "[ticker][polymarket][inventory]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();

    // Fresh ctor zeroes the array.
    REQUIRE(ticker->get_outcome_exchange_inventory(0) == 0.0);
    REQUIRE(ticker->get_outcome_exchange_inventory(1) == 0.0);

    ticker->update_outcome_exchange_inventory(0, 100.0);
    ticker->update_outcome_exchange_inventory(1, 42.5);

    REQUIRE(ticker->get_outcome_exchange_inventory(0) == 100.0);
    REQUIRE(ticker->get_outcome_exchange_inventory(1) == 42.5);

    // Out-of-range returns 0.0 without throwing.
    REQUIRE(ticker->get_outcome_exchange_inventory(-1) == 0.0);
    REQUIRE(ticker->get_outcome_exchange_inventory(99) == 0.0);
}

// ──────────────────────────────────────────────────────────────────────
// Section — Event timing
// ──────────────────────────────────────────────────────────────────────

TEST_CASE("TradeTicker::get_time_elapsed_since_event_start_ms computes delta",
          "[ticker][polymarket][timing]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();

    // engine.get_time() is 0 in backtest mode with no market events
    // processed; elapsed = 0 - event_start.
    ticker->event_start_time_ms.store(5000);
    REQUIRE(ticker->get_time_elapsed_since_event_start_ms() == -5000);

    ticker->event_start_time_ms.store(-3000);
    REQUIRE(ticker->get_time_elapsed_since_event_start_ms() == 3000);
}

TEST_CASE("TradeTicker::get_time_left_in_event_ms computes delta",
          "[ticker][polymarket][timing]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();

    // remaining = event_end - engine.get_time() (which is 0).
    ticker->event_end_time_ms.store(7500);
    REQUIRE(ticker->get_time_left_in_event_ms() == 7500);

    ticker->event_end_time_ms.store(-1000);
    REQUIRE(ticker->get_time_left_in_event_ms() == -1000);
}

// ──────────────────────────────────────────────────────────────────────
// Section — configure_from_strategy propagates flags
// ──────────────────────────────────────────────────────────────────────

TEST_CASE(
    "TradeTicker::configure_from_strategy sets is_market_making_strategy",
    "[ticker][configure]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();
    REQUIRE_FALSE(ticker->is_market_making_strategy.load());

    auto db = TestHelpers::make_disabled_strategy_config();
    db.is_market_making_strategy = true;
    ticker->configure_from_strategy(db);

    REQUIRE(ticker->is_market_making_strategy.load());
}

TEST_CASE("TradeTicker::configure_from_strategy maps account_mode string",
          "[ticker][configure]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();
    // Default is SHARED (set in ticker.h).
    REQUIRE(ticker->account_mode == Core::AccountMode::SHARED);

    auto db = TestHelpers::make_disabled_strategy_config();
    db.account_mode = "isolated";
    ticker->configure_from_strategy(db);

    REQUIRE(ticker->account_mode == Core::AccountMode::ISOLATED);
}

// ──────────────────────────────────────────────────────────────────────
// Section — Fill routing dispatch + find_active_trade_for_fill
// (ticker.cpp:888-898 for routing; :178-216 for find)
// ──────────────────────────────────────────────────────────────────────

namespace {

Core::OrderFillInfo make_fill(unsigned long long order_id, double qty,
                               double price, long long ts,
                               std::optional<unsigned long long> trade_id =
                                   std::nullopt,
                               const std::string& clob_token_id = "") {
    Core::OrderFillInfo fi;
    fi.order_id = order_id;
    fi.fill_amount = mpfr::mpreal(std::to_string(qty));
    fi.price = mpfr::mpreal(std::to_string(price));
    fi.order_fill_timestamp = ts;
    fi.trade_id = trade_id;
    fi.clob_token_id = clob_token_id;
    fi.side = Core::OrderSide::BUY;
    fi.symbol = "BTCUSDT";
    fi.exchange_name = "binance";
    fi.market_type = "swap";
    fi.strategy_id = 1;
    fi.account_id = 1;
    return fi;
}

Core::Trade make_active_trade(unsigned long long id, unsigned long long signal_id,
                               const std::string& symbol = "BTCUSDT",
                               const std::string& exchange = "binance",
                               const std::string& market = "swap") {
    Core::Trade t;
    t.id = id;
    t.signal_id = signal_id;
    t.strategy_id = 1;
    t.symbol = symbol;
    t.exchange_name = exchange;
    t.market_type = market;
    t.account_id = 1;
    t.open_quantity = mpfr::mpreal("1");
    t.open_price = mpfr::mpreal("50000");
    t.type = Core::TradeType::LONG;
    t.type_internal = Core::TradeTypeInternal::DIRECTIONAL_TRADE;
    t.open_time_ms = 1700000000000;
    t.close_time_ms = 0;
    return t;
}

}  // namespace

TEST_CASE(
    "TradeTicker::on_order_filled_ack with MM flag + no matching order is a "
    "safe no-op",
    "[ticker][fill][routing]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();
    ticker->is_market_making_strategy.store(true);
    auto fill = make_fill(/*order_id=*/999, 0.1, 50000, 1700000001000);

    // No matching order → mm_trade_order_filled early-returns at
    // ticker.cpp:756-758. record_fill still runs at the end.
    REQUIRE_NOTHROW(
        ticker->on_order_filled_ack(Core::Engine::getInstance(), fill));

    // record_fill hook fired — recent_fills has exactly 1 entry.
    REQUIRE(ticker->recent_fills.size(1700000001000) == 1);
}

TEST_CASE(
    "TradeTicker::on_order_filled_ack without MM + no trade_id is a safe "
    "no-op (enter path)",
    "[ticker][fill][routing]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();
    auto fill = make_fill(999, 0.1, 50000, 1700000002000);
    REQUIRE_FALSE(fill.trade_id.has_value());  // enter path selector

    REQUIRE_NOTHROW(
        ticker->on_order_filled_ack(Core::Engine::getInstance(), fill));
    REQUIRE(ticker->recent_fills.size(1700000002000) == 1);
}

TEST_CASE(
    "TradeTicker::on_order_filled_ack without MM + trade_id present is a "
    "safe no-op (exit path)",
    "[ticker][fill][routing]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();
    auto fill = make_fill(999, 0.1, 50000, 1700000003000,
                          /*trade_id=*/42ull);

    REQUIRE_NOTHROW(
        ticker->on_order_filled_ack(Core::Engine::getInstance(), fill));
    REQUIRE(ticker->recent_fills.size(1700000003000) == 1);
}

TEST_CASE(
    "TradeTicker::find_active_trade_for_fill returns nullopt on empty "
    "active_trades (all 3 branches)",
    "[ticker][fill][find]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();
    auto fill = make_fill(1, 0.1, 50000, 1700000000000);
    auto order = make_simple_order();

    // Default branch (not PM, not MM) — empty active_trades → nullopt.
    auto got = ticker->find_active_trade_for_fill(fill, order);
    REQUIRE_FALSE(got.has_value());

    ticker->is_market_making_strategy.store(true);
    auto got_mm = ticker->find_active_trade_for_fill(fill, order);
    REQUIRE_FALSE(got_mm.has_value());
    ticker->is_market_making_strategy.store(false);

    ticker->is_trading_on_prediction_market.store(true);
    fill.clob_token_id = "0xabc";
    auto got_pm = ticker->find_active_trade_for_fill(fill, order);
    REQUIRE_FALSE(got_pm.has_value());
}

TEST_CASE(
    "TradeTicker::find_active_trade_for_fill MM branch matches on "
    "exchange+market",
    "[ticker][fill][find]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();
    ticker->is_market_making_strategy.store(true);

    // Populate two trades: one that matches, one that doesn't.
    auto t1 = make_active_trade(1, 0, "BTCUSDT", "binance", "swap");
    auto t2 = make_active_trade(2, 0, "ETHUSDT", "binance", "spot");
    ticker->active_trades.push_back(t1);
    ticker->active_trades.push_back(t2);

    auto fill = make_fill(1, 0.1, 50000, 1700000000000);
    auto order = make_simple_order();  // binance/swap default
    auto got = ticker->find_active_trade_for_fill(fill, order);
    REQUIRE(got.has_value());
    REQUIRE(got->get().id == 1);
}

TEST_CASE(
    "TradeTicker::find_active_trade_for_fill PM branch keys on clob_token_id",
    "[ticker][fill][find][polymarket]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();
    ticker->is_trading_on_prediction_market.store(true);

    auto t1 = make_active_trade(1, 0);
    t1.clob_token_id = "0xaaa";
    t1.trade_creation_type = Core::TradeCreationType::ORGANIC;
    auto t2 = make_active_trade(2, 0);
    t2.clob_token_id = "0xbbb";
    t2.trade_creation_type = Core::TradeCreationType::ORGANIC;
    ticker->active_trades.push_back(t1);
    ticker->active_trades.push_back(t2);

    auto fill = make_fill(1, 0.1, 0.5, 1700000000000, std::nullopt, "0xbbb");
    fill.trade_creation_type = Core::TradeCreationType::ORGANIC;
    auto order = make_simple_order();
    auto got = ticker->find_active_trade_for_fill(fill, order);
    REQUIRE(got.has_value());
    REQUIRE(got->get().id == 2);

    // Empty clob_token_id → nullopt.
    fill.clob_token_id = "";
    REQUIRE_FALSE(
        ticker->find_active_trade_for_fill(fill, order).has_value());
}

TEST_CASE(
    "TradeTicker::find_active_trade_for_fill default branch keys on "
    "signal_id via get_trade_by_signal_id",
    "[ticker][fill][find]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();
    auto t1 = make_active_trade(1, /*signal_id=*/1001);
    auto t2 = make_active_trade(2, /*signal_id=*/1002);
    ticker->active_trades.push_back(t1);
    ticker->active_trades.push_back(t2);

    auto fill = make_fill(1, 0.1, 50000, 1700000000000);
    auto order = make_simple_order();
    order.signal_id = 1002;  // matches t2
    order.symbol = "BTCUSDT";

    auto got = ticker->find_active_trade_for_fill(fill, order);
    REQUIRE(got.has_value());
    REQUIRE(got->get().id == 2);
}

// ──────────────────────────────────────────────────────────────────────
// Section — manually_remove_trade_by_id + move_trade_to_completed
// ──────────────────────────────────────────────────────────────────────

TEST_CASE(
    "TradeTicker::manually_remove_trade_by_id removes matching trade + "
    "updates count",
    "[ticker][trade-mgmt]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();
    ticker->active_trades.push_back(make_active_trade(1, 101));
    ticker->active_trades.push_back(make_active_trade(2, 102));
    ticker->active_trade_count.store(2);

    bool removed = ticker->manually_remove_trade_by_id(1);
    REQUIRE(removed);
    REQUIRE(ticker->active_trades.size() == 1);
    REQUIRE(ticker->active_trades.front().id == 2);
    REQUIRE(ticker->active_trade_count.load() == 1);
}

TEST_CASE(
    "TradeTicker::manually_remove_trade_by_id returns false for unknown id",
    "[ticker][trade-mgmt]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();
    ticker->active_trades.push_back(make_active_trade(1, 101));

    bool removed = ticker->manually_remove_trade_by_id(999);
    REQUIRE_FALSE(removed);
    REQUIRE(ticker->active_trades.size() == 1);
}

TEST_CASE(
    "TradeTicker::force_close_trade completes trade and clears active list",
    "[ticker][trade-mgmt]") {
    TestHelpers::ensure_minimal_engine();
    auto& engine = Core::Engine::getInstance();
    auto ticker = TestHelpers::make_test_ticker();
    auto trade = make_active_trade(1, 101);
    trade.open_quantity = mpfr::mpreal("0.5");
    ticker->active_trades.push_back(trade);
    ticker->active_trades.push_back(make_active_trade(2, 102));
    ticker->active_trade_count.store(2);

    nlohmann::json ctx;
    ctx["remaining_notional"] = 3.0;
    ctx["min_notional"] = 5.0;
    ctx["mark_price"] = 50000.0;

    REQUIRE_NOTHROW(ticker->force_close_trade(
        engine, 1, Core::ForceCloseReason::REMAINING_NOTIONAL_BELOW_MIN, ctx));

    REQUIRE(ticker->active_trades.size() == 1);
    REQUIRE(ticker->active_trades.front().id == 2);
    REQUIRE(ticker->active_trade_count.load() == 1);
    REQUIRE(ticker->trade_completion_times.size() == 1);
}

TEST_CASE(
    "TradeTicker::force_close_trade is a no-op for unknown trade id",
    "[ticker][trade-mgmt]") {
    TestHelpers::ensure_minimal_engine();
    auto& engine = Core::Engine::getInstance();
    auto ticker = TestHelpers::make_test_ticker();
    ticker->active_trades.push_back(make_active_trade(1, 101));
    ticker->active_trade_count.store(1);

    nlohmann::json ctx;
    ctx["remaining_notional"] = 3.0;
    ctx["min_notional"] = 5.0;
    ctx["mark_price"] = 50000.0;

    REQUIRE_NOTHROW(ticker->force_close_trade(
        engine, 999, Core::ForceCloseReason::REMAINING_NOTIONAL_BELOW_MIN,
        ctx));

    REQUIRE(ticker->active_trades.size() == 1);
    REQUIRE(ticker->active_trade_count.load() == 1);
    REQUIRE(ticker->trade_completion_times.empty());
}

TEST_CASE(
    "TradeTicker::move_trade_to_completed removes from active + updates count",
    "[ticker][trade-mgmt]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();
    auto t1 = make_active_trade(1, 101);
    ticker->active_trades.push_back(t1);
    ticker->active_trades.push_back(make_active_trade(2, 102));
    ticker->active_trade_count.store(2);

    auto& trade_ref = ticker->active_trades.front();
    ticker->move_trade_to_completed(trade_ref, /*close_time_ms=*/1700000099000);

    REQUIRE(ticker->active_trades.size() == 1);
    REQUIRE(ticker->active_trades.front().id == 2);
    REQUIRE(ticker->active_trade_count.load() == 1);
    REQUIRE(ticker->trade_completion_times.size() == 1);
}

// ──────────────────────────────────────────────────────────────────────
// Section — mark_order_to_be_created + erase_order_from_state
// (ticker.cpp:1594-1646 + :325-382)
// ──────────────────────────────────────────────────────────────────────

TEST_CASE("TradeTicker::mark_order_to_be_created is a no-op when qty is 0",
          "[ticker][order-lifecycle]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();
    auto strategy = TestHelpers::make_test_strategy();

    auto order = make_simple_order();
    order.quantity = mpfr::mpreal("0");
    Core::StrategyDecision sd;

    ticker->mark_order_to_be_created(Core::Engine::getInstance(), order, sd,
                                      *strategy);
    // Short-circuit at ticker.cpp:1597 — nothing mutates.
    REQUIRE(ticker->orders.empty());
    REQUIRE(ticker->order_count.load() == 0);
    REQUIRE(ticker->unacked_enter_order_count.load() == 0);
    REQUIRE(sd.orders.empty());
}

TEST_CASE(
    "TradeTicker::mark_order_to_be_created happy path populates orders + "
    "counters + strategy decision (BUY enter)",
    "[ticker][order-lifecycle]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();
    auto strategy = TestHelpers::make_test_strategy();
    auto order = make_simple_order();
    Core::StrategyDecision sd;

    ticker->mark_order_to_be_created(Core::Engine::getInstance(), order, sd,
                                      *strategy);

    REQUIRE(ticker->orders.size() == 1);
    REQUIRE(ticker->order_count.load() == 1);
    REQUIRE(ticker->unacked_enter_order_count.load() == 1);
    REQUIRE(ticker->unacked_exit_order_count.load() == 0);
    REQUIRE(ticker->active_bid_count == 1);
    REQUIRE(ticker->active_bid_notional ==
            (order.price * order.quantity).toDouble());
    REQUIRE(ticker->bid_order_count_by_price[order.price.toDouble()] == 1);
    REQUIRE(sd.orders.size() == 1);  // pushed into the decision too
    REQUIRE(ticker->get_recent_enter_orders_count_10s() == 1);
}

TEST_CASE(
    "TradeTicker::mark_order_to_be_created on SELL side populates ask counters",
    "[ticker][order-lifecycle]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();
    auto strategy = TestHelpers::make_test_strategy();
    auto order = make_simple_order();
    order.side = Core::OrderSide::SELL;
    Core::StrategyDecision sd;

    ticker->mark_order_to_be_created(Core::Engine::getInstance(), order, sd,
                                      *strategy);

    REQUIRE(ticker->active_ask_count == 1);
    REQUIRE(ticker->active_bid_count == 0);
    REQUIRE(ticker->ask_order_count_by_price[order.price.toDouble()] == 1);
    REQUIRE(ticker->bid_order_count_by_price.empty());
}

TEST_CASE(
    "TradeTicker::mark_order_to_be_created with is_trade_close_order "
    "increments unacked_exit instead of unacked_enter",
    "[ticker][order-lifecycle]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();
    auto strategy = TestHelpers::make_test_strategy();
    auto order = make_simple_order();
    order.is_trade_close_order = true;
    Core::StrategyDecision sd;

    ticker->mark_order_to_be_created(Core::Engine::getInstance(), order, sd,
                                      *strategy);

    REQUIRE(ticker->unacked_exit_order_count.load() == 1);
    REQUIRE(ticker->unacked_enter_order_count.load() == 0);
    REQUIRE(ticker->get_recent_exit_orders_count_10s() == 1);
    REQUIRE(ticker->get_recent_enter_orders_count_10s() == 0);
}

TEST_CASE(
    "TradeTicker::mark_order_to_be_created with trade_id appends order id "
    "to that trade's exit_order_ids",
    "[ticker][order-lifecycle]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();
    auto strategy = TestHelpers::make_test_strategy();
    auto trade = make_active_trade(/*id=*/555, /*signal_id=*/77);
    ticker->active_trades.push_back(trade);

    auto order = make_simple_order();
    order.trade_id = 555ull;
    order.is_trade_close_order = true;
    Core::StrategyDecision sd;

    ticker->mark_order_to_be_created(Core::Engine::getInstance(), order, sd,
                                      *strategy);

    REQUIRE(ticker->active_trades.front().exit_order_ids.size() == 1);
    REQUIRE(ticker->active_trades.front().exit_order_ids.front() == order.id);
}

TEST_CASE(
    "TradeTicker::erase_order_from_state decrements counters + removes order",
    "[ticker][order-lifecycle]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();
    auto strategy = TestHelpers::make_test_strategy();

    auto order = make_simple_order();
    Core::StrategyDecision sd;
    ticker->mark_order_to_be_created(Core::Engine::getInstance(), order, sd,
                                      *strategy);
    REQUIRE(ticker->orders.size() == 1);

    auto it = ticker->orders.begin();
    ticker->erase_order_from_state(Core::Engine::getInstance(), it);

    REQUIRE(ticker->orders.empty());
    REQUIRE(ticker->order_count.load() == 0);
    REQUIRE(ticker->active_bid_count == 0);
    REQUIRE(ticker->active_bid_notional == 0.0);
    // Unacked + non-trade-close → unacked_enter_order_count decrements.
    REQUIRE(ticker->unacked_enter_order_count.load() == 0);
}

TEST_CASE(
    "TradeTicker::erase_order_from_state on acked order leaves unacked "
    "counter alone",
    "[ticker][order-lifecycle]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();

    // Manually populate: unacked_enter_order_count=0 but active_bid_count=1
    // (simulates an already-acked order that survived ack without us
    // seeing the whole flow).
    auto order = make_simple_order();
    order.is_order_creation_acknowledged = true;
    ticker->orders.push_back(order);
    ticker->order_count.store(1);
    ticker->active_bid_count = 1;
    ticker->bid_order_count_by_price[order.price.toDouble()] = 1;
    ticker->active_bid_notional = (order.price * order.quantity).toDouble();

    auto it = ticker->orders.begin();
    ticker->erase_order_from_state(Core::Engine::getInstance(), it);

    REQUIRE(ticker->orders.empty());
    REQUIRE(ticker->order_count.load() == 0);
    // Unacked counter stays at 0 — the order was already acked.
    REQUIRE(ticker->unacked_enter_order_count.load() == 0);
}

TEST_CASE(
    "TradeTicker::erase_order_from_state clears matching unacked_new_cancels "
    "entry",
    "[ticker][order-lifecycle]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();

    auto order = make_simple_order();
    ticker->orders.push_back(order);
    ticker->order_count.store(1);
    ticker->active_bid_count = 1;
    ticker->bid_order_count_by_price[order.price.toDouble()] = 1;

    auto cancel_key = Core::TradeTicker::get_order_cancel_key(order);
    ticker->inject_pending_cancel(cancel_key, order,
                                   std::chrono::steady_clock::now());
    REQUIRE(ticker->is_cancel_already_sent(cancel_key));

    auto it = ticker->orders.begin();
    ticker->erase_order_from_state(Core::Engine::getInstance(), it);

    // After erase, the pending-cancel entry should be gone.
    REQUIRE_FALSE(ticker->is_cancel_already_sent(cancel_key));
}

TEST_CASE(
    "TradeTicker::erase_order_from_state on SELL side decrements ask counters",
    "[ticker][order-lifecycle]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();
    auto strategy = TestHelpers::make_test_strategy();

    auto order = make_simple_order();
    order.side = Core::OrderSide::SELL;
    Core::StrategyDecision sd;
    ticker->mark_order_to_be_created(Core::Engine::getInstance(), order, sd,
                                      *strategy);
    REQUIRE(ticker->active_ask_count == 1);

    auto it = ticker->orders.begin();
    ticker->erase_order_from_state(Core::Engine::getInstance(), it);
    REQUIRE(ticker->active_ask_count == 0);
    REQUIRE(ticker->active_ask_notional == 0.0);
    REQUIRE(ticker->ask_order_count_by_price[order.price.toDouble()] == 0);
}

TEST_CASE(
    "TradeTicker::move_trade_to_completed on polymarket path with "
    "unfinalized fills populates recently_completed_polymarket_trades",
    "[ticker][trade-mgmt][polymarket]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();
    ticker->is_trading_on_prediction_market.store(true);

    auto trade = make_active_trade(1, 101);
    // An unfinalized fill is one that's_polymarket_fill_confirmed=false with
    // a taker_order_id. We'll approximate by pushing a fill with these set.
    Core::OrderFillInfo fi;
    fi.taker_order_id = "taker-1";
    fi.is_polymarket_fill_confirmed = false;
    trade.open_fills.push_back(fi);

    ticker->active_trades.push_back(trade);
    ticker->active_trade_count.store(1);

    auto& trade_ref = ticker->active_trades.front();
    ticker->move_trade_to_completed(trade_ref, 1700000099000);

    REQUIRE(ticker->active_trades.empty());
    // Polymarket branch archives into recently_completed_polymarket_trades.
    REQUIRE(ticker->recently_completed_polymarket_trades.size() == 1);
}

TEST_CASE(
    "TradeTicker::purge_stale_removed_orders respects grace window",
    "[ticker][order-lifecycle]") {
    TestHelpers::ensure_minimal_engine();
    auto ticker = TestHelpers::make_test_ticker();

    constexpr long long kGraceMs = 3600LL * 1000;
    constexpr long long kAckTime = 1'700'000'000'000LL;

    auto order = make_simple_order();
    order.ack_state_removal = kAckTime;
    ticker->orders_removed_from_state.push_back(order);

    {
        std::lock_guard<std::mutex> lock(ticker->trading_mutex);
        REQUIRE(ticker->purge_stale_removed_orders(kAckTime + kGraceMs - 1,
                                                   kGraceMs) == 0);
        REQUIRE(ticker->orders_removed_from_state.size() == 1);

        REQUIRE(ticker->purge_stale_removed_orders(kAckTime + kGraceMs,
                                                   kGraceMs) == 1);
        REQUIRE(ticker->orders_removed_from_state.empty());
    }
}
