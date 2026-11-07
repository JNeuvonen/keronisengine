#include "engine_control_server/server.h"

#include <utility>

#include "engine_control_server/handler_helpers.h"
#include "engine_control_server/lifecycle_handlers.h"
#include "engine_control_server/read_handlers.h"
#include "engine_control_server/strategy_handlers.h"
#include "engine_control_server/trigger_handlers.h"
#include "engine_control_server/write_handlers.h"

namespace EngineControl {

Server::Server(std::string bind_address, int port, bool mutations_enabled,
               std::chrono::steady_clock::time_point start_time)
    : ServerCommon::HttplibServerBase(std::move(bind_address), port),
      mutations_enabled_(mutations_enabled),
      start_time_(start_time) {
    register_handlers();
}

void Server::register_mutating(
    const std::string& path,
    std::function<void(const httplib::Request&, httplib::Response&)> real) {
    if (mutations_enabled_) {
        server_.Post(path, std::move(real));
    } else {
        server_.Post(path,
                     [](const httplib::Request&, httplib::Response& res) {
                         set_error(
                             res, 403,
                             "mutations disabled — set "
                             "ENABLE_ENGINE_CONTROL_MUTATIONS=1");
                     });
    }
}

void Server::register_handlers() {
    // ── Read endpoints (Phase A) ───────────────────────────────────────────
    server_.Get("/control/health",
                [this](const httplib::Request&, httplib::Response& res) {
                    ReadHandlers::health(res, start_time_, mutations_enabled_);
                });
    server_.Get("/control/engine/orders",
                [](const httplib::Request& req, httplib::Response& res) {
                    ReadHandlers::orders(req, res);
                });
    server_.Get("/control/engine/orders/unacked",
                [](const httplib::Request&, httplib::Response& res) {
                    ReadHandlers::orders_unacked(res);
                });
    server_.Get("/control/engine/orders/failed-cancels",
                [](const httplib::Request&, httplib::Response& res) {
                    ReadHandlers::orders_failed_cancels(res);
                });
    server_.Get("/control/engine/trades",
                [](const httplib::Request&, httplib::Response& res) {
                    ReadHandlers::trades(res);
                });
    server_.Get("/control/engine/strategies",
                [](const httplib::Request&, httplib::Response& res) {
                    ReadHandlers::strategies(res);
                });
    server_.Get("/control/engine/strategy-telemetry",
                [](const httplib::Request&, httplib::Response& res) {
                    ReadHandlers::strategy_telemetry(res);
                });
    server_.Get("/control/engine/exposure",
                [](const httplib::Request&, httplib::Response& res) {
                    ReadHandlers::exposure(res);
                });
    server_.Get("/control/engine/inventory",
                [](const httplib::Request& req, httplib::Response& res) {
                    ReadHandlers::inventory(req, res);
                });
    server_.Get("/control/engine/full-account-state",
                [](const httplib::Request& req, httplib::Response& res) {
                    ReadHandlers::full_account_state(req, res);
                });
    server_.Get("/control/engine/bba",
                [](const httplib::Request& req, httplib::Response& res) {
                    ReadHandlers::bba(req, res);
                });
    server_.Get("/control/engine/orderbook",
                [](const httplib::Request& req, httplib::Response& res) {
                    ReadHandlers::orderbook(req, res);
                });
    server_.Get("/control/engine/price",
                [](const httplib::Request& req, httplib::Response& res) {
                    ReadHandlers::price(req, res);
                });
    server_.Get("/control/engine/spread",
                [](const httplib::Request& req, httplib::Response& res) {
                    ReadHandlers::spread(req, res);
                });
    server_.Get("/control/engine/state-snapshot",
                [](const httplib::Request&, httplib::Response& res) {
                    ReadHandlers::state_snapshot(res);
                });
    server_.Get("/control/engine/orderbooks-out-of-sync",
                [](const httplib::Request&, httplib::Response& res) {
                    ReadHandlers::orderbooks_out_of_sync(res);
                });
    server_.Get("/control/engine/counters",
                [](const httplib::Request&, httplib::Response& res) {
                    ReadHandlers::counters(res);
                });
    server_.Get("/control/engine/markouts/pending",
                [](const httplib::Request&, httplib::Response& res) {
                    ReadHandlers::markouts_pending(res);
                });
    server_.Get("/control/engine/orders/misc/tracked",
                [](const httplib::Request&, httplib::Response& res) {
                    ReadHandlers::misc_tracked_orders(res);
                });
    server_.Get("/control/engine/borrowable-inventory",
                [](const httplib::Request& req, httplib::Response& res) {
                    ReadHandlers::borrowable_inventory(req, res);
                });
    server_.Get("/control/engine/exchange-info",
                [](const httplib::Request& req, httplib::Response& res) {
                    ReadHandlers::exchange_info(req, res);
                });
    server_.Get("/control/engine/time",
                [](const httplib::Request&, httplib::Response& res) {
                    ReadHandlers::engine_time(res);
                });
    server_.Get("/control/engine/polymarket/event",
                [](const httplib::Request& req, httplib::Response& res) {
                    ReadHandlers::polymarket_event(req, res);
                });
    server_.Get("/control/engine/polymarket/orderbook/bba",
                [](const httplib::Request& req, httplib::Response& res) {
                    ReadHandlers::polymarket_orderbook_bba(req, res);
                });

    // ── Write endpoints (Phase B.1) — order lifecycle ──────────────────────
    register_mutating("/control/engine/orders/created-ack",
                      WriteHandlers::order_created_ack);
    register_mutating("/control/engine/orders/filled-ack",
                      WriteHandlers::order_filled_ack);
    register_mutating("/control/engine/orders/canceled-ack",
                      WriteHandlers::order_canceled_ack);
    register_mutating("/control/engine/orders/update",
                      WriteHandlers::update_order);
    register_mutating("/control/engine/orders/misc/track",
                      WriteHandlers::misc_track);
    register_mutating("/control/engine/orders/misc/untrack",
                      WriteHandlers::misc_untrack);

    // ── Write endpoints (Phase B.1) — market data injection ────────────────
    register_mutating("/control/engine/bba", WriteHandlers::update_bba);
    register_mutating("/control/engine/price", WriteHandlers::update_price);
    register_mutating("/control/engine/orderbook/init",
                      WriteHandlers::init_orderbook);
    register_mutating("/control/engine/orderbook/full-refresh",
                      WriteHandlers::apply_full_refresh_orderbook);
    register_mutating("/control/engine/orderbook/update",
                      WriteHandlers::apply_orderbook_update);
    register_mutating("/control/engine/orderbook/levels-only",
                      WriteHandlers::apply_orderbook_levels_only);

    // ── Write endpoints (Phase B.1) — inventory + balances ─────────────────
    register_mutating("/control/engine/inventory",
                      WriteHandlers::update_inventory);
    register_mutating("/control/engine/quote-asset",
                      WriteHandlers::update_quote_asset);
    register_mutating("/control/engine/borrowable-inventory",
                      WriteHandlers::update_borrowable_inventory);
    register_mutating("/control/engine/inventory-and-quote-bulk",
                      WriteHandlers::update_inventory_and_quote_assets);

    // ── Write endpoints (Phase B.1) — trade management ─────────────────────
    register_mutating("/control/engine/trades/inventory-mismatch-remove",
                      WriteHandlers::inventory_mismatch_remove_trade);
    register_mutating("/control/engine/trades/manual-remove",
                      WriteHandlers::manually_remove_trade);

    // ── Trigger endpoints (Phase B.1) — virtual-clock replacement ──────────
    register_mutating("/control/engine/triggers/run-periodic-calls",
                      TriggerHandlers::run_periodic_calls);
    register_mutating("/control/engine/triggers/poll-strategy-states",
                      TriggerHandlers::poll_strategy_states);
    register_mutating("/control/engine/triggers/poll-realtime-subscriptions",
                      TriggerHandlers::poll_realtime_subscriptions);
    register_mutating("/control/engine/triggers/process-engine-signals",
                      TriggerHandlers::process_engine_signals);
    register_mutating("/control/engine/triggers/generate-predictions",
                      TriggerHandlers::generate_predictions);
    register_mutating("/control/engine/triggers/sync-persistent-state",
                      TriggerHandlers::sync_persistent_state_to_disk);
    register_mutating("/control/engine/triggers/flush-trading-logs",
                      TriggerHandlers::flush_trading_logs);
    register_mutating("/control/engine/triggers/emit-state-snapshot",
                      TriggerHandlers::emit_state_snapshot);

    // ── Write endpoints (Phase B.2) — MarketEvent + Polymarket ─────────────
    register_mutating("/control/engine/market-event",
                      WriteHandlers::market_event);
    register_mutating("/control/engine/trades/funding-payment",
                      WriteHandlers::funding_payment);
    register_mutating("/control/engine/polymarket/event",
                      WriteHandlers::polymarket_event_add);
    register_mutating("/control/engine/polymarket/event/remove",
                      WriteHandlers::polymarket_event_remove);
    register_mutating("/control/engine/polymarket/bba",
                      WriteHandlers::polymarket_event_bba);
    register_mutating("/control/engine/polymarket/last-trade",
                      WriteHandlers::polymarket_event_last_trade);
    register_mutating("/control/engine/polymarket/orderbook/init",
                      WriteHandlers::polymarket_orderbook_init);
    register_mutating("/control/engine/polymarket/orderbook/snapshot",
                      WriteHandlers::polymarket_orderbook_snapshot);
    register_mutating("/control/engine/polymarket/orderbook/delta",
                      WriteHandlers::polymarket_orderbook_delta);

    // ── Strategy reads (Phase C) ───────────────────────────────────────────
    server_.Get("/control/strategy/orders",
                [](const httplib::Request& req, httplib::Response& res) {
                    StrategyHandlers::orders(req, res);
                });
    server_.Get("/control/strategy/trades",
                [](const httplib::Request& req, httplib::Response& res) {
                    StrategyHandlers::trades(req, res);
                });
    server_.Get("/control/strategy/virtual-balance",
                [](const httplib::Request& req, httplib::Response& res) {
                    StrategyHandlers::virtual_balance(req, res);
                });
    server_.Get("/control/strategy/recent-orders",
                [](const httplib::Request& req, httplib::Response& res) {
                    StrategyHandlers::recent_orders(req, res);
                });

    // ── SM reads (Phase C) ─────────────────────────────────────────────────
    server_.Get("/control/strategy/sm/sync-progress",
                [](const httplib::Request& req, httplib::Response& res) {
                    StrategyHandlers::sm_sync_progress(req, res);
                });
    server_.Get("/control/strategy/sm/features",
                [](const httplib::Request& req, httplib::Response& res) {
                    StrategyHandlers::sm_features(req, res);
                });

    // ── SM-by-name reads (Patch 5) ─────────────────────────────────────────
    // Independent of strategy attachment — query the dispatcher singleton
    // for any registered SM. Required for strategies that WATCH but don't
    // ATTACH.
    server_.Get("/control/sm/list",
                [](const httplib::Request& req, httplib::Response& res) {
                    StrategyHandlers::sm_list(req, res);
                });
    server_.Get("/control/sm/sync-progress",
                [](const httplib::Request& req, httplib::Response& res) {
                    StrategyHandlers::sm_by_name_sync_progress(req, res);
                });
    server_.Get("/control/sm/features",
                [](const httplib::Request& req, httplib::Response& res) {
                    StrategyHandlers::sm_by_name_features(req, res);
                });

    // ── SM mutating (Phase C, gated) ───────────────────────────────────────
    register_mutating("/control/strategy/sm/react",
                      StrategyHandlers::sm_react);

    // ── SM-by-name mutating (Patch 5 v2, gated) ───────────────────────────
    // Drive SM sync state for SMs no loaded strategy attaches as primary.
    register_mutating("/control/sm/react",
                      StrategyHandlers::sm_by_name_react);

    // ── Lifecycle (Phase C, gated) ─────────────────────────────────────────
    register_mutating("/control/engine/state-ready",
                      LifecycleHandlers::state_ready);
    register_mutating("/control/engine/stop-trading",
                      LifecycleHandlers::stop_trading);
    register_mutating("/control/engine/gracefully-stop",
                      LifecycleHandlers::gracefully_stop);
    register_mutating("/control/engine/reset",
                      LifecycleHandlers::reset_engine);
}

}  // namespace EngineControl
