#pragma once

#include <optional>
#include <vector>

#include "market_data/bar_close_policy.h"
#include "market_making/dtypes/def_only_structs.h"
#include "market_making/dtypes/market_event.h"
#include "market_making/dtypes/strategy_decision.h"
#include "market_making/engine.h"
#include "market_making/strategy.h"

// ``test_market_maker`` — minimal two-sided MM strategy for e2e tests.
//
// Purpose: exercise the order-lifecycle code path that handles BOTH
// sides of a quote (BUY at best_bid - tick AND SELL at best_ask +
// tick), as PostOnly limits. Catches MM-specific bugs that a
// one-sided quoter (test_quoter) misses — e.g., orderbook updates
// triggering re-quotes on only one side, exposure calculations that
// only fire on entries, etc.
//
// Behavior: on BBA event, if the ticker has no active orders, submit
// one BUY + one SELL one tick beyond the inside. PostOnly so neither
// crosses — guaranteed to rest. Single-pair per empty-ticker tick (no
// re-quoting).
//
// DB config: strategy row with ``strategy_name = "test_market_maker"``.
// Wired in ``trading_engine/src/strategy.cpp``.

namespace AlgorithmTestMarketMaker {

std::vector<MarketData::BarClosePolicy> get_strategy_required_bar_types();
std::vector<Core::ExchangeAndMarket> get_signal_exchanges_list();

void compute_predictions(Core::Engine& engine, Core::Strategy& strategy);

std::vector<Core::StrategyDecision> on_market_event_callback(
    Core::Engine& engine, Core::Strategy& strategy,
    std::optional<Core::MarketEvent> market_event);

}  // namespace AlgorithmTestMarketMaker
