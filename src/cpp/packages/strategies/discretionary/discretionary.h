#pragma once

#include "market_data/bar_close_policy.h"
#include "market_making/dtypes/market_event.h"
#include "market_making/dtypes/strategy_decision.h"
#include "market_making/engine.h"
#include "market_making/strategy.h"

namespace AlgorithmDiscretionary {

std::vector<MarketData::BarClosePolicy> get_strategy_required_bar_types();
void compute_predictions(Core::Engine& engine, Core::Strategy& strategy);
std::vector<Core::StrategyDecision> on_market_event_callback(
    Core::Engine& engine, Core::Strategy& strategy,
    std::optional<Core::MarketEvent> market_event);

}  // namespace AlgorithmDiscretionary
