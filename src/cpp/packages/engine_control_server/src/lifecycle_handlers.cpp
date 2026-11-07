#include "engine_control_server/lifecycle_handlers.h"

#include <nlohmann/json.hpp>

#include "engine_control_server/handler_helpers.h"
#include "market_making/engine.h"

namespace EngineControl::LifecycleHandlers {

void state_ready(const httplib::Request&, httplib::Response& res) {
    Core::Engine::getInstance().state_is_ready();
    set_json(res, 200, {{"ok", true}});
}

void stop_trading(const httplib::Request&, httplib::Response& res) {
    Core::Engine::getInstance().stop_trading();
    set_json(res, 200, {{"ok", true}});
}

void gracefully_stop(const httplib::Request&, httplib::Response& res) {
    // Sets engine.running=false; engine event loop exits at next tick.
    // Tests must NOT call this — would kill subsequent test cases sharing
    // the engine singleton.
    Core::Engine::getInstance().gracefully_stop();
    set_json(res, 200, {{"ok", true}});
}

void reset_engine(const httplib::Request&, httplib::Response& res) {
    // Test-harness hook: clears transient engine state between tests
    // without respawning the binary. See Core::Engine::reset in
    // engine.cpp for the exact contract of what's cleared vs left.
    Core::Engine::getInstance().reset();
    set_json(res, 200, {{"ok", true}});
}

}  // namespace EngineControl::LifecycleHandlers
