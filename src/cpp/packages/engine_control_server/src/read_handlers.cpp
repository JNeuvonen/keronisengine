#include "engine_control_server/read_handlers.h"

#include <utility>

#include <nlohmann/json.hpp>

#include "datetime_utils.h"
#include "engine_control_server/handler_helpers.h"
#include "market_making/dtypes/full_account_state.h"
#include "market_making/dtypes/inventory.h"
#include "market_making/engine.h"

namespace EngineControl::ReadHandlers {

void health(httplib::Response& res,
            std::chrono::steady_clock::time_point start_time,
            bool mutations_enabled) {
    auto& engine = Core::Engine::getInstance();
    auto now = std::chrono::steady_clock::now();
    long long uptime_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time)
            .count();

    set_json(res, 200,
             {{"status", "ok"},
              {"initialized", engine.isInitialized()},
              {"state_ready", engine.get_is_state_ready()},
              {"trading", engine.is_engine_trading()},
              {"actively_trading", engine.is_engine_actively_trading()},
              {"mutations_enabled", mutations_enabled},
              {"uptime_ms", uptime_ms}});
}

void orders(const httplib::Request& req, httplib::Response& res) {
    auto& engine = Core::Engine::getInstance();
    std::vector<Core::Order> orders;

    const std::string symbol = req.get_param_value("symbol");
    const std::string exchange = req.get_param_value("exchange");
    const std::string market_type = req.get_param_value("market_type");

    if (!symbol.empty() && !exchange.empty() && !market_type.empty()) {
        orders = engine.get_orders(symbol, exchange, market_type);
    } else if (!symbol.empty()) {
        orders = engine.get_orders(symbol);
    } else {
        orders = engine.get_orders();
    }

    nlohmann::json arr = nlohmann::json::array();
    for (const auto& o : orders) arr.push_back(o.to_json());
    set_json(res, 200, std::move(arr));
}

void orders_unacked(httplib::Response& res) {
    auto orders = Core::Engine::getInstance().get_unacked_orders();
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& o : orders) arr.push_back(o.to_json());
    set_json(res, 200, std::move(arr));
}

void orders_failed_cancels(httplib::Response& res) {
    auto orders = Core::Engine::getInstance().get_failed_cancels();
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& o : orders) arr.push_back(o.to_json());
    set_json(res, 200, std::move(arr));
}

void trades(httplib::Response& res) {
    auto trades = Core::Engine::getInstance().get_trades();
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& t : trades) arr.push_back(t.to_json());
    set_json(res, 200, std::move(arr));
}

void strategies(httplib::Response& res) {
    auto& engine = Core::Engine::getInstance();
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& s : engine.get_strategies()) {
        nlohmann::json entry;
        entry["id"] = s->strategy_config.id;
        entry["name"] = s->strategy_config.strategy_name;
        entry["type"] = s->strategy_config.strategy_type;
        entry["is_in_reduce_only"] = s->is_in_reduce_only;
        entry["is_using_hedges"] = s->is_using_hedges;
        entry["is_market_making_strategy"] = s->is_market_making_strategy;
        entry["needs_to_run_realtime"] = s->needs_to_run_realtime;
        entry["account_mode"] = s->account_mode;
        entry["stop_making_markets"] = s->stop_making_markets.load();
        entry["virtual_balance"] = s->get_virtual_balance();
        entry["trade_tickers_count"] = s->trade_tickers.size();
        arr.push_back(std::move(entry));
    }
    set_json(res, 200, std::move(arr));
}

void strategy_telemetry(httplib::Response& res) {
    auto telemetry =
        Core::Engine::getInstance().get_latest_strategy_telemetry();
    nlohmann::json obj = nlohmann::json::object();
    if (telemetry) {
        for (const auto& [strategy_id, event] : *telemetry) {
            obj[std::to_string(strategy_id)] = event.to_json();
        }
    }
    set_json(res, 200, std::move(obj));
}

void exposure(httplib::Response& res) {
    auto& engine = Core::Engine::getInstance();
    set_json(res, 200,
             {{"nav", engine.get_nav()},
              {"long_ratio", engine.get_long_ratio().toDouble()},
              {"short_ratio", engine.get_short_ratio().toDouble()}});
}

void inventory(const httplib::Request& req, httplib::Response& res) {
    std::string exchange, market_type, symbol;
    if (!require_emt(req, res, exchange, market_type, symbol)) return;
    const std::string account_id_str = req.get_param_value("account_id");
    if (account_id_str.empty()) {
        set_error(res, 400, "missing required query param: account_id");
        return;
    }
    long long account_id = 0;
    try {
        account_id = std::stoll(account_id_str);
    } catch (const std::exception&) {
        set_error(res, 400, "account_id must be an integer");
        return;
    }
    auto inv = Core::Engine::getInstance().get_inventory(
        exchange, market_type, symbol, account_id);
    if (!inv.has_value()) {
        set_error(res, 404, "no inventory for that key");
        return;
    }
    set_json(res, 200, inv->to_json());
}

void full_account_state(const httplib::Request& req, httplib::Response& res) {
    const std::string account_id_str = req.get_param_value("account_id");
    if (account_id_str.empty()) {
        set_error(res, 400, "missing required query param: account_id");
        return;
    }
    long long account_id = 0;
    try {
        account_id = std::stoll(account_id_str);
    } catch (const std::exception&) {
        set_error(res, 400, "account_id must be an integer");
        return;
    }
    auto state = Core::Engine::getInstance().get_full_account_state(account_id);
    nlohmann::json j;
    j["quote_assets"] = nlohmann::json::object();
    for (const auto& [k, qa] : state.quote_assets) {
        j["quote_assets"][k] = qa.to_json();
    }
    j["inventories"] = nlohmann::json::object();
    for (const auto& [k, inv] : state.inventories) {
        j["inventories"][k] = inv.to_json();
    }
    j["trades"] = nlohmann::json::array();
    for (const auto& t : state.trades) j["trades"].push_back(t.to_json());
    set_json(res, 200, std::move(j));
}

void bba(const httplib::Request& req, httplib::Response& res) {
    std::string exchange, market_type, symbol;
    if (!require_emt(req, res, exchange, market_type, symbol)) return;
    auto bba = Core::Engine::getInstance().get_bba(symbol, exchange,
                                                    market_type);
    if (!bba.has_value()) {
        set_error(res, 404, "no BBA for that key");
        return;
    }
    set_json(res, 200,
             {{"best_bid_price", bba->best_bid_price},
              {"best_bid_qty", bba->best_bid_qty},
              {"best_ask_price", bba->best_ask_price},
              {"best_ask_qty", bba->best_ask_qty},
              {"timestamp_ms", bba->timestamp_ms}});
}

void orderbook(const httplib::Request& req, httplib::Response& res) {
    // Engine::get_order_book has no impl in the current codebase — declared
    // but never defined. BBA covers the most common summary use case; full
    // L2 book serialization can land in Phase B alongside the orderbook
    // injection endpoints if a scenario demands it.
    (void)req;
    set_error(res, 501,
              "orderbook readout not implemented yet — use /control/engine/bba "
              "for top-of-book; full L2 lands in Phase B");
}

void price(const httplib::Request& req, httplib::Response& res) {
    std::string exchange, market_type, symbol;
    if (!require_emt(req, res, exchange, market_type, symbol)) return;
    auto p =
        Core::Engine::getInstance().get_price(symbol, exchange, market_type);
    if (!p.has_value()) {
        set_error(res, 404, "no price for that key");
        return;
    }
    set_json(res, 200, {{"price", *p}});
}

void spread(const httplib::Request& req, httplib::Response& res) {
    std::string exchange, market_type, symbol;
    if (!require_emt(req, res, exchange, market_type, symbol)) return;
    auto s =
        Core::Engine::getInstance().get_spread(symbol, exchange, market_type);
    if (!s.has_value()) {
        set_error(res, 404, "no spread for that key");
        return;
    }
    set_json(res, 200, {{"spread", *s}});
}

void state_snapshot(httplib::Response& res) {
    auto snapshot = Core::Engine::getInstance().build_realtime_snapshot();
    set_json(res, 200, snapshot.to_json());
}

void orderbooks_out_of_sync(httplib::Response& res) {
    auto out = Core::Engine::getInstance().get_order_books_out_of_sync();
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& s : out) arr.push_back(s);
    set_json(res, 200, std::move(arr));
}

void counters(httplib::Response& res) {
    auto& engine = Core::Engine::getInstance();
    set_json(
        res, 200,
        {{"num_market_events_processed",
          engine.num_market_events_processed.load()},
         {"num_backpressure_events", engine.num_backpressure_events_},
         {"num_dropped_trade_events", engine.num_dropped_trade_events_},
         {"num_dropped_orderbook_events", engine.num_dropped_orderbook_events_},
         {"latest_market_event_time_ms",
          engine.get_latest_market_event_time_ms()}});
}

void borrowable_inventory(const httplib::Request& req,
                            httplib::Response& res) {
    const std::string asset = req.get_param_value("asset");
    const std::string exchange = req.get_param_value("exchange");
    const std::string market_type = req.get_param_value("market_type");
    const std::string account_id_str = req.get_param_value("account_id");
    if (asset.empty() || exchange.empty() || market_type.empty()
            || account_id_str.empty()) {
        set_error(res, 400,
                  "missing required query params: asset, exchange, "
                  "market_type, account_id");
        return;
    }
    long long account_id = 0;
    try {
        account_id = std::stoll(account_id_str);
    } catch (const std::exception&) {
        set_error(res, 400, "account_id must be an integer");
        return;
    }
    auto val = Core::Engine::getInstance().get_borrowable_inventory(
        asset, exchange, market_type, account_id);
    if (!val.has_value()) {
        set_error(res, 404,
                  "no borrowable inventory for that key");
        return;
    }
    set_json(res, 200, {
        {"asset", asset},
        {"exchange", exchange},
        {"market_type", market_type},
        {"account_id", account_id},
        {"value", *val},
    });
}

void misc_tracked_orders(httplib::Response& res) {
    auto map = Core::Engine::getInstance().get_tracked_misc_orders();
    nlohmann::json obj = nlohmann::json::object();
    for (auto& [key, orders] : map) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& o : orders) arr.push_back(o.to_json());
        obj[key] = std::move(arr);
    }
    set_json(res, 200, std::move(obj));
}

void markouts_pending(httplib::Response& res) {
    auto jobs = Core::Engine::getInstance().get_pending_markouts_snapshot();
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& j : jobs) {
        nlohmann::json snaps = nlohmann::json::array();
        for (const auto& s : j.snapshots) {
            snaps.push_back({
                {"timestamp_ms", s.timestamp_ms},
                {"elapsed_ms", s.elapsed_ms},
                {"mid_price", s.mid_price},
                {"markout_bps", s.markout_bps},
            });
        }
        arr.push_back({
            {"order_fill_id", j.order_fill_id},
            {"order_id", j.order_id},
            {"strategy_id", j.strategy_id},
            {"symbol", j.symbol},
            {"exchange_name", j.exchange_name},
            {"market_type", j.market_type},
            {"fill_price", j.fill_price},
            {"fill_side", static_cast<int>(j.fill_side)},
            {"fill_timestamp_ms", j.fill_timestamp_ms},
            {"duration_ms", j.duration_ms},
            {"interval_ms", j.interval_ms},
            {"snapshots", std::move(snaps)},
        });
    }
    set_json(res, 200, std::move(arr));
}

void exchange_info(const httplib::Request& req, httplib::Response& res) {
    auto infos =
        Core::Engine::getInstance().get_exchange_symbol_infos();
    const std::string filter_symbol = req.get_param_value("symbol");
    nlohmann::json obj = nlohmann::json::object();
    for (const auto& [key, info] : infos) {
        if (!filter_symbol.empty() && info.symbol != filter_symbol) continue;
        obj[key] = info.to_json();
    }
    set_json(res, 200, std::move(obj));
}

void engine_time(httplib::Response& res) {
    set_json(res, 200,
             {{"engine_time_ms", Core::Engine::getInstance().get_time()},
              {"wall_time_ms", DatetimeUtils::get_current_time_ms()}});
}

void polymarket_orderbook_bba(const httplib::Request& req,
                                httplib::Response& res) {
    const std::string asset_id = req.get_param_value("asset_id");
    if (asset_id.empty()) {
        set_error(res, 400, "missing required query param: asset_id");
        return;
    }
    auto* ob = Core::Engine::getInstance()
                   .get_polymarket_orderbook(asset_id);
    if (!ob) {
        set_error(res, 404,
                  "no polymarket orderbook for that asset_id");
        return;
    }
    set_json(res, 200, {
        {"asset_id", asset_id},
        {"has_bid", ob->bba_has_bid.load()},
        {"has_ask", ob->bba_has_ask.load()},
        {"best_bid_price", ob->bba_bid_price.load()},
        {"best_bid_qty", ob->bba_bid_qty.load()},
        {"best_bid_updated_ms", ob->bba_bid_updated_ms.load()},
        {"best_ask_price", ob->bba_ask_price.load()},
        {"best_ask_qty", ob->bba_ask_qty.load()},
        {"best_ask_updated_ms", ob->bba_ask_updated_ms.load()},
        {"is_in_sync", ob->is_in_sync},
        {"last_sequence_number", ob->last_sequence_number},
    });
}

void polymarket_event(const httplib::Request& req, httplib::Response& res) {
    const std::string condition_id = req.get_param_value("condition_id");
    if (condition_id.empty()) {
        set_error(res, 400, "missing required query param: condition_id");
        return;
    }
    auto event = Core::Engine::getInstance().get_polymarket_event(condition_id);
    if (!event.has_value()) {
        set_error(res, 404, "no polymarket event with that condition_id");
        return;
    }
    auto& e = **event;
    nlohmann::json outcomes = nlohmann::json::array();
    for (int i = 0; i < e.num_outcomes; ++i) {
        outcomes.push_back({
            {"name", e.outcome_names[i]},
            {"clob_token_id",
             i < static_cast<int>(e.clob_token_ids.size())
                 ? e.clob_token_ids[i]
                 : ""},
            {"best_bid", e.outcome_state[i].best_bid.load()},
            {"best_ask", e.outcome_state[i].best_ask.load()},
            {"bba_timestamp_ms", e.outcome_state[i].bba_timestamp_ms.load()},
            {"last_trade_price", e.outcome_state[i].last_trade_price.load()},
            {"last_trade_timestamp_ms",
             e.outcome_state[i].last_trade_timestamp_ms.load()},
        });
    }
    set_json(res, 200,
             {{"condition_id", e.condition_id},
              {"series_slug", e.series_slug},
              {"slug", e.slug},
              {"event_start_time_ms", e.event_start_time_ms},
              {"event_end_time_ms", e.event_end_time_ms},
              {"num_outcomes", e.num_outcomes},
              {"outcomes", std::move(outcomes)}});
}

}  // namespace EngineControl::ReadHandlers
