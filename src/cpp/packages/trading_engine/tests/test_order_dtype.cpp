// Reference unit tests for Core::Order (src/dtypes/order.cpp).
//
// Scope: the Order dtype's construction, accessor, and serialization
// contract in isolation. The engine fixture is used only because
// Order::to_json() looks up precision via SymbolPrecisionAccess; the
// constructors and accessors are pure.

#include <catch2/catch_test_macros.hpp>

#include "market_making/dtypes/constants.h"
#include "market_making/dtypes/def_only_structs.h"
#include "market_making/dtypes/enums.h"
#include "market_making/dtypes/order.h"
#include "market_making/dtypes/order_res.h"
#include "market_making/dtypes/prediction.h"
#include "market_making/engine.h"
#include "test_helpers.h"

namespace {

// Wrapper around Order::create — populates the non-interesting params
// with sensible defaults so each TEST_CASE can stay focused on the
// fields it exercises.
Core::Order make_test_order(
    const std::string& symbol = "BTCUSDT",
    Core::OrderType type = Core::OrderType::LIMIT,
    Core::OrderSide side = Core::OrderSide::BUY,
    const mpfr::mpreal& price = mpfr::mpreal("50000.00"),
    const mpfr::mpreal& qty = mpfr::mpreal("0.100"),
    const std::string& exchange = "binance",
    const std::string& market = "swap",
    int strategy_id = 1,
    long long account_id = 1,
    bool is_alt_market_routed = false,
    const std::optional<std::string>& alt_symbol = std::nullopt,
    const std::optional<std::string>& alt_base = std::nullopt,
    const std::optional<std::string>& alt_quote = std::nullopt) {
    return Core::Order::create(
        symbol, type, side, price, /*prediction=*/std::nullopt, qty, exchange,
        market, strategy_id, /*expires_on_ms=*/0,
        /*sizing_coeff_delta=*/mpfr::mpreal("0"),
        /*trade_id=*/std::nullopt, /*reduce_only=*/false,
        /*timestamp=*/1700000000000, /*quote_asset=*/"USDT",
        /*base_asset=*/"BTC", /*allowed_to_bypass_risk_engine=*/false,
        /*is_trade_close_order=*/false, Core::OrderTypeInternal::DEFAULT,
        is_alt_market_routed, alt_symbol, alt_base, alt_quote,
        /*margin_auto_borrow=*/false, /*margin_auto_repay=*/false,
        /*derivatives_settle_currency=*/"USDT", /*kv_cache=*/{},
        /*hedge_for_trade_id=*/std::nullopt, account_id,
        /*position_side=*/std::nullopt);
}

Core::Signal make_test_signal(unsigned long long signal_id = 42,
                               const std::string& symbol = "BTCUSDT") {
    Core::Signal sig;
    sig.signal_id = signal_id;
    sig.signal_strength = mpfr::mpreal("0.5");
    sig.prediction = 0.7f;
    sig.price = 50000.0;
    sig.event_time_ms = 1700000000000;
    sig.bar_close_time_ms = 1700000000000;
    sig.strategy_id = 1;
    sig.symbol = symbol;
    return sig;
}

// Lower-level direct call to Order::create. Use when a test needs a
// field that make_test_order() doesn't surface (prediction, trade_id,
// hedge_for_trade_id, position_side, markout_config, kv_cache,
// polymarket fields). Mirrors make_test_order's defaults for everything
// else.
Core::Order create_order_rich(
    const std::optional<Core::Signal>& prediction = std::nullopt,
    std::optional<unsigned long long> trade_id = std::nullopt,
    std::optional<unsigned long long> hedge_for_trade_id = std::nullopt,
    std::optional<Core::PositionSide> position_side = std::nullopt,
    const std::unordered_map<std::string, double>& kv_cache = {},
    bool is_prediction_market_order = false,
    const std::string& clob_id = "", const std::string& outcome_name = "",
    const std::optional<Core::MarkoutConfig>& markout_config = std::nullopt,
    const std::string& exchange = "binance",
    const std::string& market = "swap") {
    return Core::Order::create(
        /*symbol=*/"BTCUSDT", Core::OrderType::LIMIT, Core::OrderSide::BUY,
        /*price=*/mpfr::mpreal("50000.00"), prediction,
        /*quantity=*/mpfr::mpreal("0.100"), exchange, market,
        /*strategy_id=*/1, /*expires_on_ms=*/0,
        /*sizing_coeff_delta=*/mpfr::mpreal("1.0"), trade_id,
        /*reduce_only=*/false, /*timestamp=*/1700000000000,
        /*quote_asset=*/"USDT", /*base_asset=*/"BTC",
        /*allowed_to_bypass_risk_engine=*/false,
        /*is_trade_close_order=*/false, Core::OrderTypeInternal::DEFAULT,
        /*is_alt_market_routed=*/false, std::nullopt, std::nullopt,
        std::nullopt,
        /*margin_auto_borrow=*/false, /*margin_auto_repay=*/false,
        /*derivatives_settle_currency=*/"USDT", kv_cache,
        hedge_for_trade_id, /*account_id=*/1, position_side,
        is_prediction_market_order, clob_id, outcome_name, markout_config);
}

}  // namespace

TEST_CASE("Order::create populates caller fields and assigns engine defaults",
          "[order][create]") {
    TestHelpers::ensure_minimal_engine();

    auto order = make_test_order();

    REQUIRE(order.symbol == "BTCUSDT");
    REQUIRE(order.exchange_name == "binance");
    REQUIRE(order.market_type == "swap");
    REQUIRE(order.type == Core::OrderType::LIMIT);
    REQUIRE(order.side == Core::OrderSide::BUY);
    REQUIRE(order.quote_asset == "USDT");
    REQUIRE(order.base_asset == "BTC");
    REQUIRE(order.strategy_id == 1);
    REQUIRE(order.account_id == 1);

    // Engine-assigned defaults
    REQUIRE(order.status == Core::ORDER_STATUS_WORKING);
    REQUIRE(order.filled_quantity == mpfr::mpreal("0"));
    REQUIRE(order.id != 0);
    REQUIRE(!order.client_order_id.empty());
    REQUIRE(order.signal_id == 0);  // no prediction provided
    REQUIRE(order.type_internal == Core::OrderTypeInternal::DEFAULT);
}

TEST_CASE("Order::create resolves exchange/market enums from strings",
          "[order][create]") {
    TestHelpers::ensure_minimal_engine();

    auto order = make_test_order();

    REQUIRE(order.exchange_enum != Core::Exchange::UNKNOWN);
    REQUIRE(order.market_enum != Core::Market::UNKNOWN);
}

TEST_CASE("Exchange enum maps ibkr string roundtrip", "[order][create][exchange]") {
    REQUIRE(Core::exchange_from_str("ibkr") == Core::Exchange::IBKR);
    REQUIRE(Core::exchange_to_str(Core::Exchange::IBKR) == "ibkr");
}

TEST_CASE(
    "Order::create throws when is_alt_market_routed is true but alt fields "
    "are missing",
    "[order][create][alt-routing]") {
    TestHelpers::ensure_minimal_engine();

    // Missing all three alt fields.
    REQUIRE_THROWS_AS(make_test_order("BTCUSDT", Core::OrderType::LIMIT,
                                       Core::OrderSide::BUY,
                                       mpfr::mpreal("50000"),
                                       mpfr::mpreal("0.1"), "binance", "swap",
                                       1, 1, /*alt_routed=*/true),
                      std::runtime_error);

    // Two of three present — still throws.
    REQUIRE_THROWS_AS(make_test_order("BTCUSDT", Core::OrderType::LIMIT,
                                       Core::OrderSide::BUY,
                                       mpfr::mpreal("50000"),
                                       mpfr::mpreal("0.1"), "binance", "swap",
                                       1, 1, /*alt_routed=*/true,
                                       /*alt_symbol=*/"ETHUSDT",
                                       /*alt_base=*/"ETH",
                                       /*alt_quote=*/std::nullopt),
                      std::runtime_error);
}

TEST_CASE("Order::create with full alt-routing fields does not throw",
          "[order][create][alt-routing]") {
    TestHelpers::ensure_minimal_engine();

    auto order = make_test_order(
        "BTCUSDT", Core::OrderType::LIMIT, Core::OrderSide::BUY,
        mpfr::mpreal("50000"), mpfr::mpreal("0.1"), "binance", "swap", 1, 1,
        /*alt_routed=*/true, /*alt_symbol=*/"ETHUSDT", /*alt_base=*/"ETH",
        /*alt_quote=*/"USDT");

    REQUIRE(order.is_alt_market_routed);
    REQUIRE(order.route_order_to_alt_symbol.has_value());
    REQUIRE(order.route_order_to_alt_symbol.value() == "ETHUSDT");
    REQUIRE(order.routed_order_base_asset.value() == "ETH");
    REQUIRE(order.routed_order_quote_asset.value() == "USDT");
}

TEST_CASE("Order::get_order_api_symbol falls back to symbol when not alt-routed",
          "[order][accessors]") {
    TestHelpers::ensure_minimal_engine();

    auto order = make_test_order();
    REQUIRE(order.get_order_api_symbol() == "BTCUSDT");
}

TEST_CASE(
    "Order::get_order_api_symbol returns routed base+quote concatenated when "
    "alt-routed",
    "[order][accessors][alt-routing]") {
    TestHelpers::ensure_minimal_engine();

    auto order = make_test_order(
        "BTCUSDT", Core::OrderType::LIMIT, Core::OrderSide::BUY,
        mpfr::mpreal("50000"), mpfr::mpreal("0.1"), "binance", "swap", 1, 1,
        /*alt_routed=*/true, /*alt_symbol=*/"ETHUSDT", /*alt_base=*/"ETH",
        /*alt_quote=*/"USDT");

    // Note: get_order_api_symbol returns base+quote, NOT the alt symbol field.
    REQUIRE(order.get_order_api_symbol() == "ETHUSDT");
}

TEST_CASE("Order::get_order_lookup_id uses client_order_id for CEX orders",
          "[order][accessors]") {
    TestHelpers::ensure_minimal_engine();

    auto order = make_test_order();
    REQUIRE(!order.client_order_id.empty());
    REQUIRE(order.exchange_order_id.empty());
    REQUIRE(!order.is_prediction_market_order);

    // CEX path: always client_order_id, even after exchange_order_id is set
    // by ack. The proxy's query-order handler hands the URL `client_order_id`
    // param straight to the CEX as the clientOrderId field; routing the
    // numeric exchange-side orderId through that slot 500s.
    REQUIRE(order.get_order_lookup_id() == order.client_order_id);
    order.exchange_order_id = "EXCH-12345";
    REQUIRE(order.get_order_lookup_id() == order.client_order_id);
}

TEST_CASE(
    "Order::get_order_lookup_id uses exchange_order_id for polymarket",
    "[order][accessors]") {
    TestHelpers::ensure_minimal_engine();

    auto order = make_test_order();
    order.is_prediction_market_order = true;
    order.exchange_order_id = "EXCH-12345";
    REQUIRE(order.get_order_lookup_id() == "EXCH-12345");
}

TEST_CASE("Order::get_key composes exchange:market:symbol", "[order][accessors]") {
    TestHelpers::ensure_minimal_engine();

    auto order = make_test_order();
    REQUIRE(order.get_key() == "binance:swap:BTCUSDT");
}

TEST_CASE("Order::get_backtest_quote_asset remaps binance/swap to USDC",
          "[order][accessors]") {
    TestHelpers::ensure_minimal_engine();

    auto binance_swap = make_test_order();
    REQUIRE(binance_swap.get_backtest_quote_asset() == "USDC");

    auto binance_spot =
        make_test_order("BTCUSDT", Core::OrderType::LIMIT, Core::OrderSide::BUY,
                        mpfr::mpreal("50000"), mpfr::mpreal("0.1"), "binance",
                        "spot");
    REQUIRE(binance_spot.get_backtest_quote_asset() == "USDT");
}

TEST_CASE("Order::to_json -> from_json roundtrip preserves core fields (BUY)",
          "[order][serialization]") {
    TestHelpers::ensure_minimal_engine();

    auto original = make_test_order("BTCUSDT", Core::OrderType::LIMIT,
                                     Core::OrderSide::BUY);
    original.exchange_order_id = "EXCH-999";
    original.status = Core::ORDER_STATUS_NOT_WORKING;

    auto json = original.to_json();
    auto recovered = Core::Order::from_json(json);

    REQUIRE(recovered.id == original.id);
    REQUIRE(recovered.symbol == original.symbol);
    REQUIRE(recovered.type == original.type);
    REQUIRE(recovered.side == original.side);
    REQUIRE(recovered.exchange_name == original.exchange_name);
    REQUIRE(recovered.market_type == original.market_type);
    REQUIRE(recovered.quote_asset == original.quote_asset);
    REQUIRE(recovered.base_asset == original.base_asset);
    REQUIRE(recovered.strategy_id == original.strategy_id);
    REQUIRE(recovered.account_id == original.account_id);
    REQUIRE(recovered.client_order_id == original.client_order_id);
    REQUIRE(recovered.exchange_order_id == original.exchange_order_id);
    REQUIRE(recovered.status == original.status);
    REQUIRE(recovered.type_internal == original.type_internal);
    REQUIRE(recovered.is_alt_market_routed == original.is_alt_market_routed);
}

TEST_CASE("Order::to_json -> from_json roundtrip preserves side on SELL",
          "[order][serialization]") {
    // Symmetric case for the BUY roundtrip above — FEATURE_REVIEW §1.3
    // demands both sides are checked for any path that stores or branches
    // on side. The Order dtype only stores side, but this pin ensures the
    // JSON "BUY"/"SELL" string mapping stays roundtrip-safe.
    TestHelpers::ensure_minimal_engine();

    auto original = make_test_order("BTCUSDT", Core::OrderType::LIMIT,
                                     Core::OrderSide::SELL);
    auto recovered = Core::Order::from_json(original.to_json());

    REQUIRE(recovered.side == Core::OrderSide::SELL);
    REQUIRE(recovered.id == original.id);
}

// ──────────────────────────────────────────────────────────────────────
// Section — Order::create optional field capture
// ──────────────────────────────────────────────────────────────────────

TEST_CASE("Order::create with prediction captures signal_id",
          "[order][create][prediction]") {
    TestHelpers::ensure_minimal_engine();

    auto sig = make_test_signal(/*signal_id=*/777);
    auto order = create_order_rich(/*prediction=*/sig);

    REQUIRE(order.prediction.has_value());
    REQUIRE(order.prediction->signal_id == 777);
    REQUIRE(order.signal_id == 777);  // propagated into top-level field
}

TEST_CASE("Order::create without prediction leaves signal_id at 0",
          "[order][create][prediction]") {
    TestHelpers::ensure_minimal_engine();

    auto order = create_order_rich(/*prediction=*/std::nullopt);

    REQUIRE(!order.prediction.has_value());
    REQUIRE(order.signal_id == 0);
}

TEST_CASE("Order::create with trade_id captures it",
          "[order][create]") {
    TestHelpers::ensure_minimal_engine();

    auto order = create_order_rich(/*prediction=*/std::nullopt,
                                    /*trade_id=*/12345ull);

    REQUIRE(order.trade_id.has_value());
    REQUIRE(order.trade_id.value() == 12345ull);
}

TEST_CASE("Order::create with hedge_for_trade_id captures it",
          "[order][create][hedge]") {
    TestHelpers::ensure_minimal_engine();

    auto order = create_order_rich(std::nullopt, std::nullopt,
                                    /*hedge_for_trade_id=*/999ull);

    REQUIRE(order.hedge_for_trade_id.has_value());
    REQUIRE(order.hedge_for_trade_id.value() == 999ull);
}

TEST_CASE("Order::create with position_side stores LONG",
          "[order][create][position-side]") {
    TestHelpers::ensure_minimal_engine();

    auto order = create_order_rich(std::nullopt, std::nullopt, std::nullopt,
                                    Core::PositionSide::LONG_SIDE);

    REQUIRE(order.position_side.has_value());
    REQUIRE(order.position_side.value() == Core::PositionSide::LONG_SIDE);
}

TEST_CASE("Order::create with position_side stores SHORT",
          "[order][create][position-side]") {
    TestHelpers::ensure_minimal_engine();

    auto order = create_order_rich(std::nullopt, std::nullopt, std::nullopt,
                                    Core::PositionSide::SHORT_SIDE);

    REQUIRE(order.position_side.has_value());
    REQUIRE(order.position_side.value() == Core::PositionSide::SHORT_SIDE);
}

TEST_CASE("Order::create with kv_cache preserves entries",
          "[order][create][kv-cache]") {
    TestHelpers::ensure_minimal_engine();

    std::unordered_map<std::string, double> cache = {
        {"edge_bps", 2.5}, {"vol_1m", 0.015}};
    auto order = create_order_rich(std::nullopt, std::nullopt, std::nullopt,
                                    std::nullopt, cache);

    REQUIRE(order.kv_cache.size() == 2);
    REQUIRE(order.kv_cache.at("edge_bps") == 2.5);
    REQUIRE(order.kv_cache.at("vol_1m") == 0.015);
}

TEST_CASE("Order::create with markout_config captures duration + interval",
          "[order][create][markout]") {
    TestHelpers::ensure_minimal_engine();

    Core::MarkoutConfig cfg;
    cfg.duration_ms = 300000;  // 5 min
    cfg.interval_ms = 5000;    // 5s
    auto order = create_order_rich(std::nullopt, std::nullopt, std::nullopt,
                                    std::nullopt, {}, false, "", "", cfg);

    REQUIRE(order.markout_config.has_value());
    REQUIRE(order.markout_config->duration_ms == 300000);
    REQUIRE(order.markout_config->interval_ms == 5000);
}

TEST_CASE(
    "Order::create with is_prediction_market_order captures clob_id + "
    "outcome_name",
    "[order][create][polymarket]") {
    TestHelpers::ensure_minimal_engine();

    auto order = create_order_rich(std::nullopt, std::nullopt, std::nullopt,
                                    std::nullopt, {},
                                    /*is_prediction_market_order=*/true,
                                    /*clob_id=*/"0xabcdef123",
                                    /*outcome_name=*/"YES");

    REQUIRE(order.is_prediction_market_order);
    REQUIRE(order.clob_id == "0xabcdef123");
    REQUIRE(order.outcome_name == "YES");
}

TEST_CASE("Order::create on gateio uses 26-char client_order_id",
          "[order][create][client-order-id]") {
    TestHelpers::ensure_minimal_engine();

    auto order = make_test_order(
        "BTCUSDT", Core::OrderType::LIMIT, Core::OrderSide::BUY,
        mpfr::mpreal("50000"), mpfr::mpreal("0.1"),
        /*exchange=*/"gateio", /*market=*/"spot");

    // Gateio's CLOID format has a hard 30-char limit, so the code uses
    // a shorter 26-char hash. Everyone else gets a 32-bit hex id.
    REQUIRE(order.client_order_id.length() == 26);
}

TEST_CASE("Order::create on non-gateio exchanges uses 32-bit client_order_id",
          "[order][create][client-order-id]") {
    TestHelpers::ensure_minimal_engine();

    auto order = make_test_order();  // binance default

    // 32-bit hex → 8-char string (hash_to_32bit returns hex-truncated).
    // The exact length is whatever hash_to_32bit emits, but it's NOT 26.
    REQUIRE(order.client_order_id.length() != 26);
    REQUIRE(!order.client_order_id.empty());
}

// ──────────────────────────────────────────────────────────────────────
// Section — Alt-routing accessors (beyond get_order_api_symbol)
// ──────────────────────────────────────────────────────────────────────

TEST_CASE(
    "Order::get_potentially_alt_routed_symbol returns original when no alt",
    "[order][accessors][alt-routing]") {
    TestHelpers::ensure_minimal_engine();

    auto order = make_test_order();
    REQUIRE(order.get_potentially_alt_routed_symbol() == "BTCUSDT");
}

TEST_CASE(
    "Order::get_potentially_alt_routed_symbol returns alt_symbol when set",
    "[order][accessors][alt-routing]") {
    TestHelpers::ensure_minimal_engine();

    auto order = make_test_order(
        "BTCUSDT", Core::OrderType::LIMIT, Core::OrderSide::BUY,
        mpfr::mpreal("50000"), mpfr::mpreal("0.1"), "binance", "swap", 1, 1,
        /*alt_routed=*/true, /*alt_symbol=*/"ETHUSDT", /*alt_base=*/"ETH",
        /*alt_quote=*/"USDT");
    REQUIRE(order.get_potentially_alt_routed_symbol() == "ETHUSDT");
}

TEST_CASE("Order::get_potentially_alt_routed base/quote follow same pattern",
          "[order][accessors][alt-routing]") {
    TestHelpers::ensure_minimal_engine();

    auto no_alt = make_test_order();
    REQUIRE(no_alt.get_potentially_alt_routed_base_asset() == "BTC");
    REQUIRE(no_alt.get_potentially_alt_routed_quote_asset() == "USDT");

    auto alt = make_test_order(
        "BTCUSDT", Core::OrderType::LIMIT, Core::OrderSide::BUY,
        mpfr::mpreal("50000"), mpfr::mpreal("0.1"), "binance", "swap", 1, 1,
        /*alt_routed=*/true, /*alt_symbol=*/"SOLUSDT", /*alt_base=*/"SOL",
        /*alt_quote=*/"USDT");
    REQUIRE(alt.get_potentially_alt_routed_base_asset() == "SOL");
    REQUIRE(alt.get_potentially_alt_routed_quote_asset() == "USDT");
}

// ──────────────────────────────────────────────────────────────────────
// Section — Roundtrip (to_json → from_json) preserves optional fields
// ──────────────────────────────────────────────────────────────────────

TEST_CASE("Order roundtrip preserves prediction + signal_id",
          "[order][serialization][prediction]") {
    TestHelpers::ensure_minimal_engine();

    auto sig = make_test_signal(/*signal_id=*/888);
    auto original = create_order_rich(sig);

    auto recovered = Core::Order::from_json(original.to_json());

    REQUIRE(recovered.signal_id == 888);
    REQUIRE(recovered.prediction.has_value());
    REQUIRE(recovered.prediction->signal_id == 888);
}

TEST_CASE("Order roundtrip preserves position_side",
          "[order][serialization][position-side]") {
    TestHelpers::ensure_minimal_engine();

    auto long_order = create_order_rich(std::nullopt, std::nullopt,
                                         std::nullopt,
                                         Core::PositionSide::LONG_SIDE);
    auto r_long = Core::Order::from_json(long_order.to_json());
    REQUIRE(r_long.position_side.has_value());
    REQUIRE(r_long.position_side.value() == Core::PositionSide::LONG_SIDE);

    auto short_order = create_order_rich(std::nullopt, std::nullopt,
                                          std::nullopt,
                                          Core::PositionSide::SHORT_SIDE);
    auto r_short = Core::Order::from_json(short_order.to_json());
    REQUIRE(r_short.position_side.has_value());
    REQUIRE(r_short.position_side.value() == Core::PositionSide::SHORT_SIDE);
}

TEST_CASE("Order roundtrip preserves markout_config",
          "[order][serialization][markout]") {
    TestHelpers::ensure_minimal_engine();

    Core::MarkoutConfig cfg;
    cfg.duration_ms = 600000;
    cfg.interval_ms = 2000;
    auto original = create_order_rich(std::nullopt, std::nullopt,
                                       std::nullopt, std::nullopt, {}, false,
                                       "", "", cfg);
    auto recovered = Core::Order::from_json(original.to_json());

    REQUIRE(recovered.markout_config.has_value());
    REQUIRE(recovered.markout_config->duration_ms == 600000);
    REQUIRE(recovered.markout_config->interval_ms == 2000);
}

TEST_CASE("Order roundtrip preserves hedge_for_trade_id",
          "[order][serialization][hedge]") {
    TestHelpers::ensure_minimal_engine();

    auto original = create_order_rich(std::nullopt, std::nullopt, 321ull);
    auto recovered = Core::Order::from_json(original.to_json());

    REQUIRE(recovered.hedge_for_trade_id.has_value());
    REQUIRE(recovered.hedge_for_trade_id.value() == 321ull);
}

TEST_CASE("Order roundtrip preserves kv_cache entries",
          "[order][serialization][kv-cache]") {
    TestHelpers::ensure_minimal_engine();

    std::unordered_map<std::string, double> cache = {
        {"theo_mid", 50123.5}, {"pred_bps", -1.2}};
    auto original = create_order_rich(std::nullopt, std::nullopt, std::nullopt,
                                       std::nullopt, cache);
    auto recovered = Core::Order::from_json(original.to_json());

    REQUIRE(recovered.kv_cache.size() == 2);
    REQUIRE(recovered.kv_cache.at("theo_mid") == 50123.5);
    REQUIRE(recovered.kv_cache.at("pred_bps") == -1.2);
}

TEST_CASE("Order roundtrip preserves alt-routing fields",
          "[order][serialization][alt-routing]") {
    TestHelpers::ensure_minimal_engine();

    auto original = make_test_order(
        "BTCUSDT", Core::OrderType::LIMIT, Core::OrderSide::BUY,
        mpfr::mpreal("50000"), mpfr::mpreal("0.1"), "binance", "swap", 1, 1,
        /*alt_routed=*/true, /*alt_symbol=*/"ETHUSDT", /*alt_base=*/"ETH",
        /*alt_quote=*/"USDT");
    auto recovered = Core::Order::from_json(original.to_json());

    REQUIRE(recovered.is_alt_market_routed);
    REQUIRE(recovered.route_order_to_alt_symbol.has_value());
    REQUIRE(recovered.route_order_to_alt_symbol.value() == "ETHUSDT");
    REQUIRE(recovered.routed_order_base_asset.value() == "ETH");
    REQUIRE(recovered.routed_order_quote_asset.value() == "USDT");
}

TEST_CASE("Order roundtrip preserves polymarket fields",
          "[order][serialization][polymarket]") {
    TestHelpers::ensure_minimal_engine();

    auto original = create_order_rich(std::nullopt, std::nullopt, std::nullopt,
                                       std::nullopt, {},
                                       /*is_prediction_market_order=*/true,
                                       /*clob_id=*/"0xdeadbeef",
                                       /*outcome_name=*/"NO");
    auto recovered = Core::Order::from_json(original.to_json());

    REQUIRE(recovered.is_prediction_market_order);
    REQUIRE(recovered.clob_id == "0xdeadbeef");
    REQUIRE(recovered.outcome_name == "NO");
}

// ──────────────────────────────────────────────────────────────────────
// Section — to_ccxt_proxy_format — wire-format formatting for the proxy
// ──────────────────────────────────────────────────────────────────────

TEST_CASE("Order::to_ccxt_proxy_format lowercases type and side",
          "[order][serialization][ccxt]") {
    TestHelpers::ensure_minimal_engine();

    auto buy_limit = make_test_order("BTCUSDT", Core::OrderType::LIMIT,
                                      Core::OrderSide::BUY);
    auto bj = buy_limit.to_ccxt_proxy_format();
    REQUIRE(bj["type"] == "limit");
    REQUIRE(bj["side"] == "buy");

    auto sell_market = make_test_order(
        "BTCUSDT", Core::OrderType::MARKET, Core::OrderSide::SELL);
    auto sj = sell_market.to_ccxt_proxy_format();
    REQUIRE(sj["type"] == "market");
    REQUIRE(sj["side"] == "sell");
}

TEST_CASE(
    "Order::to_ccxt_proxy_format formats swap symbol with :settle suffix",
    "[order][serialization][ccxt]") {
    TestHelpers::ensure_minimal_engine();

    auto order = make_test_order();  // binance/swap/BTCUSDT default
    auto j = order.to_ccxt_proxy_format();
    REQUIRE(j["symbol"] == "BTC/USDT:USDT");
}

TEST_CASE(
    "Order::to_ccxt_proxy_format formats spot symbol without settle suffix",
    "[order][serialization][ccxt]") {
    TestHelpers::ensure_minimal_engine();

    auto order = make_test_order(
        "BTCUSDT", Core::OrderType::LIMIT, Core::OrderSide::BUY,
        mpfr::mpreal("50000"), mpfr::mpreal("0.1"), "binance", "spot");
    auto j = order.to_ccxt_proxy_format();
    REQUIRE(j["symbol"] == "BTC/USDT");
}

TEST_CASE("Order::to_ccxt_proxy_format routes alt symbol when alt-routed",
          "[order][serialization][ccxt][alt-routing]") {
    TestHelpers::ensure_minimal_engine();

    auto order = make_test_order(
        "BTCUSDT", Core::OrderType::LIMIT, Core::OrderSide::BUY,
        mpfr::mpreal("50000"), mpfr::mpreal("0.1"), "binance", "swap", 1, 1,
        /*alt_routed=*/true, /*alt_symbol=*/"ETHUSDT", /*alt_base=*/"ETH",
        /*alt_quote=*/"USDT");
    auto j = order.to_ccxt_proxy_format();

    REQUIRE(j.contains("route_order_to_alt_symbol"));
    REQUIRE(j["route_order_to_alt_symbol"] == "ETH/USDT:USDT");
}

// ──────────────────────────────────────────────────────────────────────
// Section — from_order_res_normalized — fill extraction from wire
// ──────────────────────────────────────────────────────────────────────

TEST_CASE("Order::from_order_res_normalized extracts full fill",
          "[order][fill-extraction]") {
    TestHelpers::ensure_minimal_engine();

    auto order = make_test_order();
    order.sizing_coeff_delta = mpfr::mpreal("1.0");

    Core::OrderResNormalized res;
    res.symbol = "BTCUSDT";
    res.client_order_id = order.client_order_id;
    res.filled_quantity = mpfr::mpreal("0.100");
    res.price = mpfr::mpreal("50000.00");
    res.last_trade_timestamp = 1700000001000;
    res.is_maker = true;

    auto fill = order.from_order_res_normalized(
        Core::Engine::getInstance(), res);

    REQUIRE(fill.order_id == order.id);
    REQUIRE(fill.side == order.side);
    REQUIRE(fill.fill_amount == mpfr::mpreal("0.100"));
    REQUIRE(fill.price == mpfr::mpreal("50000.00"));
    REQUIRE(fill.order_fill_timestamp == 1700000001000);
    REQUIRE(fill.is_maker);
    // sizing_coeff_delta math: (fill_delta / qty) * sizing_coeff_delta
    //                        = (0.1 / 0.1) * 1.0 = 1.0 for a full fresh fill.
    REQUIRE(fill.sizing_coeff_delta == mpfr::mpreal("1.0"));
}

TEST_CASE(
    "Order::from_order_res_normalized handles partial fill delta correctly",
    "[order][fill-extraction]") {
    TestHelpers::ensure_minimal_engine();

    auto order = make_test_order();  // quantity = 0.1
    order.sizing_coeff_delta = mpfr::mpreal("2.0");
    order.filled_quantity = mpfr::mpreal("0.030");  // 30% already filled

    Core::OrderResNormalized res;
    res.symbol = "BTCUSDT";
    res.filled_quantity = mpfr::mpreal("0.050");  // now 50% cumulative
    res.price = mpfr::mpreal("50000.00");
    res.last_trade_timestamp = 1700000002000;
    res.is_maker = false;

    auto fill = order.from_order_res_normalized(
        Core::Engine::getInstance(), res);

    // fill_amount is the CUMULATIVE filled_qty from the res, not the delta.
    REQUIRE(fill.fill_amount == mpfr::mpreal("0.050"));
    // sizing_coeff_delta reflects the INCREMENTAL delta:
    // ((0.05 - 0.03) / 0.1) * 2.0 = 0.4
    REQUIRE(fill.sizing_coeff_delta == mpfr::mpreal("0.4"));
    REQUIRE_FALSE(fill.is_maker);
}

TEST_CASE(
    "Order::from_order_res_normalized detects fee paid in base asset",
    "[order][fill-extraction][fee]") {
    TestHelpers::ensure_minimal_engine();

    auto order = make_test_order();  // base_asset = "BTC"

    Core::OrderResNormalized res;
    res.symbol = "BTCUSDT";
    res.filled_quantity = mpfr::mpreal("0.100");
    res.price = mpfr::mpreal("50000.00");
    res.last_trade_timestamp = 1700000003000;

    // Fee paid in base asset — case-insensitive match per order.cpp:486-489.
    Core::OrderResFees fee;
    fee.currency = "btc";  // lowercase — matches after to_lowercase
    fee.amount = mpfr::mpreal("0.00005");
    res.fee = fee;

    auto fill = order.from_order_res_normalized(
        Core::Engine::getInstance(), res);

    REQUIRE(fill.fee.has_value());
    REQUIRE(fill.is_fee_paid_in_base_asset);

    // Fee in quote asset should NOT set the base-asset flag.
    Core::OrderResFees quote_fee;
    quote_fee.currency = "USDT";
    quote_fee.amount = mpfr::mpreal("0.5");
    res.fee = quote_fee;

    auto fill2 = order.from_order_res_normalized(
        Core::Engine::getInstance(), res);
    REQUIRE_FALSE(fill2.is_fee_paid_in_base_asset);
}
