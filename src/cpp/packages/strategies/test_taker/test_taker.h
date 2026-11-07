#pragma once

#include <optional>
#include <vector>

#include "market_data/bar_close_policy.h"
#include "market_making/dtypes/def_only_structs.h"
#include "market_making/dtypes/market_event.h"
#include "market_making/dtypes/strategy_decision.h"
#include "market_making/engine.h"
#include "market_making/strategy.h"

// ``test_taker`` — minimal taker strategy for e2e tests.
//
// Purpose: exercise the MARKET-order construction + submission path.
// test_quoter (limit) and test_market_maker (PostOnly) both stay
// non-marketable; this archetype crosses the book intentionally to
// test the taker-flow paths (immediate fill semantics, taker fees,
// slippage handling, etc.).
//
// Behavior: on each BBA event, if the ticker has no active orders,
// submit one MARKET BUY at TEST_TAKER_ORDER_QTY. The market order
// crosses immediately when the simulator has matching liquidity;
// scenarios that want a fill must seed the simulator's book via
// ``s.simulator.set_book(...)``.
//
// DB config: ``strategy_name = "test_taker"``. Wired in
// ``trading_engine/src/strategy.cpp``.

namespace AlgorithmTestTaker {

std::vector<MarketData::BarClosePolicy> get_strategy_required_bar_types();
std::vector<Core::ExchangeAndMarket> get_signal_exchanges_list();

void compute_predictions(Core::Engine& engine, Core::Strategy& strategy);

std::vector<Core::StrategyDecision> on_market_event_callback(
    Core::Engine& engine, Core::Strategy& strategy,
    std::optional<Core::MarketEvent> market_event);

}  // namespace AlgorithmTestTaker
