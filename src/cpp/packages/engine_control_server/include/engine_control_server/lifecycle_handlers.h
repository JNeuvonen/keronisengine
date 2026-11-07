#pragma once

#include "httplib.h"

namespace EngineControl::LifecycleHandlers {

// Engine lifecycle ops. All gated via Server::register_mutating().
//
// Lock-states diagnostic deferred — would require either Engine::
// get_lock_states_snapshot() to wrap try_lock on 16 private mutexes, or
// widening their visibility. Out of scope for a control-server-only phase.

void state_ready(const httplib::Request& req, httplib::Response& res);
void stop_trading(const httplib::Request& req, httplib::Response& res);
void gracefully_stop(const httplib::Request& req, httplib::Response& res);
void reset_engine(const httplib::Request& req, httplib::Response& res);

}  // namespace EngineControl::LifecycleHandlers
