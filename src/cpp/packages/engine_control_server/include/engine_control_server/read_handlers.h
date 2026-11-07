#pragma once

#include <chrono>

#include "httplib.h"

namespace EngineControl::ReadHandlers {

// Free-function read handlers. All hit Core::Engine::getInstance() directly.
// Grouped here so an agent looking for "where do reads live" finds one file.

void health(httplib::Response& res,
            std::chrono::steady_clock::time_point start_time,
            bool mutations_enabled);
void orders(const httplib::Request& req, httplib::Response& res);
void orders_unacked(httplib::Response& res);
void orders_failed_cancels(httplib::Response& res);
void trades(httplib::Response& res);
void strategies(httplib::Response& res);
void strategy_telemetry(httplib::Response& res);
void exposure(httplib::Response& res);
void inventory(const httplib::Request& req, httplib::Response& res);
void full_account_state(const httplib::Request& req, httplib::Response& res);
void bba(const httplib::Request& req, httplib::Response& res);
void orderbook(const httplib::Request& req, httplib::Response& res);
void price(const httplib::Request& req, httplib::Response& res);
void spread(const httplib::Request& req, httplib::Response& res);
void state_snapshot(httplib::Response& res);
void orderbooks_out_of_sync(httplib::Response& res);
void counters(httplib::Response& res);
void markouts_pending(httplib::Response& res);
void misc_tracked_orders(httplib::Response& res);
void borrowable_inventory(const httplib::Request& req,
                            httplib::Response& res);
void exchange_info(const httplib::Request& req, httplib::Response& res);
void engine_time(httplib::Response& res);

// Polymarket — fetch the active event by condition_id, including live atomic
// BBA + last-trade values per outcome. Test enabler for the Phase B.2 write
// endpoints.
void polymarket_event(const httplib::Request& req, httplib::Response& res);

// Polymarket L2 orderbook BBA snapshot — keyed by asset_id.
// publish_bba writes atomics on the OrderBook itself (NOT the engine's
// bba_states_ cache used for exchange books), so polymarket BBA reads
// can't share /control/engine/bba.
void polymarket_orderbook_bba(const httplib::Request& req,
                                httplib::Response& res);

}  // namespace EngineControl::ReadHandlers
