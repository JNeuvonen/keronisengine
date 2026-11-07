#include "engine_control_server/strategy_handlers.h"

#include <chrono>
#include <exception>
#include <string>

#include <nlohmann/json.hpp>

#include "engine_control_server/dtype_helpers.h"
#include "engine_control_server/handler_helpers.h"
#include "market_data/state_machine_dispatcher.h"
#include "market_data/strategy_state_machine.h"
#include "market_making/dtypes/order.h"
#include "market_making/dtypes/trade.h"
#include "market_making/strategy.h"

namespace EngineControl::StrategyHandlers {

namespace {

bool parse_body(const httplib::Request& req, httplib::Response& res,
                nlohmann::json& out) {
    try {
        out = nlohmann::json::parse(req.body);
        return true;
    } catch (const std::exception& e) {
        set_error(res, 400, std::string("invalid JSON body: ") + e.what());
        return false;
    }
}

}  // namespace

// ── Strategy reads ───────────────────────────────────────────────────────

void orders(const httplib::Request& req, httplib::Response& res) {
    Core::Strategy* strat = require_strategy(req, res);
    if (!strat) return;
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& o : strat->get_orders()) arr.push_back(o.to_json());
    set_json(res, 200, std::move(arr));
}

void trades(const httplib::Request& req, httplib::Response& res) {
    Core::Strategy* strat = require_strategy(req, res);
    if (!strat) return;
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& t : strat->get_trades()) arr.push_back(t.to_json());
    set_json(res, 200, std::move(arr));
}

void virtual_balance(const httplib::Request& req, httplib::Response& res) {
    Core::Strategy* strat = require_strategy(req, res);
    if (!strat) return;
    set_json(res, 200, {{"virtual_balance", strat->get_virtual_balance()}});
}

void recent_orders(const httplib::Request& req, httplib::Response& res) {
    Core::Strategy* strat = require_strategy(req, res);
    if (!strat) return;
    int window_sec = 10;
    const std::string window_str = req.get_param_value("window_sec");
    if (!window_str.empty()) {
        try {
            window_sec = std::stoi(window_str);
        } catch (const std::exception&) {
            set_error(res, 400, "window_sec must be an integer");
            return;
        }
        if (window_sec <= 0) {
            set_error(res, 400, "window_sec must be positive");
            return;
        }
    }
    std::chrono::seconds window{window_sec};
    set_json(res, 200,
             {{"window_sec", window_sec},
              {"enter_count", strat->get_recent_enter_orders_count(window)},
              {"exit_count", strat->get_recent_exit_orders_count(window)}});
}

// ── SM reads ─────────────────────────────────────────────────────────────

void sm_sync_progress(const httplib::Request& req, httplib::Response& res) {
    Core::Strategy* strat = require_strategy(req, res);
    if (!strat) return;
    auto sm = strat->strategy_event_state_machine;
    if (!sm) {
        set_error(res, 404, "strategy has no state machine attached");
        return;
    }
    auto [synced, total] = sm->get_sync_progress();
    set_json(res, 200,
             {{"is_fully_synced", sm->is_fully_synced()},
              {"synced_deps", synced},
              {"total_deps", total},
              {"min_synced_ts", sm->get_min_synced_ts()}});
}

void sm_features(const httplib::Request& req, httplib::Response& res) {
    Core::Strategy* strat = require_strategy(req, res);
    if (!strat) return;
    auto sm = strat->strategy_event_state_machine;
    if (!sm) {
        set_error(res, 404, "strategy has no state machine attached");
        return;
    }
    const std::string symbol_idx_str = req.get_param_value("symbol_idx");
    std::optional<std::vector<float>> values;
    std::vector<std::string> names;
    if (symbol_idx_str.empty()) {
        values = sm->emit_state();
        names = sm->feature_names();
    } else {
        int symbol_idx = 0;
        try {
            symbol_idx = std::stoi(symbol_idx_str);
        } catch (const std::exception&) {
            set_error(res, 400, "symbol_idx must be an integer");
            return;
        }
        values = sm->emit_state_for_symbol(symbol_idx);
        names = sm->feature_names_for_symbol(symbol_idx);
    }
    if (!values.has_value()) {
        set_error(res, 404, "SM emit_state returned nullopt — not synced yet");
        return;
    }
    nlohmann::json features = nlohmann::json::array();
    for (size_t i = 0; i < values->size(); ++i) {
        features.push_back({
            {"name", i < names.size() ? names[i] : ""},
            {"value", (*values)[i]},
        });
    }
    set_json(res, 200, {{"sm_name", sm->get_name()},
                        {"features", std::move(features)}});
}

// ── SM mutating (gated) ──────────────────────────────────────────────────

void sm_react(const httplib::Request& req, httplib::Response& res) {
    Core::Strategy* strat = require_strategy(req, res);
    if (!strat) return;
    auto sm = strat->strategy_event_state_machine;
    if (!sm) {
        set_error(res, 404, "strategy has no state machine attached");
        return;
    }
    nlohmann::json body;
    if (!parse_body(req, res, body)) return;
    MarketData::MarketEvent ev;
    try {
        ev = DtypeHelpers::parse_sm_market_event(body);
    } catch (const std::exception& e) {
        set_error(res, 400,
                  std::string("invalid MarketEvent body: ") + e.what());
        return;
    }
    bool is_historical = req.get_param_value("is_historical") == "true";
    sm->react(ev, is_historical);
    set_json(res, 200, {{"ok", true}, {"is_historical", is_historical}});
}

// ── SM-by-name reads ─────────────────────────────────────────────────────
//
// Query the StateMachineDispatcher singleton directly so observability
// works for SMs strategies WATCH (not just attach as primary). Required
// because strategy/sm/* above is gated on Strategy::strategy_event_state_machine
// which is only set for strategies that ATTACH an SM. Strategies that only
// watch need this path.

void sm_list(const httplib::Request&, httplib::Response& res) {
    auto& dispatcher = ::MarketData::StateMachineDispatcher::get_instance();
    auto machines = dispatcher.get_machines();
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& m : machines) {
        if (!m) continue;
        auto [synced, total] = m->get_sync_progress();
        arr.push_back({
            {"name", m->get_name()},
            {"is_fully_synced", m->is_fully_synced()},
            {"synced_deps", synced},
            {"total_deps", total},
            {"min_synced_ts", m->get_min_synced_ts()},
        });
    }
    set_json(res, 200, {{"machines", arr}});
}

void sm_by_name_sync_progress(const httplib::Request& req,
                               httplib::Response& res) {
    const std::string name = req.get_param_value("name");
    if (name.empty()) {
        set_error(res, 400, "missing 'name' query parameter");
        return;
    }
    auto sm = ::MarketData::StateMachineDispatcher::get_instance()
                  .get_machine(name);
    if (!sm) {
        set_error(res, 404, "no SM registered with name: " + name);
        return;
    }
    auto [synced, total] = sm->get_sync_progress();
    set_json(res, 200, {
        {"name", sm->get_name()},
        {"is_fully_synced", sm->is_fully_synced()},
        {"synced_deps", synced},
        {"total_deps", total},
        {"min_synced_ts", sm->get_min_synced_ts()},
    });
}

void sm_by_name_features(const httplib::Request& req,
                          httplib::Response& res) {
    const std::string name = req.get_param_value("name");
    if (name.empty()) {
        set_error(res, 400, "missing 'name' query parameter");
        return;
    }
    auto sm = ::MarketData::StateMachineDispatcher::get_instance()
                  .get_machine(name);
    if (!sm) {
        set_error(res, 404, "no SM registered with name: " + name);
        return;
    }
    const std::string symbol_idx_str = req.get_param_value("symbol_idx");
    std::optional<std::vector<float>> values;
    std::vector<std::string> names;
    if (symbol_idx_str.empty()) {
        values = sm->emit_state();
        names = sm->feature_names();
    } else {
        int symbol_idx = 0;
        try {
            symbol_idx = std::stoi(symbol_idx_str);
        } catch (const std::exception&) {
            set_error(res, 400, "symbol_idx must be an integer");
            return;
        }
        values = sm->emit_state_for_symbol(symbol_idx);
        names = sm->feature_names_for_symbol(symbol_idx);
    }
    if (!values.has_value()) {
        set_error(res, 404,
                  "SM emit_state returned nullopt — not synced yet");
        return;
    }
    nlohmann::json features = nlohmann::json::array();
    for (size_t i = 0; i < values->size(); ++i) {
        features.push_back({
            {"name", i < names.size() ? names[i] : ""},
            {"value", (*values)[i]},
        });
    }
    set_json(res, 200, {{"sm_name", sm->get_name()},
                        {"features", std::move(features)}});
}

void sm_by_name_react(const httplib::Request& req,
                       httplib::Response& res) {
    const std::string name = req.get_param_value("name");
    if (name.empty()) {
        set_error(res, 400, "missing 'name' query parameter");
        return;
    }
    auto sm = ::MarketData::StateMachineDispatcher::get_instance()
                  .get_machine(name);
    if (!sm) {
        set_error(res, 404, "no SM registered with name: " + name);
        return;
    }
    nlohmann::json body;
    if (!parse_body(req, res, body)) return;
    MarketData::MarketEvent ev;
    try {
        ev = DtypeHelpers::parse_sm_market_event(body);
    } catch (const std::exception& e) {
        set_error(res, 400,
                  std::string("invalid MarketEvent body: ") + e.what());
        return;
    }
    bool is_historical = req.get_param_value("is_historical") == "true";
    sm->react(ev, is_historical);
    set_json(res, 200, {{"ok", true},
                        {"sm_name", sm->get_name()},
                        {"is_historical", is_historical}});
}

}  // namespace EngineControl::StrategyHandlers
