#include "engine_control_server/write_handlers.h"

#include <exception>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine_control_server/dtype_helpers.h"
#include "engine_control_server/handler_helpers.h"
#include "market_making/dtypes/inventory.h"
#include "market_making/dtypes/order.h"
#include "market_making/dtypes/order_fill_info.h"
#include "market_making/dtypes/orderbook.h"
#include "market_making/dtypes/quote_asset.h"
#include "market_making/dtypes/trade.h"
#include "market_making/engine.h"

namespace EngineControl::WriteHandlers {

namespace {

// Wraps the request-body JSON parse so each handler can early-return a 400
// with a useful message instead of crashing on garbage input.
bool parse_body(const httplib::Request& req, httplib::Response& res,
                nlohmann::json& out) {
    try {
        out = nlohmann::json::parse(req.body);
        return true;
    } catch (const std::exception& e) {
        set_error(res, 400,
                  std::string("invalid JSON body: ") + e.what());
        return false;
    }
}

// MarketEvent parser lives in dtype_helpers.h so SM react can reuse it.
// See that header for the supported subset + extension guidance.

// `Core::PolymarketActiveEvent` has atomic fields (BBA + last-trade per
// outcome). Atomics start at 0 — populate them after the fact via the
// /control/engine/polymarket/{bba,last-trade} endpoints.
std::shared_ptr<Core::PolymarketActiveEvent> build_polymarket_event(
    const nlohmann::json& j) {
    auto event = std::make_shared<Core::PolymarketActiveEvent>();
    event->condition_id = j.at("condition_id").get<std::string>();
    event->series_slug = j.value("series_slug", "");
    event->slug = j.value("slug", "");
    event->event_start_time_ms = j.value("event_start_time_ms", 0LL);
    event->event_end_time_ms = j.value("event_end_time_ms", 0LL);
    event->num_outcomes = j.value("num_outcomes", 0);
    if (j.contains("outcome_names")) {
        for (const auto& name : j["outcome_names"]) {
            event->outcome_names.push_back(name.get<std::string>());
        }
    }
    if (j.contains("clob_token_ids")) {
        for (const auto& id : j["clob_token_ids"]) {
            event->clob_token_ids.push_back(id.get<std::string>());
        }
    }
    return event;
}

// `Core::OrderBookUpdate` has no `from_json` factory in the engine headers
// (orderbook.h:200). The struct is a flat carrier — write the deserializer
// here so the rest of the codebase doesn't grow a one-off impl just for the
// control server.
Core::OrderBookUpdate parse_orderbook_update(const nlohmann::json& j) {
    Core::OrderBookUpdate u;
    if (j.contains("asks")) {
        for (const auto& p : j["asks"]) {
            u.asks.emplace_back(p[0].get<double>(), p[1].get<double>());
        }
    }
    if (j.contains("bids")) {
        for (const auto& p : j["bids"]) {
            u.bids.emplace_back(p[0].get<double>(), p[1].get<double>());
        }
    }
    u.symbol_full = j.value("symbol_full", "");
    u.timestamp = j.value("timestamp", 0LL);
    u.sequence_start = j.value("sequence_start", 0LL);
    u.sequence_end = j.value("sequence_end", 0LL);
    return u;
}

}  // namespace

// ── Order lifecycle ──────────────────────────────────────────────────────

void order_created_ack(const httplib::Request& req, httplib::Response& res) {
    nlohmann::json body;
    if (!parse_body(req, res, body)) return;
    Core::Order order = Core::Order::from_json(body);
    Core::Engine::getInstance().order_created_ack(order);
    set_json(res, 200, {{"ok", true}});
}

void order_filled_ack(const httplib::Request& req, httplib::Response& res) {
    nlohmann::json body;
    if (!parse_body(req, res, body)) return;
    Core::OrderFillInfo info = Core::OrderFillInfo::from_json(body);
    Core::Engine::getInstance().order_filled_ack(info);
    set_json(res, 200, {{"ok", true}});
}

void order_canceled_ack(const httplib::Request& req, httplib::Response& res) {
    nlohmann::json body;
    if (!parse_body(req, res, body)) return;
    Core::Order order = Core::Order::from_json(body);
    bool creation_failed = false;
    if (!req.get_param_value("creation_failed").empty()) {
        creation_failed = req.get_param_value("creation_failed") == "true";
    }
    Core::Engine::getInstance().order_canceled_ack(order, creation_failed);
    set_json(res, 200, {{"ok", true}});
}

void update_order(const httplib::Request& req, httplib::Response& res) {
    nlohmann::json body;
    if (!parse_body(req, res, body)) return;
    Core::Order order = Core::Order::from_json(body);
    Core::Engine::getInstance().update_order(order);
    set_json(res, 200, {{"ok", true}});
}

void misc_track(const httplib::Request& req, httplib::Response& res) {
    nlohmann::json body;
    if (!parse_body(req, res, body)) return;
    Core::Order order = Core::Order::from_json(body);
    Core::Engine::getInstance().add_tracked_misc_order(order);
    set_json(res, 200, {{"ok", true}});
}

void misc_untrack(const httplib::Request& req, httplib::Response& res) {
    nlohmann::json body;
    if (!parse_body(req, res, body)) return;
    Core::Order order = Core::Order::from_json(body);
    Core::Engine::getInstance().remove_tracked_misc_order(order);
    set_json(res, 200, {{"ok", true}});
}

// ── Market data injection ────────────────────────────────────────────────

void update_bba(const httplib::Request& req, httplib::Response& res) {
    nlohmann::json body;
    if (!parse_body(req, res, body)) return;
    try {
        const std::string exchange = body.at("exchange").get<std::string>();
        const std::string market_type =
            body.at("market_type").get<std::string>();
        const std::string symbol = body.at("symbol").get<std::string>();
        const double bid_price = body.at("bid_price").get<double>();
        const double bid_qty = body.at("bid_qty").get<double>();
        const double ask_price = body.at("ask_price").get<double>();
        const double ask_qty = body.at("ask_qty").get<double>();
        const long long ts = body.at("timestamp_ms").get<long long>();
        Core::Engine::getInstance().update_bba(exchange, market_type, symbol,
                                                bid_price, bid_qty, ask_price,
                                                ask_qty, ts);
    } catch (const std::exception& e) {
        set_error(res, 400,
                  std::string("missing / invalid field in body: ") + e.what());
        return;
    }
    set_json(res, 200, {{"ok", true}});
}

void update_price(const httplib::Request& req, httplib::Response& res) {
    nlohmann::json body;
    if (!parse_body(req, res, body)) return;
    try {
        const std::string symbol = body.at("symbol").get<std::string>();
        const std::string exchange = body.at("exchange").get<std::string>();
        const std::string market_type =
            body.at("market_type").get<std::string>();
        const double price = body.at("price").get<double>();
        Core::Engine::getInstance().update_price(symbol, exchange, market_type,
                                                  price);
    } catch (const std::exception& e) {
        set_error(res, 400,
                  std::string("missing / invalid field in body: ") + e.what());
        return;
    }
    set_json(res, 200, {{"ok", true}});
}

void init_orderbook(const httplib::Request& req, httplib::Response& res) {
    nlohmann::json body;
    if (!parse_body(req, res, body)) return;
    try {
        const std::string exchange = body.at("exchange").get<std::string>();
        const std::string market_type =
            body.at("market_type").get<std::string>();
        const std::string symbol = body.at("symbol").get<std::string>();
        Core::Engine::getInstance().init_orderbook(exchange, market_type,
                                                    symbol);
    } catch (const std::exception& e) {
        set_error(res, 400,
                  std::string("missing / invalid field in body: ") + e.what());
        return;
    }
    set_json(res, 200, {{"ok", true}});
}

void apply_full_refresh_orderbook(const httplib::Request& req,
                                  httplib::Response& res) {
    nlohmann::json body;
    if (!parse_body(req, res, body)) return;
    try {
        const std::string exchange = body.at("exchange").get<std::string>();
        const std::string market_type =
            body.at("market_type").get<std::string>();
        const std::string symbol = body.at("symbol").get<std::string>();
        Core::OrderBookLevels levels =
            Core::OrderBookLevels::from_json(body.at("levels"));
        Core::Engine::getInstance().apply_full_refresh_orderbook(
            exchange, market_type, symbol, levels);
    } catch (const std::exception& e) {
        set_error(res, 400,
                  std::string("invalid body: ") + e.what());
        return;
    }
    set_json(res, 200, {{"ok", true}});
}

void apply_orderbook_update(const httplib::Request& req,
                            httplib::Response& res) {
    nlohmann::json body;
    if (!parse_body(req, res, body)) return;
    try {
        Core::OrderBookUpdate update = parse_orderbook_update(body);
        Core::Engine::getInstance().apply_orderbook_update(update);
    } catch (const std::exception& e) {
        set_error(res, 400, std::string("invalid body: ") + e.what());
        return;
    }
    set_json(res, 200, {{"ok", true}});
}

void apply_orderbook_levels_only(const httplib::Request& req,
                                 httplib::Response& res) {
    nlohmann::json body;
    if (!parse_body(req, res, body)) return;
    try {
        const std::string exchange = body.at("exchange").get<std::string>();
        const std::string market_type =
            body.at("market_type").get<std::string>();
        const std::string symbol = body.at("symbol").get<std::string>();
        Core::OrderBookLevels levels =
            Core::OrderBookLevels::from_json(body.at("levels"));
        Core::Engine::getInstance().apply_orderbook_levels_only(
            exchange, market_type, symbol, levels);
    } catch (const std::exception& e) {
        set_error(res, 400, std::string("invalid body: ") + e.what());
        return;
    }
    set_json(res, 200, {{"ok", true}});
}

// ── Inventory + balances ─────────────────────────────────────────────────

void update_inventory(const httplib::Request& req, httplib::Response& res) {
    nlohmann::json body;
    if (!parse_body(req, res, body)) return;
    try {
        const std::string exchange = body.at("exchange").get<std::string>();
        const std::string market_type =
            body.at("market_type").get<std::string>();
        const std::string symbol = body.at("symbol").get<std::string>();
        const long long account_id = body.at("account_id").get<long long>();
        Core::Inventory inv = Core::Inventory::from_json(body.at("inventory"));
        Core::Engine::getInstance().update_inventory(exchange, market_type,
                                                      symbol, account_id, inv);
    } catch (const std::exception& e) {
        set_error(res, 400, std::string("invalid body: ") + e.what());
        return;
    }
    set_json(res, 200, {{"ok", true}});
}

void update_quote_asset(const httplib::Request& req, httplib::Response& res) {
    nlohmann::json body;
    if (!parse_body(req, res, body)) return;
    try {
        const std::string exchange = body.at("exchange").get<std::string>();
        const std::string market_type =
            body.at("market_type").get<std::string>();
        const std::string asset = body.at("asset").get<std::string>();
        const long long account_id = body.at("account_id").get<long long>();
        Core::QuoteAsset qa = Core::QuoteAsset::from_json(body.at("quote_asset"));
        Core::Engine::getInstance().update_quote_asset(
            exchange, market_type, asset, account_id, qa);
    } catch (const std::exception& e) {
        set_error(res, 400, std::string("invalid body: ") + e.what());
        return;
    }
    set_json(res, 200, {{"ok", true}});
}

void update_borrowable_inventory(const httplib::Request& req,
                                 httplib::Response& res) {
    // The singular `Engine::update_borrowable_inventory(string, double)` is
    // declared in engine.h but has no definition (dead declaration). The
    // plural `update_borrowable_inventories(map)` is the real entrypoint and
    // is what we wire to. Body shape: `{"updates": {"key1": value1, ...}}`.
    nlohmann::json body;
    if (!parse_body(req, res, body)) return;
    std::unordered_map<std::string, double> updates;
    try {
        for (const auto& [k, v] : body.at("updates").items()) {
            updates[k] = v.get<double>();
        }
        Core::Engine::getInstance().update_borrowable_inventories(updates);
    } catch (const std::exception& e) {
        set_error(res, 400, std::string("invalid body: ") + e.what());
        return;
    }
    set_json(res, 200, {{"ok", true}, {"updated_count", updates.size()}});
}

void update_inventory_and_quote_assets(const httplib::Request& req,
                                       httplib::Response& res) {
    nlohmann::json body;
    if (!parse_body(req, res, body)) return;
    try {
        const std::string exchange = body.at("exchange").get<std::string>();
        const std::string market_type =
            body.at("market_type").get<std::string>();
        const long long account_id = body.at("account_id").get<long long>();
        std::vector<Core::Inventory> inventories;
        for (const auto& j : body.at("inventories")) {
            inventories.push_back(Core::Inventory::from_json(j));
        }
        std::vector<Core::QuoteAsset> quote_assets;
        for (const auto& j : body.at("quote_assets")) {
            quote_assets.push_back(Core::QuoteAsset::from_json(j));
        }
        Core::Engine::getInstance().update_inventory_and_quote_assets(
            exchange, market_type, account_id, inventories, quote_assets);
    } catch (const std::exception& e) {
        set_error(res, 400, std::string("invalid body: ") + e.what());
        return;
    }
    set_json(res, 200, {{"ok", true}});
}

// ── Trade management ─────────────────────────────────────────────────────

void inventory_mismatch_remove_trade(const httplib::Request& req,
                                     httplib::Response& res) {
    nlohmann::json body;
    if (!parse_body(req, res, body)) return;
    try {
        Core::Trade trade = Core::Trade::from_json(body);
        Core::Engine::getInstance().inventory_mismatch_remove_trade(trade);
    } catch (const std::exception& e) {
        set_error(res, 400, std::string("invalid body: ") + e.what());
        return;
    }
    set_json(res, 200, {{"ok", true}});
}

void manually_remove_trade(const httplib::Request& req,
                           httplib::Response& res) {
    nlohmann::json body;
    if (!parse_body(req, res, body)) return;
    Core::ManuallyRemovedTrade removal;
    try {
        removal.db_id = body.at("db_id").get<int>();
        removal.symbol = body.at("symbol").get<std::string>();
        removal.strategy_id = body.at("strategy_id").get<int>();
        removal.trade_id = body.at("trade_id").get<unsigned long long>();
    } catch (const std::exception& e) {
        set_error(res, 400, std::string("invalid body: ") + e.what());
        return;
    }
    bool removed = Core::Engine::getInstance().manually_remove_trade(removal);
    set_json(res, 200, {{"removed", removed}});
}

// ── MarketEvent injection (Phase B.2) ────────────────────────────────────

void market_event(const httplib::Request& req, httplib::Response& res) {
    nlohmann::json body;
    if (!parse_body(req, res, body)) return;
    Core::MarketEvent ev;
    try {
        ev = DtypeHelpers::parse_market_event(body);
    } catch (const std::exception& e) {
        set_error(res, 400, std::string("invalid body: ") + e.what());
        return;
    }
    const std::string mode = req.get_param_value("mode");
    auto& engine = Core::Engine::getInstance();
    if (mode == "trade") {
        engine.trade_market_event(ev);
    } else {
        // Default = process. trade_market_event is the strategy-reaction
        // path; process_market_event is the state-update path.
        engine.process_market_event(ev);
    }
    set_json(res, 200, {{"ok", true}, {"mode", mode.empty() ? "process" : mode}});
}

void funding_payment(const httplib::Request& req, httplib::Response& res) {
    nlohmann::json body;
    if (!parse_body(req, res, body)) return;
    try {
        Core::MarketEvent ev = DtypeHelpers::parse_market_event(body);
        Core::Engine::getInstance().add_funding_payment(ev);
    } catch (const std::exception& e) {
        set_error(res, 400, std::string("invalid body: ") + e.what());
        return;
    }
    set_json(res, 200, {{"ok", true}});
}

// ── Polymarket (Phase B.2) ───────────────────────────────────────────────

void polymarket_event_add(const httplib::Request& req, httplib::Response& res) {
    nlohmann::json body;
    if (!parse_body(req, res, body)) return;
    try {
        auto event = build_polymarket_event(body);
        Core::Engine::getInstance().add_polymarket_event(event);
    } catch (const std::exception& e) {
        set_error(res, 400, std::string("invalid body: ") + e.what());
        return;
    }
    set_json(res, 200, {{"ok", true}});
}

void polymarket_event_remove(const httplib::Request& req,
                             httplib::Response& res) {
    nlohmann::json body;
    if (!parse_body(req, res, body)) return;
    try {
        const std::string condition_id =
            body.at("condition_id").get<std::string>();
        Core::Engine::getInstance().remove_polymarket_event(condition_id);
    } catch (const std::exception& e) {
        set_error(res, 400, std::string("invalid body: ") + e.what());
        return;
    }
    set_json(res, 200, {{"ok", true}});
}

void polymarket_event_bba(const httplib::Request& req, httplib::Response& res) {
    nlohmann::json body;
    if (!parse_body(req, res, body)) return;
    try {
        const std::string condition_id =
            body.at("condition_id").get<std::string>();
        const int outcome_index = body.at("outcome_index").get<int>();
        const double bid = body.at("best_bid").get<double>();
        const double ask = body.at("best_ask").get<double>();
        const long long ts = body.at("timestamp_ms").get<long long>();
        Core::Engine::getInstance().update_polymarket_event_bba(
            condition_id, outcome_index, bid, ask, ts);
    } catch (const std::exception& e) {
        set_error(res, 400, std::string("invalid body: ") + e.what());
        return;
    }
    set_json(res, 200, {{"ok", true}});
}

void polymarket_event_last_trade(const httplib::Request& req,
                                 httplib::Response& res) {
    nlohmann::json body;
    if (!parse_body(req, res, body)) return;
    try {
        const std::string condition_id =
            body.at("condition_id").get<std::string>();
        const int outcome_index = body.at("outcome_index").get<int>();
        const double price = body.at("price").get<double>();
        const long long ts = body.at("timestamp_ms").get<long long>();
        Core::Engine::getInstance().update_polymarket_event_last_trade(
            condition_id, outcome_index, price, ts);
    } catch (const std::exception& e) {
        set_error(res, 400, std::string("invalid body: ") + e.what());
        return;
    }
    set_json(res, 200, {{"ok", true}});
}

void polymarket_orderbook_init(const httplib::Request& req,
                               httplib::Response& res) {
    nlohmann::json body;
    if (!parse_body(req, res, body)) return;
    try {
        const std::string asset_id = body.at("asset_id").get<std::string>();
        Core::Engine::getInstance().init_polymarket_orderbook(asset_id);
    } catch (const std::exception& e) {
        set_error(res, 400, std::string("invalid body: ") + e.what());
        return;
    }
    set_json(res, 200, {{"ok", true}});
}

void polymarket_orderbook_snapshot(const httplib::Request& req,
                                   httplib::Response& res) {
    nlohmann::json body;
    if (!parse_body(req, res, body)) return;
    try {
        const std::string asset_id = body.at("asset_id").get<std::string>();
        Core::OrderBookLevels levels =
            Core::OrderBookLevels::from_json(body.at("levels"));
        Core::Engine::getInstance().apply_polymarket_l2_snapshot(asset_id,
                                                                  levels);
    } catch (const std::exception& e) {
        set_error(res, 400, std::string("invalid body: ") + e.what());
        return;
    }
    set_json(res, 200, {{"ok", true}});
}

void polymarket_orderbook_delta(const httplib::Request& req,
                                httplib::Response& res) {
    nlohmann::json body;
    if (!parse_body(req, res, body)) return;
    try {
        const std::string asset_id = body.at("asset_id").get<std::string>();
        const double price = body.at("price").get<double>();
        const double size = body.at("size").get<double>();
        const bool is_bid = body.at("is_bid").get<bool>();
        const long long ts = body.at("timestamp_ms").get<long long>();
        Core::Engine::getInstance().apply_polymarket_l2_delta(
            asset_id, price, size, is_bid, ts);
    } catch (const std::exception& e) {
        set_error(res, 400, std::string("invalid body: ") + e.what());
        return;
    }
    set_json(res, 200, {{"ok", true}});
}

}  // namespace EngineControl::WriteHandlers
