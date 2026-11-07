#pragma once

#include "httplib.h"

namespace EngineControl::StrategyHandlers {

// Free-function strategy + state-machine handlers. All take the strategy
// id as a query param (?id=N) and resolve via require_strategy() in
// handler_helpers.h — uniform 400 (missing/non-int) / 404 (no such id).

// Strategy reads (always on)
void orders(const httplib::Request& req, httplib::Response& res);
void trades(const httplib::Request& req, httplib::Response& res);
void virtual_balance(const httplib::Request& req, httplib::Response& res);
void recent_orders(const httplib::Request& req, httplib::Response& res);

// SM reads (always on) — operates on strategy.strategy_event_state_machine
void sm_sync_progress(const httplib::Request& req, httplib::Response& res);
void sm_features(const httplib::Request& req, httplib::Response& res);

// SM mutating (gated). Body is MarketData::MarketEvent JSON; supports TRADE
// + BBA variants in Phase C (see dtype_helpers.h::parse_sm_market_event).
// Optional ?is_historical=true.
void sm_react(const httplib::Request& req, httplib::Response& res);

// SM-by-name reads — query the StateMachineDispatcher singleton directly,
// independent of which strategies are loaded. Required for observing SMs
// that strategies WATCH rather than ATTACH as primary. The strategy/sm/* endpoints above only
// see the strategy's primary SM (strategy_event_state_machine pointer).
void sm_list(const httplib::Request& req, httplib::Response& res);
void sm_by_name_sync_progress(const httplib::Request& req,
                               httplib::Response& res);
void sm_by_name_features(const httplib::Request& req,
                          httplib::Response& res);

// SM-by-name mutating (gated). Body is MarketData::MarketEvent JSON
// (same shape as sm_react above). Required for testing SMs that no
// loaded strategy ATTACHES as primary — drives sync state advancement
// of WATCHED SMs without requiring a primary attachment.
// Optional ?is_historical=true.
void sm_by_name_react(const httplib::Request& req, httplib::Response& res);

}  // namespace EngineControl::StrategyHandlers
