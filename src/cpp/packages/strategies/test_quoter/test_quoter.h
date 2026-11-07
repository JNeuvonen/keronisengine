#pragma once

#include <optional>
#include <vector>

#include "market_data/bar_close_policy.h"
#include "market_making/dtypes/def_only_structs.h"
#include "market_making/dtypes/market_event.h"
#include "market_making/dtypes/strategy_decision.h"
#include "market_making/engine.h"
#include "market_making/strategy.h"

// ``test_quoter`` — minimal strategy for e2e test scenarios.
//
// Purpose: exercise the full order-lifecycle round trip (engine → proxy →
// simulator → fill → WS → engine → DB) without any ML model, historical
// data, or state machine dependency. Used by ``infra_e2e_testing`` scenarios
// that need a strategy to drive order flow, not to produce alpha.
//
// Behavior: on receiving a BBA event for a configured symbol, if the ticker
// has no active orders, submit a resting limit BUY one tick below the
// current best bid at ``TEST_QUOTER_ORDER_QTY``. That's it — no cancellation
// logic, no re-quoting, no exit logic. A single order per empty-ticker tick.
//
// DB config: strategy row with ``strategy_name = "test_quoter"``. Wired
// into ``Core::Strategy`` in ``trading_engine/src/strategy.cpp``.

namespace AlgorithmTestQuoter {

std::vector<MarketData::BarClosePolicy> get_strategy_required_bar_types();
std::vector<Core::ExchangeAndMarket> get_signal_exchanges_list();

void compute_predictions(Core::Engine& engine, Core::Strategy& strategy);

std::vector<Core::StrategyDecision> on_market_event_callback(
    Core::Engine& engine, Core::Strategy& strategy,
    std::optional<Core::MarketEvent> market_event);

}  // namespace AlgorithmTestQuoter
