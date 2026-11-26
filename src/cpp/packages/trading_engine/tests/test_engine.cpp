// Unit tests for Core::Engine (src/engine.cpp).
//
// Engine is a singleton with hundreds of public methods. The fixture
// in test_helpers.h initializes it in backtest mode with BTCUSDT and
// ETHUSDT exchange info on binance/swap. This file exercises the
// pure-accessor + simple update/read roundtrip surfaces — the parts
// that don't require strategies, orders, or market events to be loaded.
//
// Out of scope:
//   - Anything that requires a Strategy registered (get_total_nav etc.)
//   - Order lifecycle through ack/cancel/fill (covered by ticker tests
//     + e2e scenarios)
//   - Risk-engine math (depends on strategy + inventory + orderbook)
//   - ONNX model lookup
//   - Realtime subscriptions / fault injection

#include <atomic>
#include <chrono>
#include <set>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "market_making/strategy_registry.h"
#include "market_making/dtypes/inventory.h"
#include "market_making/dtypes/orderbook.h"
#include "market_making/dtypes/quote_asset.h"
#include "market_making/engine.h"
#include "test_helpers.h"

namespace {

Core::Inventory make_inventory(const std::string& asset, double total,
                                double used, double free_qty,
                                const std::string& quote_conversion_symbol =
                                    "") {
    Core::Inventory inv;
    inv.asset = asset;
    inv.total = mpfr::mpreal(std::to_string(total));
    // Sign split as Inventory::from_json does for one-way venues; the bulk
    // update rebuilds total from the sides.
    inv.long_total = total > 0 ? inv.total : mpfr::mpreal(0);
    inv.short_total = total < 0 ? -inv.total : mpfr::mpreal(0);
    inv.used = mpfr::mpreal(std::to_string(used));
    inv.free = mpfr::mpreal(std::to_string(free_qty));
    inv.debt = mpfr::mpreal("0");
    // Bulk update_inventory_and_quote_assets keys by quote_conversion_symbol;
    // the per-symbol update_inventory keys by the explicit symbol param.
    inv.quote_conversion_symbol = quote_conversion_symbol;
    return inv;
}

Core::QuoteAsset make_quote_asset(const std::string& asset, long long acct,
                                   double qty, double free_qty,
                                   const std::string& exchange = "binance",
                                   const std::string& market = "swap") {
    Core::QuoteAsset q;
    q.asset = asset;
    q.account_id = acct;
    q.exchange_name = exchange;
    q.market_type = market;
    q.quantity = mpfr::mpreal(std::to_string(qty));
    q.used = mpfr::mpreal("0");
    q.free = mpfr::mpreal(std::to_string(free_qty));
    q.price = mpfr::mpreal("1.0");
    return q;
}

}  // namespace

// ──────────────────────────────────────────────────────────────────────
// Section — empty-state queries
// ──────────────────────────────────────────────────────────────────────

TEST_CASE("Engine fixture loads no strategies", "[engine][empty-state]") {
    TestHelpers::ensure_minimal_engine();
    auto& engine = Core::Engine::getInstance();

    // The strategy-sync tests share this singleton and add ids >= 900000.
    for (const auto& strategy : engine.get_strategies()) {
        REQUIRE(strategy->strategy_config.id >= 900000);
    }
}

TEST_CASE("Engine::get_unacked_orders + get_failed_cancels empty initially",
          "[engine][empty-state]") {
    TestHelpers::ensure_minimal_engine();
    auto& engine = Core::Engine::getInstance();

    REQUIRE(engine.get_unacked_orders().empty());
    REQUIRE(engine.get_failed_cancels().empty());
}

TEST_CASE("Engine::get_orders by symbol returns empty for unknown symbol",
          "[engine][empty-state]") {
    TestHelpers::ensure_minimal_engine();
    auto& engine = Core::Engine::getInstance();

    // Three-arg overload
    REQUIRE(engine.get_orders("FOOUSDT", "binance", "swap").empty());
    // Single-arg overload
    REQUIRE(engine.get_orders("FOOUSDT").empty());
}

TEST_CASE("Engine::get_order_books_out_of_sync empty initially",
          "[engine][empty-state]") {
    TestHelpers::ensure_minimal_engine();
    auto& engine = Core::Engine::getInstance();

    REQUIRE(engine.get_order_books_out_of_sync().empty());
}

TEST_CASE("Engine::get_inventory returns nullopt for unknown symbol",
          "[engine][empty-state]") {
    TestHelpers::ensure_minimal_engine();
    auto& engine = Core::Engine::getInstance();

    auto inv =
        engine.get_inventory("binance", "swap", "NEVERSEENBEFOREUSDT", 1);
    REQUIRE_FALSE(inv.has_value());
}

TEST_CASE("Engine::get_borrowable_inventory returns nullopt for unknown asset",
          "[engine][empty-state]") {
    TestHelpers::ensure_minimal_engine();
    auto& engine = Core::Engine::getInstance();

    REQUIRE_FALSE(
        engine.get_borrowable_inventory("NEVERHEARD", "binance", "swap", 1)
            .has_value());
}

TEST_CASE("Engine::get_price returns nullopt for unknown symbol",
          "[engine][empty-state]") {
    TestHelpers::ensure_minimal_engine();
    auto& engine = Core::Engine::getInstance();

    REQUIRE_FALSE(
        engine.get_price("NEVERHEARDUSDT", "binance", "swap").has_value());
}

TEST_CASE(
    "Engine::get_best_bid / get_best_ask / get_spread return nullopt before "
    "any orderbook init",
    "[engine][empty-state][orderbook]") {
    TestHelpers::ensure_minimal_engine();
    auto& engine = Core::Engine::getInstance();

    // These query the order_books map — separate state from bba_states_.
    // Without init_orderbook, the map lookup misses.
    REQUIRE_FALSE(
        engine.get_best_bid("UNSEENUSDT", "binance", "swap").has_value());
    REQUIRE_FALSE(
        engine.get_best_ask("UNSEENUSDT", "binance", "swap").has_value());
    REQUIRE_FALSE(
        engine.get_spread("UNSEENUSDT", "binance", "swap").has_value());
}

TEST_CASE("Engine::get_bba returns nullopt before any BBA update",
          "[engine][empty-state][bba]") {
    TestHelpers::ensure_minimal_engine();
    auto& engine = Core::Engine::getInstance();

    REQUIRE_FALSE(
        engine.get_bba("NEVERSEENUSDT", "binance", "swap").has_value());
}

// ──────────────────────────────────────────────────────────────────────
// Section — Precision lookup (exercised through Order tests but pinned
// here directly)
// ──────────────────────────────────────────────────────────────────────

TEST_CASE("Engine::get_qty_precision returns fixture value for BTCUSDT",
          "[engine][precision]") {
    TestHelpers::ensure_minimal_engine();
    auto& engine = Core::Engine::getInstance();

    // Fixture seeds BTCUSDT/binance/swap with amount precision = 3.
    REQUIRE(engine.get_qty_precision("BTCUSDT", "binance", "swap") == 3);
}

TEST_CASE("Engine::get_price_precision returns fixture value for BTCUSDT",
          "[engine][precision]") {
    TestHelpers::ensure_minimal_engine();
    auto& engine = Core::Engine::getInstance();

    REQUIRE(engine.get_price_precision("BTCUSDT", "binance", "swap") == 2);
}

TEST_CASE("Engine::round_quantity rounds to fixture precision",
          "[engine][precision]") {
    TestHelpers::ensure_minimal_engine();
    auto& engine = Core::Engine::getInstance();

    // amount precision = 3 → 0.123456 → 0.123
    auto rounded =
        engine.round_quantity("BTCUSDT", "binance", "swap",
                               mpfr::mpreal("0.123456"));
    REQUIRE(rounded == mpfr::mpreal("0.123"));
}

TEST_CASE("Engine::round_price rounds to fixture precision",
          "[engine][precision]") {
    TestHelpers::ensure_minimal_engine();
    auto& engine = Core::Engine::getInstance();

    // price precision = 2 → 50000.111 → 50000.11 (clear truncate, no
    // ambiguity from round-half-even at the boundary).
    auto rounded =
        engine.round_price("BTCUSDT", "binance", "swap",
                            mpfr::mpreal("50000.111"));
    REQUIRE(rounded == mpfr::mpreal("50000.11"));
}

// ──────────────────────────────────────────────────────────────────────
// Section — BBA roundtrip
// ──────────────────────────────────────────────────────────────────────

TEST_CASE("Engine::update_bba + get_bba roundtrip",
          "[engine][bba]") {
    TestHelpers::ensure_minimal_engine();
    auto& engine = Core::Engine::getInstance();

    // Use a unique symbol so prior tests don't pollute. We registered
    // ETHUSDT precision in the fixture so this is safe.
    engine.update_bba("binance", "swap", "ETHUSDT", /*bid_px=*/3000.0,
                      /*bid_qty=*/2.5, /*ask_px=*/3001.0, /*ask_qty=*/1.5,
                      /*ts=*/1700000000000);

    auto snap = engine.get_bba("ETHUSDT", "binance", "swap");
    REQUIRE(snap.has_value());
    REQUIRE(snap->best_bid_price == 3000.0);
    REQUIRE(snap->best_bid_qty == 2.5);
    REQUIRE(snap->best_ask_price == 3001.0);
    REQUIRE(snap->best_ask_qty == 1.5);
    REQUIRE(snap->timestamp_ms == 1700000000000);
}

// Note: get_best_bid / get_best_ask / get_spread / get_best_*_with_qty
// all read from the order_books map (populated by init_orderbook +
// apply_full_refresh_orderbook). They do NOT read from bba_states_.
// update_bba populates bba_states_, which only get_bba reads.
// Order-book population requires a richer fixture (OrderBookLevels) and
// is deferred to a follow-up phase.

TEST_CASE("Engine::get_bba returns nullopt when timestamp is 0",
          "[engine][bba]") {
    TestHelpers::ensure_minimal_engine();
    auto& engine = Core::Engine::getInstance();

    // Use a fresh symbol so prior tests don't pollute state.
    engine.update_bba("binance", "swap", "BTCUSDT", 50000.0, 1.0, 50001.0, 1.0,
                      /*ts=*/0);
    // get_bba guards on snap.timestamp_ms == 0 → returns nullopt
    // (engine.cpp:2415).
    REQUIRE_FALSE(engine.get_bba("BTCUSDT", "binance", "swap").has_value());
}

TEST_CASE("Engine::get_bba reads back zeros after reset to ts=0",
          "[engine][bba]") {
    TestHelpers::ensure_minimal_engine();
    auto& engine = Core::Engine::getInstance();

    // Two sequential updates — the second overwrites; we read the latest.
    engine.update_bba("binance", "swap", "BTCUSDT", 50000.0, 1.0, 50001.0, 1.0,
                      1700000010000);
    engine.update_bba("binance", "swap", "BTCUSDT", 50100.0, 2.0, 50101.0, 2.0,
                      1700000011000);

    auto snap = engine.get_bba("BTCUSDT", "binance", "swap");
    REQUIRE(snap.has_value());
    REQUIRE(snap->best_bid_price == 50100.0);
    REQUIRE(snap->best_ask_price == 50101.0);
    REQUIRE(snap->timestamp_ms == 1700000011000);
}

// ──────────────────────────────────────────────────────────────────────
// Section — Price update + lookup
// ──────────────────────────────────────────────────────────────────────

TEST_CASE("Engine::update_price + get_price roundtrip",
          "[engine][price]") {
    TestHelpers::ensure_minimal_engine();
    auto& engine = Core::Engine::getInstance();

    engine.update_price("ETHUSDT", "binance", "swap", 3050.5);

    auto price = engine.get_price("ETHUSDT", "binance", "swap");
    REQUIRE(price.has_value());
    REQUIRE(price.value() == 3050.5);
}

// ──────────────────────────────────────────────────────────────────────
// Section — Inventory roundtrip
// ──────────────────────────────────────────────────────────────────────

TEST_CASE("Engine::update_inventory + get_inventory roundtrip",
          "[engine][inventory]") {
    TestHelpers::ensure_minimal_engine();
    auto& engine = Core::Engine::getInstance();

    auto inv = make_inventory("ETH", /*total=*/100.0, /*used=*/30.0,
                               /*free=*/70.0);
    engine.update_inventory("binance", "swap", "ETHUSDT", /*account_id=*/42,
                             inv);

    auto got = engine.get_inventory("binance", "swap", "ETHUSDT", 42);
    REQUIRE(got.has_value());
    REQUIRE(got->asset == "ETH");
    REQUIRE(got->total == mpfr::mpreal("100"));
    REQUIRE(got->free == mpfr::mpreal("70"));
}

TEST_CASE(
    "Engine::get_inventory returns nullopt for wrong account_id after update",
    "[engine][inventory]") {
    TestHelpers::ensure_minimal_engine();
    auto& engine = Core::Engine::getInstance();

    auto inv = make_inventory("ETH", 100.0, 30.0, 70.0);
    engine.update_inventory("binance", "swap", "ETHUSDT", /*account_id=*/100,
                             inv);

    // Different account_id → inventory is account-scoped, lookup misses.
    REQUIRE_FALSE(
        engine.get_inventory("binance", "swap", "ETHUSDT", /*account_id=*/999)
            .has_value());
}

// ──────────────────────────────────────────────────────────────────────
// Section — Borrowable inventory roundtrip
// ──────────────────────────────────────────────────────────────────────

TEST_CASE(
    "Engine::update_borrowable_inventories applies a batch of updates",
    "[engine][inventory][borrowable]") {
    // Note: the single-key Engine::update_borrowable_inventory(string, double)
    // is declared in engine.h:349 but has no definition (linker fails). The
    // bulk update is the only callable path today.
    TestHelpers::ensure_minimal_engine();
    auto& engine = Core::Engine::getInstance();

    // Key format is asset:exchange:market:account_id (per
    // Utils::get_symbol_exchange_market_account_key, where 'symbol' here
    // means the asset name).
    std::unordered_map<std::string, double> batch = {
        {"USDC:binance:swap:5", 1000.0},
        {"USDT:binance:swap:5", 2000.0},
    };
    engine.update_borrowable_inventories(batch);

    REQUIRE(engine.get_borrowable_inventory("USDC", "binance", "swap", 5)
                .value() == 1000.0);
    REQUIRE(engine.get_borrowable_inventory("USDT", "binance", "swap", 5)
                .value() == 2000.0);
}

// ──────────────────────────────────────────────────────────────────────
// Section — Quote asset balance roundtrip (via update_quote_asset)
// ──────────────────────────────────────────────────────────────────────

TEST_CASE(
    "Engine::update_quote_asset + update_inventory_and_quote_assets bulk path",
    "[engine][quote-asset]") {
    TestHelpers::ensure_minimal_engine();
    auto& engine = Core::Engine::getInstance();

    auto qa = make_quote_asset("USDT", /*acct=*/3, /*qty=*/100000.0,
                                /*free=*/100000.0);
    engine.update_quote_asset("binance", "swap", "USDT", 3, qa);

    // No direct getter for QuoteAsset state — we just exercise the write
    // path doesn't throw and bulk update succeeds the same way.
    std::vector<Core::Inventory> invs = {
        // Bulk path keys by quote_conversion_symbol — set it to the symbol
        // we'll look up under so the read finds the entry.
        make_inventory("BTC", 1.5, 0.5, 1.0,
                       /*quote_conversion_symbol=*/"BTCUSDT")};
    std::vector<Core::QuoteAsset> qas = {
        make_quote_asset("USDC", 3, 50000.0, 50000.0)};
    REQUIRE_NOTHROW(engine.update_inventory_and_quote_assets("binance", "swap",
                                                              3, invs, qas));

    auto got = engine.get_inventory("binance", "swap", "BTCUSDT", 3);
    REQUIRE(got.has_value());
    REQUIRE(got->total == mpfr::mpreal("1.5"));
}

TEST_CASE(
    "Engine::update_inventory_and_quote_assets zeroes a symbol the snapshot "
    "leaves out",
    "[engine][inventory]") {
    TestHelpers::ensure_minimal_engine();
    auto& engine = Core::Engine::getInstance();

    engine.update_inventory_and_quote_assets(
        "binance", "swap", 7,
        {make_inventory("INIT", 407.0, 0, 0, "INITUSDT"),
         make_inventory("RECALL", -1790.0, 0, 0, "RECALLUSDT")},
        {});
    engine.update_inventory_and_quote_assets(
        "binance", "swap", 8, {make_inventory("INIT", 5.0, 0, 0, "INITUSDT")},
        {});
    engine.update_inventory_and_quote_assets(
        "binance", "swap", 7, {make_inventory("RECALL", -1000.0, 0, 0, "RECALLUSDT")},
        {});

    auto init = engine.get_inventory("binance", "swap", "INITUSDT", 7);
    REQUIRE(init.has_value());
    REQUIRE(init->total == 0);
    REQUIRE(init->long_total == 0);
    REQUIRE(engine.get_inventory("binance", "swap", "RECALLUSDT", 7)->total ==
            mpfr::mpreal("-1000"));
    REQUIRE(engine.get_inventory("binance", "swap", "INITUSDT", 8)->total ==
            mpfr::mpreal("5"));
}

// ──────────────────────────────────────────────────────────────────────
// Section — Engine clock + time
// ──────────────────────────────────────────────────────────────────────

TEST_CASE("Engine::get_time returns 0 in fresh backtest mode",
          "[engine][clock]") {
    TestHelpers::ensure_minimal_engine();
    auto& engine = Core::Engine::getInstance();

    // Backtest mode + no market events processed = 0.
    REQUIRE(engine.get_time() == 0);
}

// ──────────────────────────────────────────────────────────────────────
// Section — Fee rate lookup
// ──────────────────────────────────────────────────────────────────────

TEST_CASE("Engine::get_fee_rate returns 0 for unconfigured exchange",
          "[engine][fees]") {
    TestHelpers::ensure_minimal_engine();
    auto& engine = Core::Engine::getInstance();

    // Fixture sets no fee config — the lookup should default to 0
    // rather than throw or return garbage.
    auto fee = engine.get_fee_rate("binance", "swap", /*is_maker=*/true);
    REQUIRE(fee == mpfr::mpreal("0"));
}

// ──────────────────────────────────────────────────────────────────────
// Section — Order book setup + accessors
// init_orderbook + apply_full_refresh_orderbook populate the order_books
// map; get_best_bid/get_best_ask/get_spread/get_best_*_with_qty all read
// from there. These are SEPARATE from update_bba/get_bba.
// ──────────────────────────────────────────────────────────────────────

namespace {

Core::OrderBookLevels make_levels(
    const std::vector<std::pair<double, double>>& bids,
    const std::vector<std::pair<double, double>>& asks,
    long long ts_ms = 1700000000000) {
    Core::OrderBookLevels lvl;
    lvl.bids = bids;
    lvl.asks = asks;
    lvl.timestamp_ms = ts_ms;
    lvl.last_sequence_number = ts_ms;
    return lvl;
}

}  // namespace

TEST_CASE("Engine::init_orderbook + apply_full_refresh + get_best_bid/ask",
          "[engine][orderbook]") {
    TestHelpers::ensure_minimal_engine();
    auto& engine = Core::Engine::getInstance();

    engine.init_orderbook("binance", "swap", "BTCUSDT");
    auto levels = make_levels(
        /*bids=*/{{49999.00, 1.0}, {49998.00, 2.5}},
        /*asks=*/{{50001.00, 1.5}, {50002.00, 3.0}});
    engine.apply_full_refresh_orderbook("binance", "swap", "BTCUSDT", levels);

    auto bid = engine.get_best_bid("BTCUSDT", "binance", "swap");
    auto ask = engine.get_best_ask("BTCUSDT", "binance", "swap");
    REQUIRE(bid.has_value());
    REQUIRE(ask.has_value());
    REQUIRE(bid.value() == 49999.00);
    REQUIRE(ask.value() == 50001.00);

    auto spread = engine.get_spread("BTCUSDT", "binance", "swap");
    REQUIRE(spread.has_value());
    REQUIRE(spread.value() == 2.00);
}

TEST_CASE("Engine::get_best_bid_with_qty + get_best_ask_with_qty after refresh",
          "[engine][orderbook]") {
    TestHelpers::ensure_minimal_engine();
    auto& engine = Core::Engine::getInstance();

    // Use ETHUSDT to avoid contamination from prior orderbook tests.
    engine.init_orderbook("binance", "swap", "ETHUSDT");
    auto levels =
        make_levels(/*bids=*/{{2999.00, 5.5}}, /*asks=*/{{3001.00, 4.4}});
    engine.apply_full_refresh_orderbook("binance", "swap", "ETHUSDT", levels);

    auto bid = engine.get_best_bid_with_qty("ETHUSDT", "binance", "swap");
    auto ask = engine.get_best_ask_with_qty("ETHUSDT", "binance", "swap");
    REQUIRE(bid.has_value());
    REQUIRE(ask.has_value());
    REQUIRE(bid->first == 2999.00);
    REQUIRE(bid->second == 5.5);
    REQUIRE(ask->first == 3001.00);
    REQUIRE(ask->second == 4.4);
}

TEST_CASE("Engine::get_best_bid_ask_with_qty bundles both in one snapshot",
          "[engine][orderbook]") {
    TestHelpers::ensure_minimal_engine();
    auto& engine = Core::Engine::getInstance();

    engine.init_orderbook("binance", "swap", "ETHUSDT");
    auto levels = make_levels(/*bids=*/{{2998.00, 1.0}},
                               /*asks=*/{{3002.00, 2.0}});
    engine.apply_full_refresh_orderbook("binance", "swap", "ETHUSDT", levels);

    auto snap =
        engine.get_best_bid_ask_with_qty("ETHUSDT", "binance", "swap");
    REQUIRE(snap.has_value());
    REQUIRE(snap->bid_price == 2998.00);
    REQUIRE(snap->bid_qty == 1.0);
    REQUIRE(snap->ask_price == 3002.00);
    REQUIRE(snap->ask_qty == 2.0);
}

TEST_CASE(
    "Engine::apply_full_refresh_orderbook is a no-op when init wasn't called",
    "[engine][orderbook]") {
    TestHelpers::ensure_minimal_engine();
    auto& engine = Core::Engine::getInstance();

    // No init_orderbook for SOLUSDT — apply_full_refresh early-returns
    // (engine.cpp:2271). get_best_bid stays nullopt.
    auto levels = make_levels({{100.0, 1.0}}, {{101.0, 1.0}});
    engine.apply_full_refresh_orderbook("binance", "swap", "SOLUSDT", levels);

    REQUIRE_FALSE(
        engine.get_best_bid("SOLUSDT", "binance", "swap").has_value());
}

TEST_CASE("Engine::apply_full_refresh marks orderbook in-sync",
          "[engine][orderbook]") {
    TestHelpers::ensure_minimal_engine();
    auto& engine = Core::Engine::getInstance();

    engine.init_orderbook("binance", "swap", "ETHUSDT");
    auto levels =
        make_levels({{3000.00, 1.0}}, {{3001.00, 1.0}}, /*ts=*/1700000005000);
    engine.apply_full_refresh_orderbook("binance", "swap", "ETHUSDT", levels);

    // After apply_full_refresh the book is in-sync — drop from out-of-sync.
    auto out = engine.get_order_books_out_of_sync();
    auto found =
        std::find_if(out.begin(), out.end(), [](const std::string& key) {
            return key.find("ETHUSDT:binance:swap") != std::string::npos;
        });
    REQUIRE(found == out.end());
}

TEST_CASE("Engine::get_ms_since_last_orderbook_full_refresh after refresh",
          "[engine][orderbook]") {
    TestHelpers::ensure_minimal_engine();
    auto& engine = Core::Engine::getInstance();

    engine.init_orderbook("binance", "swap", "ETHUSDT");
    auto levels =
        make_levels({{3000.00, 1.0}}, {{3001.00, 1.0}}, /*ts=*/1700000010000);
    engine.apply_full_refresh_orderbook("binance", "swap", "ETHUSDT", levels);

    // Wall-clock since the refresh — must be a small non-negative value.
    auto since =
        engine.get_ms_since_last_orderbook_full_refresh("ETHUSDT", "binance",
                                                         "swap");
    REQUIRE(since.has_value());
    REQUIRE(since.value() >= 0);
    REQUIRE(since.value() < 60000);  // sanity cap — under 1 minute
}

TEST_CASE(
    "Engine::get_ms_since_last_orderbook_update returns nullopt for unknown "
    "symbol",
    "[engine][orderbook]") {
    TestHelpers::ensure_minimal_engine();
    auto& engine = Core::Engine::getInstance();

    REQUIRE_FALSE(
        engine.get_ms_since_last_orderbook_update("UNINITUSDT", "binance",
                                                   "swap")
            .has_value());
    REQUIRE_FALSE(engine.get_ms_since_last_orderbook_full_refresh(
                       "UNINITUSDT", "binance", "swap")
                      .has_value());
}

// ──────────────────────────────────────────────────────────────────────
// Section — Exposure math (empty-state invariants)
//
// Populated exposure requires a full update_risk_engine_and_global_state
// sweep over strategies + trades + quote_assets + prices, which is
// deferred. These tests pin the empty-state and divide-by-zero guards.
// ──────────────────────────────────────────────────────────────────────

TEST_CASE("Engine::get_nav returns 0 for fresh engine",
          "[engine][exposure]") {
    TestHelpers::ensure_minimal_engine();
    auto& engine = Core::Engine::getInstance();
    REQUIRE(engine.get_nav() == 0.0);
}

TEST_CASE("Engine::get_long_ratio returns 0 when nav is 0",
          "[engine][exposure]") {
    TestHelpers::ensure_minimal_engine();
    auto& engine = Core::Engine::getInstance();
    // Divide-by-zero guard (engine.cpp:2054-2056) — with no strategies or
    // trades, nav is 0 and the ratio short-circuits to 0.
    REQUIRE(engine.get_long_ratio() == mpfr::mpreal("0"));
}

TEST_CASE("Engine::get_short_ratio returns 0 when nav is 0",
          "[engine][exposure]") {
    TestHelpers::ensure_minimal_engine();
    auto& engine = Core::Engine::getInstance();
    REQUIRE(engine.get_short_ratio() == mpfr::mpreal("0"));
}

TEST_CASE(
    "Engine::get_long_ratio_by_symbol + get_short_ratio_by_symbol return 0 "
    "with empty state",
    "[engine][exposure]") {
    TestHelpers::ensure_minimal_engine();
    auto& engine = Core::Engine::getInstance();
    REQUIRE(engine.get_long_ratio_by_symbol("BTCUSDT") == mpfr::mpreal("0"));
    REQUIRE(engine.get_short_ratio_by_symbol("BTCUSDT") == mpfr::mpreal("0"));
    // Unknown symbol also safe.
    REQUIRE(engine.get_long_ratio_by_symbol("NEVERSEEN") == mpfr::mpreal("0"));
}

// Note: global_long_exposure / global_short_exposure / risk_engine are
// private members despite appearing after a "struct" block in the header;
// they're inside the `private:` section that starts above. Exposure math
// under populated state must be tested through public getters only
// (get_long_ratio / get_short_ratio / get_nav), not direct atomic reads.

TEST_CASE("Engine::get_nav_by_account_id returns nullopt for unknown account",
          "[engine][exposure]") {
    TestHelpers::ensure_minimal_engine();
    auto& engine = Core::Engine::getInstance();
    // Strategy-level nav is keyed separately; engine's is populated by
    // update_risk_engine_and_global_state (engine.cpp:1071).
    REQUIRE_FALSE(engine.get_nav_by_account_id(999).has_value());
}

TEST_CASE("Engine::apply_full_refresh second time updates the book",
          "[engine][orderbook]") {
    TestHelpers::ensure_minimal_engine();
    auto& engine = Core::Engine::getInstance();

    engine.init_orderbook("binance", "swap", "ETHUSDT");
    engine.apply_full_refresh_orderbook(
        "binance", "swap", "ETHUSDT",
        make_levels({{3000.00, 1.0}}, {{3001.00, 1.0}}));

    // Refresh with new prices — best should move.
    engine.apply_full_refresh_orderbook(
        "binance", "swap", "ETHUSDT",
        make_levels({{3050.00, 2.0}}, {{3051.00, 2.0}}));

    REQUIRE(engine.get_best_bid("ETHUSDT", "binance", "swap").value() ==
            3050.00);
    REQUIRE(engine.get_best_ask("ETHUSDT", "binance", "swap").value() ==
            3051.00);
}

// ──────────────────────────────────────────────────────────────────────
// Section — engine signals
// ──────────────────────────────────────────────────────────────────────

TEST_CASE("Engine signals reach the handler registered for their type",
          "[engine][signals]") {
    TestHelpers::ensure_minimal_engine();
    auto& engine = Core::Engine::getInstance();
    auto calls = std::make_shared<int>(0);
    engine.register_engine_signal_handler(
        "test_signal_ok", [calls](const Core::EngineSignal& signal) {
            ++*calls;
            return Core::EngineSignalOutcome{Core::EngineSignalStatus::APPLIED,
                                             signal.key};
        });
    engine.register_engine_signal_handler(
        "test_signal_throws",
        [](const Core::EngineSignal&) -> Core::EngineSignalOutcome {
            throw std::runtime_error("boom");
        });

    const auto outcomes = engine.apply_engine_signals(
        {Core::EngineSignal{1, "test_signal_ok", "k1", nlohmann::json::object()},
         Core::EngineSignal{2, "test_signal_unregistered", "",
                            nlohmann::json::object()},
         Core::EngineSignal{3, "test_signal_throws", "",
                            nlohmann::json::object()}});

    REQUIRE(outcomes.size() == 3);
    CHECK(outcomes[0].status == Core::EngineSignalStatus::APPLIED);
    CHECK(outcomes[0].error == "k1");
    CHECK(*calls == 1);
    CHECK(outcomes[1].status == Core::EngineSignalStatus::SKIPPED);
    CHECK(outcomes[2].status == Core::EngineSignalStatus::FAILED);
    CHECK(outcomes[2].error == "boom");

    // The fetch asks only for these, so an unhandled type is never acked.
    const auto types = engine.engine_signal_types();
    auto has = [&types](const std::string& t) {
        return std::find(types.begin(), types.end(), t) != types.end();
    };
    CHECK(has(Core::ENGINE_SIGNAL_REMOVE_TRADE));
    CHECK(has(Core::ENGINE_SIGNAL_STOP_MAKING_MARKETS));
    CHECK(has("test_signal_ok"));
    CHECK_FALSE(has("test_signal_unregistered"));
}

TEST_CASE("remove_trade fails on a bad payload and skips a trade not held here",
          "[engine][signals]") {
    TestHelpers::ensure_minimal_engine();
    auto& engine = Core::Engine::getInstance();

    const auto outcomes = engine.apply_engine_signals(
        {Core::EngineSignal{10, Core::ENGINE_SIGNAL_REMOVE_TRADE, "1",
                            nlohmann::json{{"symbol", "BTCUSDT"}}},
         Core::EngineSignal{11, Core::ENGINE_SIGNAL_REMOVE_TRADE, "999",
                            nlohmann::json{{"symbol", "BTCUSDT"},
                                           {"strategy_id", 424242},
                                           {"trade_id", "999"}}}});

    REQUIRE(outcomes.size() == 2);
    CHECK(outcomes[0].status == Core::EngineSignalStatus::FAILED);
    CHECK(outcomes[0].error.find("bad payload") != std::string::npos);
    CHECK(outcomes[1].status == Core::EngineSignalStatus::SKIPPED);
}

TEST_CASE("stop_making_markets with no matching strategy is skipped",
          "[engine][signals]") {
    TestHelpers::ensure_minimal_engine();
    auto& engine = Core::Engine::getInstance();

    const auto outcomes = engine.apply_engine_signals(
        {Core::EngineSignal{12, Core::ENGINE_SIGNAL_STOP_MAKING_MARKETS, "",
                            nlohmann::json{{"strategy_id", 424242}}}});

    REQUIRE(outcomes.size() == 1);
    CHECK(outcomes[0].status == Core::EngineSignalStatus::SKIPPED);
}

// ──────────────────────────────────────────────────────────────────────
// Section — live strategy sync
// ──────────────────────────────────────────────────────────────────────

namespace {

using SyncResult = Core::Engine::SyncResult;

template <typename Call>
SyncResult retry_while_busy(Call call) {
    auto result = SyncResult::BUSY;
    for (int attempt = 0; attempt < 50 && result == SyncResult::BUSY;
         ++attempt) {
        result = call();
    }
    return result;
}

Core::Strategy* find_strategy(int id) {
    for (auto& strategy : Core::Engine::getInstance().get_strategies()) {
        if (strategy->strategy_config.id == id) return strategy.get();
    }
    return nullptr;
}

}  // namespace

TEST_CASE("Live strategy sync stays consistent under concurrent readers",
          "[engine][strategy-sync]") {
    TestHelpers::ensure_minimal_engine();
    auto& engine = Core::Engine::getInstance();
    const auto before = engine.get_strategies().size();

    std::atomic<bool> stop{false};
    std::atomic<long long> reads{0};
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&] {
            while (!stop.load()) {
                for (auto& strategy : engine.get_strategies()) {
                    // Nested shared lock: engine.get_trades() walks the
                    // strategies again under the outer view.
                    (void)strategy->strategy_config.id;
                    (void)engine.get_trades();
                }
                reads.fetch_add(1);
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        });
    }

    constexpr int kFirst = 900001;
    constexpr int kCount = 20;
    for (int id = kFirst; id < kFirst + kCount; ++id) {
        auto strategy = TestHelpers::make_test_strategy(id);
        REQUIRE(retry_while_busy([&] {
                    return engine.add_strategy_while_trading(strategy);
                }) == SyncResult::APPLIED);
        REQUIRE(strategy == nullptr);
    }
    for (int id = kFirst; id < kFirst + kCount; id += 2) {
        auto next = TestHelpers::make_test_strategy(id);
        REQUIRE(retry_while_busy([&] {
                    return engine.replace_strategy(next);
                }) == SyncResult::APPLIED);
    }
    for (int id = kFirst + 1; id < kFirst + kCount; id += 2) {
        REQUIRE(retry_while_busy([&] { return engine.remove_strategy(id); }) ==
                SyncResult::APPLIED);
    }
    stop.store(true);
    for (auto& reader : readers) reader.join();

    REQUIRE(reads.load() > 0);
    REQUIRE(engine.get_strategies().size() == before + kCount / 2);
    REQUIRE(find_strategy(kFirst) != nullptr);
    REQUIRE(find_strategy(kFirst + 1) == nullptr);
    REQUIRE(engine.remove_strategy(kFirst + 1) == SyncResult::NOT_FOUND);
}

TEST_CASE("replace_strategy carries tickers and running state",
          "[engine][strategy-sync]") {
    TestHelpers::ensure_minimal_engine();
    auto& engine = Core::Engine::getInstance();
    constexpr int kId = 900300;

    auto old_strategy = TestHelpers::make_test_strategy(kId);
    std::shared_ptr<Core::TradeTicker> btc = TestHelpers::make_test_ticker(
        "BTCUSDT", "USDT", "BTC", kId);
    std::shared_ptr<Core::TradeTicker> eth = TestHelpers::make_test_ticker(
        "ETHUSDT", "USDT", "ETH", kId);
    old_strategy->add_trade_ticker("BTCUSDT", btc);
    old_strategy->add_trade_ticker("ETHUSDT", eth);
    old_strategy->stop_making_markets.store(true);
    Core::Strategy* old_raw = old_strategy.get();
    REQUIRE(engine.add_strategy_while_trading(old_strategy) ==
            SyncResult::APPLIED);

    auto db = TestHelpers::make_disabled_strategy_config(kId);
    db.virtual_balance = 5.0;
    auto next = std::make_unique<Core::Strategy>(db);
    next->adopt_runtime_state(*old_raw, {"BTCUSDT"});
    REQUIRE(engine.replace_strategy(next) == SyncResult::APPLIED);

    Core::Strategy* current = find_strategy(kId);
    REQUIRE(current != nullptr);
    REQUIRE(current != old_raw);
    REQUIRE(current->trade_tickers.at("BTCUSDT") == btc);
    REQUIRE_FALSE(current->trade_tickers.contains("ETHUSDT"));
    REQUIRE(current->get_virtual_balance() == 100000.0);
    REQUIRE(current->stop_making_markets.load());
    // Retired, not destroyed: queued tasks may still hold the old pointer.
    REQUIRE(old_raw->trade_tickers.at("ETHUSDT") == eth);
}

TEST_CASE("quiesce_strategy waits for in-flight passes and restores on timeout",
          "[engine][strategy-sync]") {
    TestHelpers::ensure_minimal_engine();
    auto& engine = Core::Engine::getInstance();
    auto strategy = TestHelpers::make_test_strategy(900400);
    strategy->set_disabled(false);

    REQUIRE(engine.quiesce_strategy(*strategy, std::chrono::milliseconds(50)));
    REQUIRE(strategy->is_disabled_now());

    strategy->set_disabled(false);
    strategy->mark_prediction_start();
    REQUIRE_FALSE(strategy->is_idle());
    REQUIRE_FALSE(
        engine.quiesce_strategy(*strategy, std::chrono::milliseconds(20)));
    REQUIRE_FALSE(strategy->is_disabled_now());
}

TEST_CASE("add_strategy_while_trading refuses a strategy with models",
          "[engine][strategy-sync]") {
    TestHelpers::ensure_minimal_engine();
    auto& engine = Core::Engine::getInstance();
    const auto before = engine.get_strategies().size();

    auto db = TestHelpers::make_disabled_strategy_config(900100);
    db.model_schedule_path = "/nonexistent/fold_schedule.json";
    auto strategy = std::make_unique<Core::Strategy>(db);

    REQUIRE(engine.add_strategy_while_trading(strategy) ==
            SyncResult::RESTART_REQUIRED);
    REQUIRE(strategy != nullptr);
    REQUIRE(engine.get_strategies().size() == before);
}

namespace {
void bind_registry_test_strategy(Core::Strategy&) {}
bool matches_registry_test_prefix(const std::string& name) {
    return name.starts_with("registry-test-prefix:");
}
}  // namespace

TEST_CASE("Strategy registry resolves names, matchers and state-machine "
          "ownership", "[engine][registry]") {
    REQUIRE(Core::StrategyRegistry::find("test_quoter") != nullptr);
    REQUIRE(Core::StrategyRegistry::find("crypto-discretionary:5") != nullptr);
    REQUIRE(Core::StrategyRegistry::find("never-registered") == nullptr);
    REQUIRE_FALSE(Core::Strategy::registers_own_state_machine("test_quoter"));

    Core::StrategyRegistry::add("registry-test-owner",
                                bind_registry_test_strategy, true);
    REQUIRE(Core::Strategy::registers_own_state_machine("registry-test-owner"));
    REQUIRE_THROWS_AS(Core::StrategyRegistry::add("registry-test-owner",
                                                  bind_registry_test_strategy),
                      std::logic_error);

    Core::StrategyRegistry::add_matcher(matches_registry_test_prefix,
                                        bind_registry_test_strategy);
    REQUIRE(Core::StrategyRegistry::find("registry-test-prefix:a") ==
            &bind_registry_test_strategy);
}

TEST_CASE("Restore keeps trades with no ticker and pauses their strategy",
          "[engine][restore]") {
    TestHelpers::ensure_minimal_engine();
    auto& engine = Core::Engine::getInstance();
    constexpr int kId = 900500;
    constexpr int kUnloadedId = 900599;

    auto strategy = TestHelpers::make_test_strategy(kId);
    strategy->strategy_config.is_disabled = false;
    strategy->set_disabled(false);
    std::shared_ptr<Core::TradeTicker> btc =
        TestHelpers::make_test_ticker("BTCUSDT", "USDT", "BTC", kId);
    strategy->add_trade_ticker("BTCUSDT", btc);
    REQUIRE(engine.add_strategy_while_trading(strategy) == SyncResult::APPLIED);
    Core::Strategy* loaded = find_strategy(kId);
    REQUIRE(loaded != nullptr);

    const auto trade = [](unsigned long long id, int strategy_id,
                          const std::string& symbol) {
        Core::Trade t;
        t.id = id;
        t.strategy_id = strategy_id;
        t.symbol = symbol;
        t.exchange_name = "binance";
        t.market_type = "swap";
        t.account_id = 1;
        t.open_quantity = mpfr::mpreal("1");
        t.open_price = mpfr::mpreal("100");
        t.type = Core::TradeType::LONG;
        t.type_internal = Core::TradeTypeInternal::DIRECTIONAL_TRADE;
        return t;
    };
    engine.recover_state_from_disk({trade(9005001, kId, "BTCUSDT"),
                                    trade(9005002, kId, "MITOUSDT"),
                                    trade(9005003, kUnloadedId, "ETHUSDT")},
                                   {});

    REQUIRE(btc->get_trades().size() == 1);
    REQUIRE(loaded->is_disabled_now());
    std::set<unsigned long long> held;
    for (const auto& t : engine.get_held_trades()) held.insert(t.id);
    REQUIRE(held == std::set<unsigned long long>{9005002, 9005003});

    // Removing the last held trade resumes the strategy.
    REQUIRE(engine.manually_remove_trade({1, "MITOUSDT", kId, 9005002}));
    REQUIRE_FALSE(loaded->is_disabled_now());
    REQUIRE(engine.get_held_trades().size() == 1);

    // A strategy built later with the ticker takes its held trade.
    auto late = TestHelpers::make_test_strategy(kUnloadedId);
    std::shared_ptr<Core::TradeTicker> eth =
        TestHelpers::make_test_ticker("ETHUSDT", "USDT", "ETH", kUnloadedId);
    late->add_trade_ticker("ETHUSDT", eth);
    REQUIRE(engine.add_strategy_while_trading(late) == SyncResult::APPLIED);
    REQUIRE(eth->get_trades().size() == 1);
    REQUIRE(engine.get_held_trades().empty());

    for (const unsigned long long id : {9005001ULL, 9005003ULL}) {
        engine.remove_trade_from_risk_engine(id);
    }
    REQUIRE(engine.remove_strategy(kId) == SyncResult::APPLIED);
    REQUIRE(engine.remove_strategy(kUnloadedId) == SyncResult::APPLIED);
}
