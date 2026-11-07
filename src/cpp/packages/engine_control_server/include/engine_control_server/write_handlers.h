#pragma once

#include "httplib.h"

namespace EngineControl::WriteHandlers {

// Free-function mutating handlers. All hit Core::Engine::getInstance()
// directly. Grouped here so an agent looking for "where do writes live"
// finds one file. Routed via Server::register_mutating(), which gates the
// whole set behind ENABLE_ENGINE_CONTROL_MUTATIONS — when disabled, every
// write route gets a 403 stub instead of these.

// Order lifecycle
void order_created_ack(const httplib::Request& req, httplib::Response& res);
void order_filled_ack(const httplib::Request& req, httplib::Response& res);
void order_canceled_ack(const httplib::Request& req, httplib::Response& res);
void update_order(const httplib::Request& req, httplib::Response& res);
void misc_track(const httplib::Request& req, httplib::Response& res);
void misc_untrack(const httplib::Request& req, httplib::Response& res);

// Market data injection
void update_bba(const httplib::Request& req, httplib::Response& res);
void update_price(const httplib::Request& req, httplib::Response& res);
void init_orderbook(const httplib::Request& req, httplib::Response& res);
void apply_full_refresh_orderbook(const httplib::Request& req,
                                  httplib::Response& res);
void apply_orderbook_update(const httplib::Request& req,
                            httplib::Response& res);
void apply_orderbook_levels_only(const httplib::Request& req,
                                 httplib::Response& res);

// MarketEvent injection (Phase B.2). ?mode=process|trade selects between
// process_market_event and trade_market_event. Body subset of MarketEvent
// fields supported; sub-struct optionals (order_book, aggr_trade, trade,
// fill_bar, polymarket_trade, polymarket_event_meta, backtest_prediction,
// bar_flush_info) deferred until a scenario needs them.
void market_event(const httplib::Request& req, httplib::Response& res);
void funding_payment(const httplib::Request& req, httplib::Response& res);

// Inventory + balances
void update_inventory(const httplib::Request& req, httplib::Response& res);
void update_quote_asset(const httplib::Request& req, httplib::Response& res);
void update_borrowable_inventory(const httplib::Request& req,
                                 httplib::Response& res);
void update_inventory_and_quote_assets(const httplib::Request& req,
                                       httplib::Response& res);

// Trade management
void inventory_mismatch_remove_trade(const httplib::Request& req,
                                     httplib::Response& res);
void manually_remove_trade(const httplib::Request& req, httplib::Response& res);

// Polymarket (Phase B.2). PolymarketActiveEvent has atomic fields — they
// init to 0; populate live values via the bba / last-trade endpoints after
// add_event lands the shared_ptr in the engine's event store.
void polymarket_event_add(const httplib::Request& req, httplib::Response& res);
void polymarket_event_remove(const httplib::Request& req,
                             httplib::Response& res);
void polymarket_event_bba(const httplib::Request& req, httplib::Response& res);
void polymarket_event_last_trade(const httplib::Request& req,
                                 httplib::Response& res);
void polymarket_orderbook_init(const httplib::Request& req,
                               httplib::Response& res);
void polymarket_orderbook_snapshot(const httplib::Request& req,
                                   httplib::Response& res);
void polymarket_orderbook_delta(const httplib::Request& req,
                                httplib::Response& res);

}  // namespace EngineControl::WriteHandlers
