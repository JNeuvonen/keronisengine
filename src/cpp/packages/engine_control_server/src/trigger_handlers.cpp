#include "engine_control_server/trigger_handlers.h"

#include <nlohmann/json.hpp>

#include "engine_control_server/handler_helpers.h"
#include "market_making/engine.h"

namespace EngineControl::TriggerHandlers {

void run_periodic_calls(const httplib::Request&, httplib::Response& res) {
    Core::Engine::getInstance().run_periodic_calls();
    set_json(res, 200, {{"ok", true}});
}

void poll_strategy_states(const httplib::Request&, httplib::Response& res) {
    Core::Engine::getInstance().poll_strategy_states();
    set_json(res, 200, {{"ok", true}});
}

void poll_realtime_subscriptions(const httplib::Request&,
                                 httplib::Response& res) {
    Core::Engine::getInstance().poll_realtime_subscriptions();
    set_json(res, 200, {{"ok", true}});
}

void process_engine_signals(const httplib::Request&,
                                    httplib::Response& res) {
    Core::Engine::getInstance().process_engine_signals();
    set_json(res, 200, {{"ok", true}});
}

void generate_predictions(const httplib::Request&, httplib::Response& res) {
    Core::Engine::getInstance().generate_predictions();
    set_json(res, 200, {{"ok", true}});
}

void sync_persistent_state_to_disk(const httplib::Request&,
                                   httplib::Response& res) {
    Core::Engine::getInstance().sync_persistent_state_to_disk();
    set_json(res, 200, {{"ok", true}});
}

void flush_trading_logs(const httplib::Request& req, httplib::Response& res) {
    bool force = req.get_param_value("force") == "true";
    Core::Engine::getInstance().flush_trading_logs(force);
    set_json(res, 200, {{"ok", true}, {"force", force}});
}

void emit_state_snapshot(const httplib::Request& req, httplib::Response& res) {
    bool bypass = req.get_param_value("bypass_guard") == "true";
    Core::Engine::getInstance().emit_engine_state_snapshot(bypass);
    set_json(res, 200, {{"ok", true}, {"bypass_guard", bypass}});
}

}  // namespace EngineControl::TriggerHandlers
