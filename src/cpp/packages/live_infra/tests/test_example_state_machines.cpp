#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "historical_feature_gen/transformations/spread_sm.h"
#include "historical_feature_gen/transformations/windowed_taker_flow_sm.h"
#include "market_data/state_machine_registry.h"
#include "market_making/strategy_registry.h"

namespace {

MarketData::MarketEvent make_bba(const std::string& sym, double bid, double ask,
                                 long long ts_ms) {
    MarketData::MarketEvent e;
    e.type = MarketData::MarketEvent::BBA;
    e.bba = MarketData::BBAEvent{
        .symbol = sym,
        .exchange_name = "binance",
        .market_type = "swap",
        .bid_price = bid,
        .bid_qty = 1.0,
        .ask_price = ask,
        .ask_qty = 1.0,
        .timestamp_ms = ts_ms,
    };
    return e;
}

MarketData::MarketEvent make_trade(const std::string& sym, double price,
                                   double qty, bool buyer_maker,
                                   long long ts_ms) {
    MarketData::MarketEvent e;
    e.type = MarketData::MarketEvent::TRADE;
    e.trade = MarketData::TradeEvent{
        .symbol = sym,
        .exchange_name = "binance",
        .market_type = "swap",
        .price = price,
        .qty = qty,
        .is_buyer_maker = buyer_maker,
        .timestamp_ms = ts_ms,
    };
    return e;
}

}  // namespace

TEST_CASE("SpreadSM waits for valid BBA and emits deterministic spreads",
          "[state_machines]") {
    auto sm = MarketData::StateMachineRegistry::create("spread");
    sm->configure({{"symbol_keys", {"BTCUSDT:binance:swap"}}});
    sm->prepare_historical();

    // No BBA yet -> must not emit synthetic 0 spread.
    REQUIRE_FALSE(sm->emit_state().has_value());

    sm->react(make_bba("BTCUSDT", 100.0, 100.5, 1'000'000), true);
    auto out = sm->emit_state();
    REQUIRE(out.has_value());
    REQUIRE(out->size() == 1);
    REQUIRE(out->at(0) == Catch::Approx(0.5f));
    REQUIRE(sm->feature_names() ==
            std::vector<std::string>{"BTCUSDT:binance:swap:spread"});
}

TEST_CASE("WindowedTakerFlowSM tracks rolling buy/sell notional and expires old buckets",
          "[state_machines]") {
    auto sm = MarketData::StateMachineRegistry::create("windowed_taker_flow");
    sm->configure({{"symbol_keys", {"BTCUSDT:binance:swap"}},
                   {"windows_sec", {5}}});
    sm->prepare_historical();

    // Taker buy (is_buyer_maker=false) of $1000 at t=10s
    sm->react(make_trade("BTCUSDT", 100.0, 10.0, false, 10'000), true);
    // Taker sell (is_buyer_maker=true) of $500 at t=12s
    sm->react(make_trade("BTCUSDT", 100.0, 5.0, true, 12'000), true);

    auto out1 = sm->emit_state();
    REQUIRE(out1.has_value());
    REQUIRE(out1->size() == sm->feature_names().size());
    REQUIRE(out1->size() == 4);
    REQUIRE(out1->at(0) == Catch::Approx(1000.0f / 1500.0f));
    REQUIRE(out1->at(1) == Catch::Approx(2.0f));
    REQUIRE(out1->at(2) == Catch::Approx(1000.0f));
    REQUIRE(out1->at(3) == Catch::Approx(500.0f));

    // Advance past the 5s window (t=20s) with a single $200 buy: old trades expire
    sm->react(make_trade("BTCUSDT", 100.0, 2.0, false, 20'000), true);
    auto out2 = sm->emit_state();
    REQUIRE(out2.has_value());
    // Window now contains only the $200 buy (buy_ratio = 1.0, count = 1, buy_vol = 200, sell_vol = 0)
    REQUIRE(out2->at(0) == Catch::Approx(1.0f));
    REQUIRE(out2->at(1) == Catch::Approx(1.0f));
    REQUIRE(out2->at(2) == Catch::Approx(200.0f));
    REQUIRE(out2->at(3) == Catch::Approx(0.0f));
}

TEST_CASE("Flagship example strategies are registered", "[state_machines]") {
    REQUIRE(Core::StrategyRegistry::find("polymarket_digital") != nullptr);
    REQUIRE(Core::StrategyRegistry::find("binance_onnx_perp") != nullptr);
    REQUIRE(Core::StrategyRegistry::find("cross_venue_arb") != nullptr);
}
