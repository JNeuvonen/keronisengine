// Unit tests for Core::Strategy (src/strategy.cpp).
//
// Strategy is a long-lived object owning trade tickers, callbacks
// (strategy_callback, compute_predictions_callback), per-account NAV,
// and rate-limit counters. Construction routes through a giant
// strategy_name dispatch chain that wires the right callbacks; this
// file verifies the dispatch is correct for the three test strategies
// (test_quoter, test_market_maker, test_taker — used by the e2e
// scenario library) and that disabled-with-unknown-name doesn't throw.
//
// Out of scope here: actual strategy callback execution (that's a
// strategy-implementation concern), trade lifecycle (covered by ticker
// tests + e2e scenarios), prediction market routing (depends on
// polymarket-specific Strategy + ticker plumbing).

#include <catch2/catch_test_macros.hpp>

#include "common_types/strategy.h"
#include "market_making/dtypes/trade.h"
#include "market_making/strategy.h"
#include "test_helpers.h"

namespace {

// Strategy ctor is heavy — it reads strategy_name, dispatches to a
// switch chain that looks up callbacks by name. This helper builds a
// StrategyConfig pointed at one of the three test strategies and runs
// the constructor through it.
::StrategyConfig make_db(const std::string& name, int id = 1,
                         bool is_disabled = false,
                         double virtual_balance = 100000.0,
                         bool is_market_making = false) {
    ::StrategyConfig db;
    db.id = id;
    db.strategy_name = name;
    db.is_disabled = is_disabled;
    db.virtual_balance = virtual_balance;
    db.account_mode = "shared";
    db.is_market_making_strategy = is_market_making;
    Core::ExchangeAndMarket em;
    em.exchange_name = "binance";
    em.market_type = "swap";
    em.account_id = 7;
    db.exchanges_list = {em};
    return db;
}

}  // namespace

// ──────────────────────────────────────────────────────────────────────
// Section — dispatch table correctness
// ──────────────────────────────────────────────────────────────────────

TEST_CASE("Strategy ctor with disabled + unknown name does not throw",
          "[strategy][construction]") {
    TestHelpers::ensure_minimal_engine();
    auto db = make_db("never-heard-of-this-strategy", 1, /*is_disabled=*/true);

    REQUIRE_NOTHROW(Core::Strategy{db});
}

TEST_CASE("Strategy ctor with enabled + unknown name throws",
          "[strategy][construction]") {
    TestHelpers::ensure_minimal_engine();
    auto db = make_db("never-heard-of-this-strategy", 1, /*is_disabled=*/false);

    REQUIRE_THROWS_AS(Core::Strategy{db}, std::runtime_error);
}

TEST_CASE("Strategy ctor accepts test_quoter as a known dispatch entry",
          "[strategy][construction][dispatch]") {
    // The dispatch chain in strategy.cpp throws on unknown names when not
    // disabled. Constructing without throwing proves the name routes to
    // a real branch. test_quoter has no SM (per strategies/test_quoter).
    TestHelpers::ensure_minimal_engine();
    auto db = make_db("test_quoter");
    REQUIRE_NOTHROW(Core::Strategy{db});

    Core::Strategy s(db);
    REQUIRE(s.strategy_event_state_machine == nullptr);
    REQUIRE(s.required_bar_types.empty());
    REQUIRE(s.signal_exchanges.empty());
}

TEST_CASE("Strategy ctor accepts test_market_maker as a known dispatch entry",
          "[strategy][construction][dispatch]") {
    TestHelpers::ensure_minimal_engine();
    auto db = make_db("test_market_maker");
    REQUIRE_NOTHROW(Core::Strategy{db});

    Core::Strategy s(db);
    REQUIRE(s.strategy_event_state_machine == nullptr);
}

TEST_CASE("Strategy ctor accepts test_taker as a known dispatch entry",
          "[strategy][construction][dispatch]") {
    TestHelpers::ensure_minimal_engine();
    auto db = make_db("test_taker");
    REQUIRE_NOTHROW(Core::Strategy{db});

    Core::Strategy s(db);
    REQUIRE(s.strategy_event_state_machine == nullptr);
}

// ──────────────────────────────────────────────────────────────────────
// Section — ctor copies state from StrategyConfig
// ──────────────────────────────────────────────────────────────────────

TEST_CASE("Strategy ctor copies flag fields from StrategyConfig",
          "[strategy][construction]") {
    TestHelpers::ensure_minimal_engine();
    auto db = make_db("test_quoter", /*id=*/42);
    db.is_in_reduce_only = true;
    db.is_market_making_strategy = true;
    db.uses_hedges = true;
    db.allow_co_strategy_compounding = true;
    db.needs_to_run_realtime = true;
    Core::Strategy s(db);

    REQUIRE(s.strategy_config.id == 42);
    REQUIRE(s.is_in_reduce_only);
    REQUIRE(s.is_market_making_strategy);
    REQUIRE(s.is_using_hedges);
    REQUIRE(s.needs_to_run_realtime);
    // allow_co_strategy_compounding is private; observable indirectly via
    // get_strategy_trade_size capping behavior — covered separately.
}

TEST_CASE("Strategy ctor seeds virtual_balance atomic from db",
          "[strategy][construction]") {
    TestHelpers::ensure_minimal_engine();
    auto db = make_db("test_quoter", 1, false, /*virtual_balance=*/500000.0);
    Core::Strategy s(db);

    REQUIRE(s.get_virtual_balance() == 500000.0);
}

TEST_CASE(
    "Strategy ctor builds exchange_market_map from exchanges_list "
    "(observed via account-id lookup)",
    "[strategy][construction]") {
    // exchange_market_map is private; the public
    // get_exchange_and_market_type_account_id is the test surface.
    TestHelpers::ensure_minimal_engine();
    auto db = make_db("test_quoter");
    Core::ExchangeAndMarket em2;
    em2.exchange_name = "aster";
    em2.market_type = "swap";
    em2.account_id = 99;
    db.exchanges_list.push_back(em2);
    Core::Strategy s(db);

    auto binance_acct =
        s.get_exchange_and_market_type_account_id("binance", "swap");
    auto aster_acct =
        s.get_exchange_and_market_type_account_id("aster", "swap");
    REQUIRE(binance_acct.has_value());
    REQUIRE(binance_acct.value() == 7);
    REQUIRE(aster_acct.has_value());
    REQUIRE(aster_acct.value() == 99);
}

// ──────────────────────────────────────────────────────────────────────
// Section — Virtual balance math
// ──────────────────────────────────────────────────────────────────────

TEST_CASE("Strategy::update_virtual_balance applies trade pnl",
          "[strategy][virtual-balance]") {
    TestHelpers::ensure_minimal_engine();
    auto db = make_db("test_quoter", 1, false, 100000.0);
    Core::Strategy s(db);

    Core::Trade winner;
    winner.pnl = mpfr::mpreal("250.5");
    s.update_virtual_balance(winner);
    REQUIRE(s.get_virtual_balance() == 100250.5);

    Core::Trade loser;
    loser.pnl = mpfr::mpreal("-100.0");
    s.update_virtual_balance(loser);
    REQUIRE(s.get_virtual_balance() == 100150.5);
}

TEST_CASE("Strategy::update_virtual_balance accumulates across many updates",
          "[strategy][virtual-balance]") {
    TestHelpers::ensure_minimal_engine();
    auto db = make_db("test_quoter", 1, false, 0.0);
    Core::Strategy s(db);

    for (int i = 0; i < 10; ++i) {
        Core::Trade t;
        t.pnl = mpfr::mpreal("1.5");
        s.update_virtual_balance(t);
    }
    REQUIRE(s.get_virtual_balance() == 15.0);
}

// ──────────────────────────────────────────────────────────────────────
// Section — Account-ID lookup
// ──────────────────────────────────────────────────────────────────────

TEST_CASE(
    "Strategy::get_exchange_and_market_type_account_id returns account_id "
    "for known key",
    "[strategy][account]") {
    TestHelpers::ensure_minimal_engine();
    auto db = make_db("test_quoter");
    Core::Strategy s(db);

    auto acct = s.get_exchange_and_market_type_account_id("binance", "swap");
    REQUIRE(acct.has_value());
    REQUIRE(acct.value() == 7);
}

TEST_CASE(
    "Strategy::get_exchange_and_market_type_account_id returns ibkr account "
    "for configured ibkr spot venue",
    "[strategy][account]") {
    TestHelpers::ensure_minimal_engine();
    auto db = make_db("test_quoter");
    db.exchanges_list.clear();

    Core::ExchangeAndMarket ibkr;
    ibkr.exchange_name = "ibkr";
    ibkr.market_type = "spot";
    ibkr.account_id = 314;
    db.exchanges_list.push_back(ibkr);

    Core::Strategy s(db);

    auto acct = s.get_exchange_and_market_type_account_id("ibkr", "spot");
    REQUIRE(acct.has_value());
    REQUIRE(acct.value() == 314);
}

TEST_CASE(
    "Strategy::get_exchange_and_market_type_account_id returns nullopt for "
    "unknown key",
    "[strategy][account]") {
    TestHelpers::ensure_minimal_engine();
    auto db = make_db("test_quoter");
    Core::Strategy s(db);

    REQUIRE_FALSE(s.get_exchange_and_market_type_account_id("bybit", "swap")
                       .has_value());
    REQUIRE_FALSE(s.get_exchange_and_market_type_account_id("binance", "spot")
                       .has_value());
}

// ──────────────────────────────────────────────────────────────────────
// Section — Rate-limit counters (mirrors ticker pattern)
// ──────────────────────────────────────────────────────────────────────

TEST_CASE("Strategy::record_enter_order_sent + 10s window",
          "[strategy][rate-limit]") {
    TestHelpers::ensure_minimal_engine();
    auto db = make_db("test_quoter");
    Core::Strategy s(db);

    REQUIRE(s.get_recent_enter_orders_count_10s() == 0);
    s.record_enter_order_sent();
    s.record_enter_order_sent();
    REQUIRE(s.get_recent_enter_orders_count_10s() == 2);

    // Exit counter is independent.
    REQUIRE(s.get_recent_exit_orders_count_10s() == 0);
    s.record_exit_order_sent();
    REQUIRE(s.get_recent_exit_orders_count_10s() == 1);
    REQUIRE(s.get_recent_enter_orders_count_10s() == 2);
}

TEST_CASE("Strategy::get_recent_enter_orders_count honors arbitrary window",
          "[strategy][rate-limit]") {
    TestHelpers::ensure_minimal_engine();
    auto db = make_db("test_quoter");
    Core::Strategy s(db);

    s.record_enter_order_sent();
    REQUIRE(s.get_recent_enter_orders_count(std::chrono::seconds(60)) == 1);
    REQUIRE(s.get_recent_enter_orders_count(std::chrono::seconds(0)) == 0);
}

// ──────────────────────────────────────────────────────────────────────
// Section — NAV per account
// ──────────────────────────────────────────────────────────────────────

TEST_CASE("Strategy::get_nav_by_account_id returns 0 for unknown account",
          "[strategy][nav]") {
    TestHelpers::ensure_minimal_engine();
    auto db = make_db("test_quoter");
    Core::Strategy s(db);

    REQUIRE(s.get_nav_by_account_id(7) == mpfr::mpreal("0"));
    REQUIRE(s.get_nav_by_account_id(999) == mpfr::mpreal("0"));
}

TEST_CASE("Strategy::update_navs_by_account_id roundtrips per-account NAVs",
          "[strategy][nav]") {
    TestHelpers::ensure_minimal_engine();
    auto db = make_db("test_quoter");
    Core::Strategy s(db);

    std::unordered_map<long long, mpfr::mpreal> navs = {
        {7, mpfr::mpreal("50000")}, {99, mpfr::mpreal("12345.67")}};
    s.update_navs_by_account_id(navs);

    REQUIRE(s.get_nav_by_account_id(7) == mpfr::mpreal("50000"));
    REQUIRE(s.get_nav_by_account_id(99) == mpfr::mpreal("12345.67"));
    REQUIRE(s.get_nav_by_account_id(0) == mpfr::mpreal("0"));
}

TEST_CASE("Strategy::update_navs_by_account_id merges (does not replace)",
          "[strategy][nav]") {
    TestHelpers::ensure_minimal_engine();
    auto db = make_db("test_quoter");
    Core::Strategy s(db);

    s.update_navs_by_account_id({{7, mpfr::mpreal("100")}});
    s.update_navs_by_account_id({{99, mpfr::mpreal("200")}});

    // Both entries persist — second update doesn't clear the first.
    REQUIRE(s.get_nav_by_account_id(7) == mpfr::mpreal("100"));
    REQUIRE(s.get_nav_by_account_id(99) == mpfr::mpreal("200"));
}

// ──────────────────────────────────────────────────────────────────────
// Section — Trade ticker management
// ──────────────────────────────────────────────────────────────────────

TEST_CASE("Strategy fresh ctor has empty trade_tickers + symbols",
          "[strategy][tickers]") {
    TestHelpers::ensure_minimal_engine();
    auto db = make_db("test_quoter");
    Core::Strategy s(db);

    REQUIRE(s.trade_tickers.empty());
    REQUIRE(s.trade_tickers_symbols.empty());
    REQUIRE(s.trade_tickers_symbols_set.empty());
}

TEST_CASE("Strategy::add_trade_ticker populates ticker map and symbols",
          "[strategy][tickers]") {
    TestHelpers::ensure_minimal_engine();
    auto db = make_db("test_quoter");
    Core::Strategy s(db);

    auto ticker_btc = std::shared_ptr<Core::TradeTicker>(
        TestHelpers::make_test_ticker("BTCUSDT").release());
    auto ticker_eth = std::shared_ptr<Core::TradeTicker>(
        TestHelpers::make_test_ticker("ETHUSDT").release());

    s.add_trade_ticker("BTCUSDT", ticker_btc);
    s.add_trade_ticker("ETHUSDT", ticker_eth);

    REQUIRE(s.trade_tickers.size() == 2);
    REQUIRE(s.trade_tickers_symbols.size() == 2);
    REQUIRE(s.trade_tickers_symbols_set.size() == 2);
    REQUIRE(s.trade_tickers_symbols_set.count("BTCUSDT") == 1);
    REQUIRE(s.trade_tickers_symbols_set.count("ETHUSDT") == 1);
}

TEST_CASE("Strategy::remove_trade_ticker drops from map + symbols",
          "[strategy][tickers]") {
    TestHelpers::ensure_minimal_engine();
    auto db = make_db("test_quoter");
    Core::Strategy s(db);

    auto t = std::shared_ptr<Core::TradeTicker>(
        TestHelpers::make_test_ticker("BTCUSDT").release());
    s.add_trade_ticker("BTCUSDT", t);
    REQUIRE(s.trade_tickers_symbols_set.count("BTCUSDT") == 1);

    s.remove_trade_ticker("BTCUSDT");
    REQUIRE(s.trade_tickers.empty());
    REQUIRE(s.trade_tickers_symbols.empty());
    REQUIRE(s.trade_tickers_symbols_set.empty());
}

TEST_CASE(
    "Strategy::get_orders / get_trades return empty when no tickers added",
    "[strategy][tickers]") {
    TestHelpers::ensure_minimal_engine();
    auto db = make_db("test_quoter");
    Core::Strategy s(db);

    REQUIRE(s.get_orders().empty());
    REQUIRE(s.get_trades().empty());
}

// ──────────────────────────────────────────────────────────────────────
// Section — get_strategy_trade_size sizing matrix
// (strategy.cpp:632-662)
//
// Decision tree:
//   1. unknown exchange       → 0
//   2. nav == 0               → 0
//   3. nav < virtual_balance  → nav
//   4. nav > virtual_balance + !co_strategy_compounding  → virtual_balance
//   5. nav > virtual_balance + co_strategy_compounding   → nav
//   6. percentage cap multiplied if is_using_percentage_max_position
//   7. nominal cap min'd in if is_using_nominal_max_position
// ──────────────────────────────────────────────────────────────────────

TEST_CASE(
    "Strategy::get_strategy_trade_size returns 0 for unknown exchange",
    "[strategy][sizing]") {
    TestHelpers::ensure_minimal_engine();
    auto db = make_db("test_quoter");
    Core::Strategy s(db);
    s.update_navs_by_account_id({{7, mpfr::mpreal("50000")}});

    auto& engine = Core::Engine::getInstance();
    REQUIRE(s.get_strategy_trade_size(engine, "bybit", "swap") == 0);
}

TEST_CASE("Strategy::get_strategy_trade_size returns 0 when nav is 0",
          "[strategy][sizing]") {
    TestHelpers::ensure_minimal_engine();
    auto db = make_db("test_quoter");
    Core::Strategy s(db);  // No NAV set → defaults to 0 for account 7.

    auto& engine = Core::Engine::getInstance();
    REQUIRE(s.get_strategy_trade_size(engine, "binance", "swap") == 0);
}

TEST_CASE("Strategy::get_strategy_trade_size returns nav when nav < vb",
          "[strategy][sizing]") {
    TestHelpers::ensure_minimal_engine();
    auto db = make_db("test_quoter", 1, false, /*virtual_balance=*/100000.0);
    Core::Strategy s(db);
    s.update_navs_by_account_id({{7, mpfr::mpreal("30000")}});

    auto& engine = Core::Engine::getInstance();
    // nav (30000) < virtual_balance (100000) → return nav.
    REQUIRE(s.get_strategy_trade_size(engine, "binance", "swap") == 30000.0);
}

TEST_CASE(
    "Strategy::get_strategy_trade_size caps at virtual_balance when nav > vb "
    "without co_strategy_compounding",
    "[strategy][sizing]") {
    TestHelpers::ensure_minimal_engine();
    auto db = make_db("test_quoter", 1, false, /*virtual_balance=*/50000.0);
    db.allow_co_strategy_compounding = false;
    Core::Strategy s(db);
    s.update_navs_by_account_id({{7, mpfr::mpreal("100000")}});

    auto& engine = Core::Engine::getInstance();
    // nav (100000) > virtual_balance (50000) + no compounding → vb wins.
    REQUIRE(s.get_strategy_trade_size(engine, "binance", "swap") == 50000.0);
}

TEST_CASE(
    "Strategy::get_strategy_trade_size returns nav when "
    "co_strategy_compounding allowed",
    "[strategy][sizing]") {
    TestHelpers::ensure_minimal_engine();
    auto db = make_db("test_quoter", 1, false, /*virtual_balance=*/50000.0);
    db.allow_co_strategy_compounding = true;
    Core::Strategy s(db);
    s.update_navs_by_account_id({{7, mpfr::mpreal("100000")}});

    auto& engine = Core::Engine::getInstance();
    // nav > vb + compounding allowed → nav, not vb.
    REQUIRE(s.get_strategy_trade_size(engine, "binance", "swap") == 100000.0);
}

TEST_CASE(
    "Strategy::get_strategy_trade_size applies percentage_max_position",
    "[strategy][sizing]") {
    TestHelpers::ensure_minimal_engine();
    auto db = make_db("test_quoter", 1, false, /*virtual_balance=*/100000.0);
    db.is_using_percentage_max_position = true;
    db.percentage_max_position = 0.25;  // 25% of 50000 = 12500
    Core::Strategy s(db);
    s.update_navs_by_account_id({{7, mpfr::mpreal("50000")}});

    auto& engine = Core::Engine::getInstance();
    REQUIRE(s.get_strategy_trade_size(engine, "binance", "swap") == 12500.0);
}

TEST_CASE(
    "Strategy::get_strategy_trade_size applies nominal_max_position when it "
    "is the smaller cap",
    "[strategy][sizing]") {
    TestHelpers::ensure_minimal_engine();
    auto db = make_db("test_quoter", 1, false, /*virtual_balance=*/100000.0);
    db.is_using_nominal_max_position = true;
    db.nominal_max_position = 8000.0;  // smaller than nav 50000
    Core::Strategy s(db);
    s.update_navs_by_account_id({{7, mpfr::mpreal("50000")}});

    auto& engine = Core::Engine::getInstance();
    REQUIRE(s.get_strategy_trade_size(engine, "binance", "swap") == 8000.0);
}

TEST_CASE(
    "Strategy::get_strategy_trade_size leaves nominal_max_position unused "
    "when computed size is smaller",
    "[strategy][sizing]") {
    TestHelpers::ensure_minimal_engine();
    auto db = make_db("test_quoter", 1, false, /*virtual_balance=*/100000.0);
    db.is_using_nominal_max_position = true;
    db.nominal_max_position = 1000000.0;  // way larger than nav
    Core::Strategy s(db);
    s.update_navs_by_account_id({{7, mpfr::mpreal("50000")}});

    auto& engine = Core::Engine::getInstance();
    // min(50000, 1000000) = 50000.
    REQUIRE(s.get_strategy_trade_size(engine, "binance", "swap") == 50000.0);
}

TEST_CASE(
    "Strategy::get_strategy_trade_size applies percentage then nominal min",
    "[strategy][sizing]") {
    TestHelpers::ensure_minimal_engine();
    auto db = make_db("test_quoter", 1, false, /*virtual_balance=*/100000.0);
    db.is_using_percentage_max_position = true;
    db.percentage_max_position = 0.5;   // 50% of 50000 = 25000
    db.is_using_nominal_max_position = true;
    db.nominal_max_position = 10000.0;  // tighter than the 25000 percentage
    Core::Strategy s(db);
    s.update_navs_by_account_id({{7, mpfr::mpreal("50000")}});

    auto& engine = Core::Engine::getInstance();
    // 50000 * 0.5 = 25000 → min(25000, 10000) = 10000.
    REQUIRE(s.get_strategy_trade_size(engine, "binance", "swap") == 10000.0);
}
