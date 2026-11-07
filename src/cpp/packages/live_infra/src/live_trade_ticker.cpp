#include "live_infra/live_trade_ticker.h"

#include <algorithm>
#include <cctype>
#include <iostream>

#include "datetime_utils.h"
#include "latency_tracker.h"
#include "market_making/dtypes/constants.h"
#include "market_making/dtypes/order_res.h"
#include "market_making/engine.h"

namespace {
// True when a Polymarket cancel rejection means the order is no longer active
// and cannot be canceled (already canceled, filled/matched, or unknown id).
// v2 returns "the order is already canceled"; v1 returned "already canceled or
// matched". Matched case-insensitively so a server wording change can't
// silently regress cancel handling again.
bool polymarket_cancel_reason_means_gone(const std::string &error_msg) {
    std::string r = error_msg;
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return r.find("already cancel") != std::string::npos ||
           r.find("not found") != std::string::npos ||
           r.find("matched") != std::string::npos;
}

// Structured log for an order dropped before it was ever sent to the venue
// (its send deadline elapsed, often during signing latency). Otherwise this
// state is silent; logging it lets diagnostics measure pre-send drop rate.
void log_order_dropped_pre_send(Core::Engine &engine, const Core::Order &order,
                                int strategy_id, const char *reason) {
    nlohmann::json md = {
        {"order_id", order.id},
        {"exchange_order_id", order.exchange_order_id},
        {"reason", reason},
        {"is_prediction_market_order", order.is_prediction_market_order}};
    engine.create_system_log(order.symbol, Core::LOG_LEVEL_WARNING,
                             std::string("[order_dropped_pre_send]: ") + reason,
                             Core::TRADING_LOG_EVENT_TYPE_ORDER_REJECTED, md,
                             strategy_id, order.exchange_name,
                             order.market_type, true);
}
}  // namespace

Live::LiveTradeTicker::LiveTradeTicker(
    std::vector<Core::ExchangeAndMarket> exchange_and_markets,
    std::string symbol, std::string quote_asset, std::string base_asset,
    int strategy_id, Live::StateUpdater &core,
    OrderFrequencyLimits order_frequency_limits)
    : TradeTicker(exchange_and_markets, symbol, quote_asset, base_asset,
                  strategy_id),
      core(core),
      order_frequency_limits(order_frequency_limits) {}

void Live::LiveTradeTicker::handle_polymarket_order_result(
    Core::Engine &engine, Core::Order &order,
    const Core::PolymarketBulkOrderResult &result, long long now_ms) {
    // result.order_id carries the id polymarket assigned (read from the submit
    // response). This is the only place exchange_order_id is set — never the
    // locally-computed hash, which diverges from polymarket's id post-v2.
    this->set_order_exchange_id(order.id, result.order_id, result.success);

    if (!result.success) {
        std::cout << "POLYMARKET ORDER FAILED: " << result.error_msg
                  << std::endl;
        nlohmann::json meta;
        meta["order_id"] = order.id;
        meta["id_str"] = order.id_str;
        meta["exchange_order_id"] = result.order_id;
        meta["order"] = order.to_json();
        meta["res"] = {{"order_id", result.order_id},
                       {"error_msg", result.error_msg},
                       {"status", result.status}};
        const long long end_ms = this->event_end_time_ms.load();
        if (end_ms > 0) meta["ms_to_resolution"] = end_ms - now_ms;
        // Distinct create-failure event (not ORDER_CANCELLED) so post failures
        // don't pollute cancel queries; tagged with venue for slicing.
        engine.create_system_log(
            order.symbol, Core::LOG_LEVEL_WARNING,
            "[polymarket_order_failed]: " + result.error_msg,
            Core::TRADING_LOG_EVENT_TYPE_ORDER_CREATE_FAILED_MSG, meta,
            this->strategy_id, order.exchange_name, order.market_type, true);
        this->on_order_cancel_success(engine, order);
        return;
    }

    engine.order_created_ack(order);

    {
        long long now = DatetimeUtils::get_current_time_ms();
        nlohmann::json meta;
        meta["order_id"] = order.id;
        meta["id_str"] = order.id_str;
        meta["exchange_order_id"] = result.order_id;
        meta["order"] = order.to_json();
        meta["res"] = {{"order_id", result.order_id},
                       {"status", result.status},
                       {"taking_amount", result.taking_amount},
                       {"making_amount", result.making_amount}};
        long long latency = now - order.create_timestamp;

        // static LatencyTracker order_latency_tracker("Latency", 5);
        // order_latency_tracker.track(latency);

        meta["creation_to_res_latency"] = latency;
        const long long end_ms = this->event_end_time_ms.load();
        if (end_ms > 0) meta["ms_to_resolution"] = end_ms - now;
        engine.create_system_log(order.symbol, Core::LOG_LEVEL_INFO,
                                 "[polymarket_order_created]: " + result.status,
                                 Core::TRADING_LOG_EVENT_TYPE_ORDER_CREATED,
                                 meta, this->strategy_id, order.exchange_name,
                                 order.market_type, true);
    }

    if (result.status == "matched") {
        // taking/making are the matched maker/taker legs in contract & USDC
        // units (not micro). A digital trades at price in (0,1], so the USDC
        // leg (= contracts * price) never exceeds the contract leg — the
        // matched contract count is the larger of the two, side-independent and
        // partial-safe. matched may be partial: book the matched leg and leave
        // any remainder resting (status open), never assume a full fill.
        std::cout << "POLYMARKET_ORDER_MATCH" << std::endl;
        const mpfr::mpreal filled_qty =
            std::max(mpfr::mpreal(result.taking_amount),
                     mpfr::mpreal(result.making_amount));

        Core::OrderResNormalized order_res;
        order_res.symbol = order.symbol;
        order_res.client_order_id = order.client_order_id;
        order_res.exchange_order_id = result.order_id;
        order_res.price = order.price;
        order_res.orig_quantity = order.quantity;
        order_res.filled_quantity = filled_qty;
        order_res.status = filled_qty >= order.quantity - MPREAL_EPSILON_E_6
                               ? Core::ORDER_STATUS_NOT_WORKING
                               : Core::ORDER_STATUS_WORKING;
        order_res.timestamp = now_ms;
        order_res.last_updated_timestamp = now_ms;
        order_res.last_trade_timestamp = now_ms;
        order_res.is_maker = false;
        core.handle_order_state_check(order, order_res);
    }
}

void Live::LiveTradeTicker::create_polymarket_order_via_proxy(
    Core::Engine &engine, Core::Order &order) {
    const auto now_ms = DatetimeUtils::get_current_time_ms();
    auto res_json = core.create_order(order);

    Core::PolymarketBulkOrderResult result;
    if (res_json.has_value()) {
        try {
            const auto &j = res_json.value();
            result.success = j.value("success", false);
            result.order_id = j.value("orderID", "");
            result.error_msg = j.value("errorMsg", "");
            result.status = j.value("status", "");
            result.taking_amount = j.value("takingAmount", "");
            result.making_amount = j.value("makingAmount", "");
        } catch (const nlohmann::json::exception &je) {
            result.error_msg =
                "proxy response parse failed: " + std::string(je.what());
        } catch (const std::exception &e) {
            result.error_msg =
                "proxy response parse failed: " + std::string(e.what());
        }
    } else {
        result.error_msg = "proxy request failed";
    }

    if (!result.order_id.empty()) {
        order.exchange_order_id = result.order_id;
    }
    handle_polymarket_order_result(engine, order, result, now_ms);
}

void Live::LiveTradeTicker::create_polymarket_orders_bulk_via_proxy(
    Core::Engine &engine, std::vector<Core::Order *> &valid_orders,
    long long now_ms) {
    std::vector<Core::Order> order_copies;
    order_copies.reserve(valid_orders.size());
    for (auto *o : valid_orders) {
        order_copies.push_back(*o);
    }

    auto results = core.create_polymarket_orders_bulk(order_copies);

    for (size_t j = 0; j < valid_orders.size(); j++) {
        if (j < results.size()) {
            if (!results[j].order_id.empty()) {
                valid_orders[j]->exchange_order_id = results[j].order_id;
            }
            handle_polymarket_order_result(engine, *valid_orders[j], results[j],
                                           now_ms);
        } else {
            Core::PolymarketBulkOrderResult fail_result;
            fail_result.error_msg = "no response for order in bulk proxy";
            handle_polymarket_order_result(engine, *valid_orders[j],
                                           fail_result, now_ms);
        }
    }
}

void Live::LiveTradeTicker::handle_ccxt_order_result(
    Core::Engine &engine, Core::Order &order,
    const Core::OrderCreationResult &result) {
    if (!result.success) {
        nlohmann::json meta;
        meta["order_id"] = order.id;
        meta["id_str"] = order.id_str;
        meta["order"] = order.to_json();
        meta["res"] = {{"error_msg", result.error_msg}};
        engine.create_system_log(order.symbol, Core::LOG_LEVEL_WARNING,
                                 "[ccxt_order_failed]: " + result.error_msg,
                                 Core::TRADING_LOG_EVENT_TYPE_ORDER_CANCELLED,
                                 meta, this->strategy_id, "", "", true);
        // No order resting at venue — remove from in-engine state so the
        // next strategy tick can repost.
        this->on_order_cancel_success(engine, order);
        return;
    }

    engine.order_created_ack(order);

    // exchange_order_id intentionally NOT set from the sync response — the
    // user-data WS stream is the canonical population path for binance/ccxt
    // and has been throughout the live history. Duplicating that work from
    // the create-response could conflict with engine state-routing
    // invariants we don't currently understand. Sync response is used only
    // for success/failure dispatch + ack.
    nlohmann::json meta;
    meta["order_id"] = order.id;
    meta["id_str"] = order.id_str;
    meta["order"] = order.to_json();
    meta["creation_to_res_latency"] =
        DatetimeUtils::get_current_time_ms() - order.create_timestamp;
    engine.create_system_log(order.symbol, Core::LOG_LEVEL_INFO,
                             "[ccxt_order_created]",
                             Core::TRADING_LOG_EVENT_TYPE_ORDER_CREATED, meta,
                             this->strategy_id, "", "", true);
}

void Live::LiveTradeTicker::on_create_order_impl(Core::Engine &engine,
                                                 Core::Order &order) {
    if (order.sending_is_valid_for_deadline > 0 &&
        DatetimeUtils::get_current_time_ms() >
            order.sending_is_valid_for_deadline) {
        log_order_dropped_pre_send(engine, order, this->strategy_id,
                                   "sending_deadline_exceeded");
        this->on_order_cancel_success(engine, order, true);
        return;
    }

    if (core.is_paper_strategy(this->strategy_id)) {
        handle_ccxt_order_result(engine, order, {.success = true});
        return;
    }

    if (!order.is_prediction_market_order) {
        if (order.exchange_enum == Core::Exchange::ASTER &&
            core.is_aster_direct_signing_enabled()) {
            const auto now_ms = DatetimeUtils::get_current_time_ms();
            std::vector<Core::Order> order_copies = {order};
            auto results = core.create_aster_orders_bulk(order_copies);
            if (!results.empty()) {
                handle_aster_order_result(engine, order, results[0], now_ms);
            } else {
                Core::PolymarketBulkOrderResult fail;
                fail.error_msg = "empty result from aster";
                handle_aster_order_result(engine, order, fail, now_ms);
            }
            return;
        }

        if (order.exchange_enum == Core::Exchange::HYPERLIQUID &&
            core.is_hyperliquid_direct_signing_enabled()) {
            const auto now_ms = DatetimeUtils::get_current_time_ms();
            std::vector<Core::Order> order_copies = {order};
            auto results = core.create_hyperliquid_orders_bulk(order_copies);
            if (!results.empty()) {
                handle_hyperliquid_order_result(engine, order, results[0],
                                                now_ms);
            } else {
                Core::PolymarketBulkOrderResult fail;
                fail.error_msg = "empty result from hyperliquid";
                handle_hyperliquid_order_result(engine, order, fail, now_ms);
            }
            return;
        }
        // Generic ccxt path (binance, bybit, gateio, ...) — synchronous: wait
        // for the proxy result, dispatch on success/failure.
        const auto res_json = core.create_order(order);
        Core::OrderCreationResult result;
        if (res_json.has_value()) {
            const auto &j = res_json.value();
            try {
                result.success = j.value("success", false);
                result.exchange_order_id = j.value("exchange_order_id", "");
                result.error_msg = j.value("error_msg", "");
            } catch (const std::exception &e) {
                result.error_msg =
                    "proxy response parse failed: " + std::string(e.what());
            }
        } else {
            result.error_msg = "proxy request failed";
        }
        handle_ccxt_order_result(engine, order, result);
        return;
    }

    if (core.is_polymarket_direct_signing_enabled()) {
        // Direct signing path: sign locally, submit directly
        const auto now_ms = DatetimeUtils::get_current_time_ms();
        std::vector<Core::Order> order_copies = {order};

        auto signed_bodies = core.sign_polymarket_orders(order_copies);
        if (signed_bodies.empty() || signed_bodies[0].order_id.empty()) {
            Core::PolymarketBulkOrderResult fail_result;
            fail_result.error_msg = "signing failed";
            handle_polymarket_order_result(engine, order, fail_result, now_ms);
            return;
        }

        // exchange_order_id is set only from the submit response (the id
        // polymarket actually assigns), never the locally-computed hash — the
        // two diverge after the v2/pUSD schema change and a mismatched id makes
        // orders uncancellable.

        // Re-check deadline after signing (resolve_token_options may have
        // added latency via HTTP calls on cache miss)
        if (order.sending_is_valid_for_deadline > 0 &&
            DatetimeUtils::get_current_time_ms() >
                order.sending_is_valid_for_deadline) {
            log_order_dropped_pre_send(engine, order, this->strategy_id,
                                       "post_sign_deadline_exceeded");
            this->on_order_cancel_success(engine, order, true);
            return;
        }

        auto result = core.submit_signed_polymarket_order(signed_bodies[0],
                                                          order.account_id);
        handle_polymarket_order_result(engine, order, result, now_ms);
    } else {
        create_polymarket_order_via_proxy(engine, order);
    }
}

void Live::LiveTradeTicker::on_create_orders_bulk(
    Core::Engine &engine, std::vector<Core::Order *> &orders) {
    const auto now_ms = DatetimeUtils::get_current_time_ms();

    std::vector<Core::Order *> valid_orders;
    valid_orders.reserve(orders.size());
    for (auto *o : orders) {
        if (o->sending_is_valid_for_deadline > 0 &&
            now_ms > o->sending_is_valid_for_deadline) {
            log_order_dropped_pre_send(engine, *o, this->strategy_id,
                                       "sending_deadline_exceeded");
            this->on_order_cancel_success(engine, *o, true);
            continue;
        }
        valid_orders.push_back(o);
    }

    if (valid_orders.empty()) return;

    if (core.is_paper_strategy(this->strategy_id)) {
        for (auto *o : valid_orders) {
            handle_ccxt_order_result(
                engine, *o,
                {.success = false,
                 .error_msg = "paper trading supports single ccxt orders only"});
        }
        return;
    }

    // Route by exchange
    if (valid_orders.front()->exchange_enum == Core::Exchange::ASTER) {
        create_aster_orders_bulk(engine, valid_orders, now_ms);
        return;
    }

    if (valid_orders.front()->exchange_enum == Core::Exchange::HYPERLIQUID) {
        create_hyperliquid_orders_bulk(engine, valid_orders, now_ms);
        return;
    }

    if (core.is_polymarket_direct_signing_enabled()) {
        // Direct signing path
        std::vector<Core::Order> order_copies;
        order_copies.reserve(valid_orders.size());
        for (auto *o : valid_orders) {
            order_copies.push_back(*o);
        }

        auto signed_bodies = core.sign_polymarket_orders(order_copies);
        if (signed_bodies.empty()) {
            for (size_t i = 0; i < valid_orders.size(); i++) {
                Core::PolymarketBulkOrderResult fail_result;
                fail_result.error_msg = "signing failed";
                fail_result.success = false;
                handle_polymarket_order_result(engine, *valid_orders[i],
                                               fail_result, now_ms);
            }
            return;
        }

        // Re-check deadlines after signing (resolve_token_options may have
        // added latency via HTTP calls). exchange_order_id is set later, only
        // from the submit response — never the locally-computed hash.
        const auto post_sign_ms = DatetimeUtils::get_current_time_ms();
        std::vector<size_t> still_valid_indices;
        for (size_t j = 0; j < signed_bodies.size() && j < valid_orders.size();
             j++) {
            if (valid_orders[j]->sending_is_valid_for_deadline > 0 &&
                post_sign_ms > valid_orders[j]->sending_is_valid_for_deadline) {
                log_order_dropped_pre_send(engine, *valid_orders[j],
                                           this->strategy_id,
                                           "post_sign_deadline_exceeded");
                this->on_order_cancel_success(engine, *valid_orders[j], true);
            } else {
                still_valid_indices.push_back(j);
            }
        }

        if (still_valid_indices.empty()) return;

        // Build filtered signed bodies for submission
        std::vector<PolymarketDirect::PolymarketClient::SignedOrderBody>
            filtered_bodies;
        filtered_bodies.reserve(still_valid_indices.size());
        for (auto idx : still_valid_indices) {
            filtered_bodies.push_back(std::move(signed_bodies[idx]));
        }

        long long account_id = valid_orders[0]->account_id;
        auto results =
            core.submit_signed_polymarket_orders(filtered_bodies, account_id);
        for (size_t r = 0; r < still_valid_indices.size(); r++) {
            auto orig_idx = still_valid_indices[r];
            if (r < results.size()) {
                handle_polymarket_order_result(engine, *valid_orders[orig_idx],
                                               results[r], now_ms);
            } else {
                Core::PolymarketBulkOrderResult fail_result;
                fail_result.order_id =
                    valid_orders[orig_idx]->exchange_order_id;
                fail_result.error_msg = "no response for order in bulk submit";
                handle_polymarket_order_result(engine, *valid_orders[orig_idx],
                                               fail_result, now_ms);
            }
        }
    } else {
        create_polymarket_orders_bulk_via_proxy(engine, valid_orders, now_ms);
    }
}

void Live::LiveTradeTicker::on_cancel_orders_bulk(
    Core::Engine &engine, std::vector<Core::Order *> &orders) {
    if (orders.empty()) return;

    if (core.is_paper_strategy(this->strategy_id)) {
        for (auto *o : orders) this->on_order_cancel_success(engine, *o);
        return;
    }

    // Route by exchange
    if (orders.front()->exchange_enum == Core::Exchange::ASTER) {
        cancel_aster_orders_bulk(engine, orders);
        return;
    }

    if (orders.front()->exchange_enum == Core::Exchange::HYPERLIQUID) {
        cancel_hyperliquid_orders_bulk(engine, orders);
        return;
    }

    std::vector<std::string> exchange_ids;
    exchange_ids.reserve(orders.size());
    for (auto *o : orders) {
        exchange_ids.push_back(o->exchange_order_id);
    }

    long long account_id = orders.front()->account_id;
    auto results = core.cancel_polymarket_orders_bulk(exchange_ids, account_id);
    const long long cancel_now_ms = DatetimeUtils::get_current_time_ms();
    const long long end_ms = this->event_end_time_ms.load();

    for (size_t i = 0; i < orders.size(); i++) {
        if (i >= results.size()) continue;
        auto *order = orders[i];

        // Structured cancel-result log so every cancel outcome is queryable
        // (acked / already-gone / failed) with venue + resolution-boundary
        // context. cancel_requested_at_ms + ms_to_resolution let diagnostics
        // place the cancel relative to our cancel-send and the 5m boundary.
        nlohmann::json meta = {
            {"order_id", order->id},
            {"exchange_order_id", order->exchange_order_id},
            {"price", order->price.toDouble()},
            {"cancel_requested_at_ms", order->cancel_timestamp},
            {"cancel_acked_at_ms", cancel_now_ms},
        };
        if (end_ms > 0) meta["ms_to_resolution"] = end_ms - cancel_now_ms;

        if (results[i].success) {
            meta["result"] = "acked";
            engine.create_system_log(
                order->symbol, Core::LOG_LEVEL_INFO,
                "[polymarket_cancel_acked]",
                Core::TRADING_LOG_EVENT_TYPE_ORDER_CANCELLED, meta,
                this->strategy_id, order->exchange_name, order->market_type,
                true);
            this->on_order_cancel_success(engine, *order);
        } else if (polymarket_cancel_reason_means_gone(results[i].error_msg)) {
            // Order already gone exchange-side (canceled at resolution/halt or
            // matched). Benign, but logged distinctly so we can tell it apart
            // from a genuine cancel rejection and correlate with fills.
            meta["result"] = "already_gone";
            meta["error_msg"] = results[i].error_msg;
            engine.create_system_log(
                order->symbol, Core::LOG_LEVEL_WARNING,
                "[polymarket_cancel_already_gone]: " + results[i].error_msg,
                Core::TRADING_LOG_EVENT_TYPE_ORDER_CANCELLED, meta,
                this->strategy_id, order->exchange_name, order->market_type,
                true);
            this->on_order_cancel_success(engine, *order);
        } else {
            meta["result"] = "failed";
            meta["error_msg"] = results[i].error_msg;
            engine.create_system_log(
                order->symbol, Core::LOG_LEVEL_ERROR,
                "[polymarket_cancel_failed]: " + results[i].error_msg,
                Core::TRADING_LOG_EVENT_TYPE_ORDER_CANCEL_FAILED_MSG, meta,
                this->strategy_id, order->exchange_name, order->market_type,
                true);
            this->on_order_cancel_success(engine, *order);
        }
    }
}

void Live::LiveTradeTicker::on_cancel_order_impl(Core::Engine &engine,
                                                 Core::Order &order) {
    if (core.is_paper_strategy(this->strategy_id)) {
        this->on_order_cancel_success(engine, order);
        return;
    }
    if (order.exchange_enum == Core::Exchange::ASTER) {
        std::vector<Core::Order *> orders_vec = {&order};
        cancel_aster_orders_bulk(engine, orders_vec);
        return;
    }
    if (order.exchange_enum == Core::Exchange::HYPERLIQUID) {
        std::vector<Core::Order *> orders_vec = {&order};
        cancel_hyperliquid_orders_bulk(engine, orders_vec);
        return;
    }
    // Binance / ccxt: act on the synchronous cancel round-trip rather than
    // waiting on the user-data WS. Resolve engine state CASE-BY-CASE — only
    // known-safe outcomes drop the order; unknown errors leave it (it may still
    // be live) for the order-state watchdog / WS to reconcile.
    const auto res = core.cancel_order(order);
    const std::string st = res.has_value() ? res->value("status", "") : "";
    // is_cancel_ready_to_be_sent only lets a binance cancel go out once the
    // create is acked, so a "gone" (-2011) here can't be a create/cancel race —
    // it reliably means the order is genuinely gone (cancelled / filled /
    // already cancelled). Either way, remove it.
    if (st == "cancelled" || st == "gone") {
        this->on_order_cancel_success(engine, order);
        return;
    }
    // Any other (unexpected) error: leave the order in state for the order-state
    // watchdog to reconcile, logged so it's queryable.
    nlohmann::json meta = {{"order_id", order.id},
                           {"client_order_id", order.client_order_id},
                           {"status", st}};
    if (res.has_value()) meta["error_msg"] = res->value("error_msg", "");
    engine.create_system_log(
        order.symbol, Core::LOG_LEVEL_WARNING, "[ccxt_cancel_unresolved]",
        Core::TRADING_LOG_EVENT_TYPE_ORDER_CANCEL_FAILED_MSG, meta,
        this->strategy_id, order.exchange_name, order.market_type, true);
}

// ── Aster bulk order handling ────────────────────────────────────────────

void Live::LiveTradeTicker::handle_aster_order_result(
    Core::Engine &engine, Core::Order &order,
    const Core::PolymarketBulkOrderResult &result, long long now_ms) {
    if (!result.order_id.empty()) {
        order.exchange_order_id = result.order_id;
        this->set_order_exchange_id(order.id, result.order_id, result.success);
    }

    if (!result.success) {
        nlohmann::json meta;
        meta["order_id"] = order.id;
        meta["id_str"] = order.id_str;
        meta["exchange_order_id"] = result.order_id;
        meta["order"] = order.to_json();
        meta["res"] = {{"order_id", result.order_id},
                       {"error_msg", result.error_msg}};
        engine.create_system_log(order.symbol, Core::LOG_LEVEL_WARNING,
                                 "[aster_order_failed]: " + result.error_msg,
                                 Core::TRADING_LOG_EVENT_TYPE_ORDER_CANCELLED,
                                 meta, this->strategy_id, "", "", true);
        this->on_order_cancel_success(engine, order);
        return;
    }

    engine.order_created_ack(order);

    nlohmann::json meta;
    meta["order_id"] = order.id;
    meta["id_str"] = order.id_str;
    meta["exchange_order_id"] = result.order_id;
    meta["order"] = order.to_json();
    meta["res"] = {{"order_id", result.order_id}};
    meta["creation_to_res_latency"] =
        DatetimeUtils::get_current_time_ms() - order.create_timestamp;
    engine.create_system_log(order.symbol, Core::LOG_LEVEL_INFO,
                             "[aster_order_created]",
                             Core::TRADING_LOG_EVENT_TYPE_ORDER_CREATED, meta,
                             this->strategy_id, "", "", true);
}

void Live::LiveTradeTicker::create_aster_orders_bulk(
    Core::Engine &engine, std::vector<Core::Order *> &valid_orders,
    long long now_ms) {
    std::vector<Core::Order> order_copies;
    order_copies.reserve(valid_orders.size());
    for (auto *o : valid_orders) {
        order_copies.push_back(*o);
    }

    auto results = core.create_aster_orders_bulk(order_copies);

    for (size_t j = 0; j < valid_orders.size(); j++) {
        if (j < results.size()) {
            handle_aster_order_result(engine, *valid_orders[j], results[j],
                                      now_ms);
        } else {
            Core::PolymarketBulkOrderResult fail_result;
            fail_result.error_msg = "no response for order in aster bulk";
            handle_aster_order_result(engine, *valid_orders[j], fail_result,
                                      now_ms);
        }
    }
}

void Live::LiveTradeTicker::cancel_aster_orders_bulk(
    Core::Engine &engine, std::vector<Core::Order *> &orders) {
    if (orders.empty()) return;

    std::vector<std::string> order_ids;
    order_ids.reserve(orders.size());
    for (auto *o : orders) {
        order_ids.push_back(o->exchange_order_id);
    }

    long long account_id = orders.front()->account_id;
    std::string symbol = orders.front()->symbol;
    auto results = core.cancel_aster_orders_bulk(order_ids, account_id, symbol);

    if (results.size() != orders.size()) {
        std::cout << "CANCEL RESULT SIZE MISMATCH (aster): expected="
                  << orders.size() << " got=" << results.size()
                  << " — unmatched orders left in pending-cancel for "
                     "verify_aster_order_states to reconcile"
                  << std::endl;
    }

    for (size_t i = 0; i < orders.size(); i++) {
        if (i >= results.size()) continue;
        if (results[i].success) {
            this->on_order_cancel_success(engine, *orders[i]);
        } else {
            std::cout << "ASTER CANCEL FAILED: " << results[i].error_msg
                      << " exchange_order_id=" << orders[i]->exchange_order_id
                      << std::endl;
        }
    }
}

// ── Hyperliquid bulk order handling ──────────────────────────────────────

void Live::LiveTradeTicker::handle_hyperliquid_order_result(
    Core::Engine &engine, Core::Order &order,
    const Core::PolymarketBulkOrderResult &result, long long now_ms) {
    if (!result.order_id.empty()) {
        order.exchange_order_id = result.order_id;
        this->set_order_exchange_id(order.id, result.order_id, result.success);
    }

    if (!result.success) {
        nlohmann::json meta;
        meta["order_id"] = order.id;
        meta["id_str"] = order.id_str;
        meta["exchange_order_id"] = result.order_id;
        meta["order"] = order.to_json();
        meta["res"] = {{"order_id", result.order_id},
                       {"error_msg", result.error_msg}};
        engine.create_system_log(
            order.symbol, Core::LOG_LEVEL_WARNING,
            "[hyperliquid_order_failed]: " + result.error_msg,
            Core::TRADING_LOG_EVENT_TYPE_ORDER_CANCELLED, meta,
            this->strategy_id, "", "", true);
        this->on_order_cancel_success(engine, order);
        return;
    }

    engine.order_created_ack(order);

    nlohmann::json meta;
    meta["order_id"] = order.id;
    meta["id_str"] = order.id_str;
    meta["exchange_order_id"] = result.order_id;
    meta["order"] = order.to_json();
    meta["res"] = {{"order_id", result.order_id}};
    meta["creation_to_res_latency"] =
        DatetimeUtils::get_current_time_ms() - order.create_timestamp;
    engine.create_system_log(order.symbol, Core::LOG_LEVEL_INFO,
                             "[hyperliquid_order_created]",
                             Core::TRADING_LOG_EVENT_TYPE_ORDER_CREATED, meta,
                             this->strategy_id, "", "", true);
}

void Live::LiveTradeTicker::create_hyperliquid_orders_bulk(
    Core::Engine &engine, std::vector<Core::Order *> &valid_orders,
    long long now_ms) {
    std::vector<Core::Order> order_copies;
    order_copies.reserve(valid_orders.size());
    for (auto *o : valid_orders) {
        order_copies.push_back(*o);
    }

    auto results = core.create_hyperliquid_orders_bulk(order_copies);

    for (size_t j = 0; j < valid_orders.size(); j++) {
        if (j < results.size()) {
            handle_hyperliquid_order_result(engine, *valid_orders[j],
                                            results[j], now_ms);
        } else {
            Core::PolymarketBulkOrderResult fail_result;
            fail_result.error_msg = "no response for order in hyperliquid bulk";
            handle_hyperliquid_order_result(engine, *valid_orders[j],
                                            fail_result, now_ms);
        }
    }
}

void Live::LiveTradeTicker::cancel_hyperliquid_orders_bulk(
    Core::Engine &engine, std::vector<Core::Order *> &orders) {
    if (orders.empty()) return;

    std::vector<std::string> order_ids;
    order_ids.reserve(orders.size());
    for (auto *o : orders) {
        order_ids.push_back(o->exchange_order_id);
    }

    long long account_id = orders.front()->account_id;
    std::string symbol = orders.front()->symbol;
    auto results =
        core.cancel_hyperliquid_orders_bulk(order_ids, account_id, symbol);

    for (size_t i = 0; i < orders.size(); i++) {
        if (i >= results.size()) continue;
        if (results[i].success) {
            this->on_order_cancel_success(engine, *orders[i]);
        } else {
            std::cout << "HYPERLIQUID CANCEL FAILED: " << results[i].error_msg
                      << " exchange_order_id=" << orders[i]->exchange_order_id
                      << std::endl;
        }
    }
}
