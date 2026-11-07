#pragma once

#include "httplib.h"

namespace EngineControl::TriggerHandlers {

// Free-function trigger handlers. The virtual-clock replacement: scenarios
// drive timer-based engine work on demand by POSTing here. All gated by
// ENABLE_ENGINE_CONTROL_MUTATIONS via Server::register_mutating().

void run_periodic_calls(const httplib::Request& req, httplib::Response& res);
void poll_strategy_states(const httplib::Request& req, httplib::Response& res);
void poll_realtime_subscriptions(const httplib::Request& req,
                                 httplib::Response& res);
void process_engine_signals(const httplib::Request& req,
                                    httplib::Response& res);
void generate_predictions(const httplib::Request& req, httplib::Response& res);
void sync_persistent_state_to_disk(const httplib::Request& req,
                                   httplib::Response& res);

// Optional ?force=true query param.
void flush_trading_logs(const httplib::Request& req, httplib::Response& res);

// Optional ?bypass_guard=true query param.
void emit_state_snapshot(const httplib::Request& req, httplib::Response& res);

}  // namespace EngineControl::TriggerHandlers
