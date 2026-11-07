#pragma once

#include <exception>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "httplib.h"
#include "market_making/engine.h"
#include "market_making/strategy.h"

namespace EngineControl {

inline void set_json(httplib::Response& res, int status, nlohmann::json body) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

inline void set_error(httplib::Response& res, int status,
                      const std::string& msg) {
    set_json(res, status, {{"error", msg}});
}

// Many endpoints take the same (exchange, market_type, symbol) triple. Pulls
// them off the query string; returns false + writes a 400 if any is missing.
inline bool require_emt(const httplib::Request& req, httplib::Response& res,
                        std::string& exchange, std::string& market_type,
                        std::string& symbol) {
    exchange = req.get_param_value("exchange");
    market_type = req.get_param_value("market_type");
    symbol = req.get_param_value("symbol");
    if (exchange.empty() || market_type.empty() || symbol.empty()) {
        set_error(res, 400,
                  "missing required query params: exchange, market_type, "
                  "symbol");
        return false;
    }
    return true;
}

// Pulls an int `id` query param and looks up the matching strategy by
// `strategy_config.id`. On miss/parse-failure, writes the appropriate 4xx
// and returns nullptr. Caller can early-return on nullptr.
inline Core::Strategy* require_strategy(const httplib::Request& req,
                                        httplib::Response& res) {
    const std::string id_str = req.get_param_value("id");
    if (id_str.empty()) {
        set_error(res, 400, "missing required query param: id");
        return nullptr;
    }
    int id = 0;
    try {
        id = std::stoi(id_str);
    } catch (const std::exception&) {
        set_error(res, 400, "id must be an integer");
        return nullptr;
    }
    for (auto& s : Core::Engine::getInstance().get_strategies()) {
        if (s->strategy_config.id == id) return s.get();
    }
    set_error(res, 404, "no strategy with that id");
    return nullptr;
}

}  // namespace EngineControl
