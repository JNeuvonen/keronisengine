#include "market_making/ticker.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

#include "common_types/constants.h"
#include "common_types/log_event_types.h"
#include "common_types/trade_ticker.h"
#include "datetime_utils.h"
#include "market_making/constants.h"
#include "market_making/dtypes/enums.h"
#include "market_making/dtypes/order_fill_info.h"
#include "market_making/engine.h"
#include "market_making/rate_limited_emitter.h"
#include "market_making/utils.h"
#include "math_utils.h"
#include "str_math.h"

namespace {
// 10s cooldown per (strategy, symbol, reason_code). A strategy that bumps
// MAX_UNACKED_ENTER_ORDERS_EXCEEDED every tick would flood the log without
// this; 10s is enough to see a sustained block pattern without burying signal.
Core::RateLimitedEmitter g_order_blocked_emitter;
constexpr long long ORDER_BLOCKED_COOLDOWN_MS = 10000;

const char *construction_result_to_reason_code(
    Core::OrderConstructionResult r) {
    switch (r) {
        case Core::STRATEGY_EXIT_RATE_LIMITED:
            return "STRATEGY_EXIT_RATE_LIMITED";
        case Core::TICKER_EXIT_RATE_LIMITED:
            return "TICKER_EXIT_RATE_LIMITED";
        case Core::STRATEGY_ENTER_RATE_LIMITED:
            return "STRATEGY_ENTER_RATE_LIMITED";
        case Core::TICKER_ENTER_RATE_LIMITED:
            return "TICKER_ENTER_RATE_LIMITED";
        case Core::FAILED_ORDER_COOLDOWN_EXIT:
            return "FAILED_ORDER_COOLDOWN_EXIT";
        case Core::FAILED_ORDER_COOLDOWN_ENTER:
            return "FAILED_ORDER_COOLDOWN_ENTER";
        case Core::INVALID_SIZING_COEFF_DELTA:
            return "INVALID_SIZING_COEFF_DELTA";
        case Core::MISSING_ORDERBOOK_PRICES:
            return "MISSING_ORDERBOOK_PRICES";
        case Core::OVERLAPPING_BID_ORDER_EXISTS:
            return "OVERLAPPING_BID_ORDER_EXISTS";
        case Core::OVERLAPPING_ASK_ORDER_EXISTS:
            return "OVERLAPPING_ASK_ORDER_EXISTS";
        case Core::MAX_NOTIONAL_RESTING_ON_BOOK_EXCEEDED:
            return "MAX_NOTIONAL_RESTING_ON_BOOK_EXCEEDED";
        case Core::MAX_UNACKED_EXIT_ORDERS_EXCEEDED:
            return "MAX_UNACKED_EXIT_ORDERS_EXCEEDED";
        case Core::MAX_UNACKED_ENTER_ORDERS_EXCEEDED:
            return "MAX_UNACKED_ENTER_ORDERS_EXCEEDED";
        case Core::POST_ONLY_VALIDATION_FAILED_SELL:
            return "POST_ONLY_VALIDATION_FAILED_SELL";
        case Core::POST_ONLY_VALIDATION_FAILED_BUY:
            return "POST_ONLY_VALIDATION_FAILED_BUY";
        case Core::MAXIMUM_SLIPPAGE_EXCEEDED_BUY:
            return "MAXIMUM_SLIPPAGE_EXCEEDED_BUY";
        case Core::MAXIMUM_SLIPPAGE_EXCEEDED_SELL:
            return "MAXIMUM_SLIPPAGE_EXCEEDED_SELL";
        case Core::PRICE_WITHIN_THRESHOLD_EXCEEDED_BUY:
            return "PRICE_WITHIN_THRESHOLD_EXCEEDED_BUY";
        case Core::PRICE_WITHIN_THRESHOLD_EXCEEDED_SELL:
            return "PRICE_WITHIN_THRESHOLD_EXCEEDED_SELL";
        case Core::MISSING_INVENTORY_OR_BORROWABLE:
            return "MISSING_INVENTORY_OR_BORROWABLE";
        case Core::RANDOMIZED_DELAY_BETWEEN_ORDERS_ACTIVE:
            return "RANDOMIZED_DELAY_BETWEEN_ORDERS_ACTIVE";
        case Core::MISSING_ACCOUNT_ID_TICKER:
            return "MISSING_ACCOUNT_ID_TICKER";
        case Core::MISSING_ACCOUNT_ID_RISK_ENGINE:
            return "MISSING_ACCOUNT_ID_RISK_ENGINE";
        case Core::STRATEGY_IN_REDUCE_ONLY_MODE:
            return "STRATEGY_IN_REDUCE_ONLY_MODE";
        case Core::ORDER_NOT_ALLOWED_TO_PASS_RISK_ENGINE:
            return "ORDER_NOT_ALLOWED_TO_PASS_RISK_ENGINE";
        case Core::MIN_NOTIONAL_FILTER_FAILED:
            return "MIN_NOTIONAL_FILTER_FAILED";
        case Core::ENGINE_CONSTRUCT_ORDER_FAILED:
            return "ENGINE_CONSTRUCT_ORDER_FAILED";
        case Core::STRATEGY_STOP_MAKING_MARKETS:
            return "STRATEGY_STOP_MAKING_MARKETS";
        case Core::VALID:
            return "VALID";
    }
    return "UNKNOWN";
}
}  // namespace

void Core::TradeTicker::set_order_exchange_id(
    unsigned long long order_id, const std::string &exchange_order_id,
    bool is_res_success) {
    std::lock_guard<std::mutex> lock(this->trading_mutex);
    auto it = std::find_if(this->orders.begin(), this->orders.end(),
                           [&](const Order &o) { return o.id == order_id; });
    if (it != this->orders.end()) {
        it->exchange_order_id = exchange_order_id;
        it->is_polymarket_order_res_success = is_res_success;
    }
}

bool Core::TradeTicker::is_cancel_ready_to_be_sent(const Order &order) const {
    if (order.is_prediction_market_order) {
        return order.is_polymarket_order_res_success;
    }
    if (order.exchange_enum == Exchange::ASTER ||
        order.exchange_enum == Exchange::HYPERLIQUID) {
        if (!order.is_order_creation_acknowledged ||
            order.exchange_order_id.empty()) {
            return false;
        }
    }
    if (order.exchange_enum == Exchange::BINANCE) {
        // Don't cancel before the create round-trip is acked — otherwise the
        // cancel races ahead of the create and binance returns -2011 ("Unknown
        // order" / not registered yet), which can orphan an order that's about
        // to go live. is_order_creation_acknowledged is set synchronously on
        // the create response (WS-independent). exchange_order_id is NOT
        // required: binance cancels by client_order_id, and exchange_order_id
        // is WS-populated and may lag. Deferred cancels are re-marked next tick.
        if (!order.is_order_creation_acknowledged) {
            return false;
        }
    }
    return true;
}

std::optional<Core::Order> Core::TradeTicker::get_order_by_id(
    const unsigned long long id) {
    auto order_it =
        std::find_if(this->orders.begin(), this->orders.end(),
                     [&](const Order &order) { return order.id == id; });
    return order_it != orders.end() ? std::make_optional(*order_it)
                                    : std::nullopt;
}

std::optional<std::reference_wrapper<Core::Trade>>
Core::TradeTicker::get_trade_by_trade_id(const unsigned long long id) {
    auto trade_it =
        std::find_if(this->active_trades.begin(), this->active_trades.end(),
                     [&](const Trade &trade) { return trade.id == id; });
    if (trade_it != this->active_trades.end()) return std::ref(*trade_it);
    return std::nullopt;
}

std::optional<std::reference_wrapper<Core::Trade>>
Core::TradeTicker::get_trade_by_enter_order_id(const unsigned long long id) {
    auto trade_it =
        std::find_if(this->active_trades.begin(), this->active_trades.end(),
                     [&](const Trade &trade) {
                         return std::find(trade.enter_order_ids.begin(),
                                          trade.enter_order_ids.end(),
                                          id) != trade.enter_order_ids.end();
                     });
    if (trade_it != this->active_trades.end()) return std::ref(*trade_it);
    return std::nullopt;
}

std::optional<std::reference_wrapper<Core::Trade>>
Core::TradeTicker::get_trade_by_signal_id(const unsigned long long signal_id,
                                          const std::string &exchange_name,
                                          const std::string &market_type,
                                          Core::TradeTypeInternal type_internal,
                                          const std::string &symbol) {
    auto trade_it =
        std::find_if(this->active_trades.begin(), this->active_trades.end(),
                     [&](const Trade &trade) {
                         return trade.signal_id == signal_id &&
                                trade.exchange_name == exchange_name &&
                                trade.market_type == market_type &&
                                trade.type_internal == type_internal &&
                                trade.symbol == symbol;
                     });
    if (trade_it != this->active_trades.end()) return std::ref(*trade_it);
    return std::nullopt;
}

std::optional<std::reference_wrapper<Core::Trade>>
Core::TradeTicker::find_active_trade_for_fill(const OrderFillInfo &fill_info,
                                              const Order &order) {
    // Per-fill trade semantics: every enter-fill creates its own Trade.
    // Used by strategies whose Python sim treats each rung fill as an
    // independent position. Aggregating by signal_id would collapse all
    // rungs of one signal into a single Trade and break TP/cover geometry
    // vs the sim the strategy was tuned against.
    if (this->is_individual_fill_individual_trade.load()) {
        return std::nullopt;
    }
    if (this->is_trading_on_prediction_market.load()) {
        if (fill_info.clob_token_id.empty()) {
            return std::nullopt;
        }
        auto it = std::find_if(
            this->active_trades.begin(), this->active_trades.end(),
            [&](const Trade &trade) {
                return trade.clob_token_id == fill_info.clob_token_id &&
                       trade.trade_creation_type ==
                           fill_info.trade_creation_type;
            });
        if (it != this->active_trades.end()) {
            return std::ref(*it);
        }
        return std::nullopt;
    }

    if (this->is_market_making_strategy.load()) {
        auto it = std::find_if(
            this->active_trades.begin(), this->active_trades.end(),
            [&](const Trade &trade) {
                return trade.exchange_name == order.exchange_name &&
                       trade.market_type == order.market_type;
            });
        if (it != this->active_trades.end()) {
            return std::ref(*it);
        }
        return std::nullopt;
    }

    return this->get_trade_by_signal_id(
        order.signal_id, order.exchange_name, order.market_type,
        order.hedge_for_trade_id.has_value()
            ? Core::TradeTypeInternal::HEDGE
            : Core::TradeTypeInternal::DIRECTIONAL_TRADE,
        order.get_potentially_alt_routed_symbol());
}

std::string Core::TradeTicker::get_order_cancel_key(const Order &order) {
    if (order.is_prediction_market_order && !order.exchange_order_id.empty()) {
        return order.exchange_order_id;
    }
    return order.client_order_id;
}

// ack_cancel merged into erase_order_from_state — single owner of all order
// removal bookkeeping

void Core::TradeTicker::ack_create_order(Engine &engine, const Order &order) {
    // Collect all the data we need while holding the lock
    std::optional<Core::Order> order_to_update;
    bool order_found = false;

    {
        std::lock_guard<std::mutex> lock(this->trading_mutex);
        auto order_it =
            std::find_if(orders.begin(), orders.end(),
                         [&](Order &o) { return o.id == order.id; });

        if (order_it == orders.end()) {
            return;
        }

        if (order_it->is_order_creation_acknowledged) {
            return;
        }

        order_found = true;
        order_it->is_order_creation_acknowledged = true;
        order_it->ack_create_timestamp = engine.get_time();
        order_to_update = *order_it;
        if (order_it->is_trade_close_order) {
            this->unacked_exit_order_count.fetch_sub(1,
                                                     std::memory_order_relaxed);
        } else {
            this->unacked_enter_order_count.fetch_sub(
                1, std::memory_order_relaxed);
        }
    }  // Lock is released here

    // Now make all engine calls outside of the ticker lock
    if (order_found && order_to_update.has_value()) {
        // ORDER_ACKNOWLEDGED is distinct from ORDER_CREATED (Phase 7D). CREATED
        // fires when the Order struct is constructed and submitted; ACK fires
        // when the venue confirms. The pair enables latency queries:
        // `ack_create_timestamp - create_timestamp` per order.
        const long long ack_latency_ms = order_to_update->ack_create_timestamp -
                                         order_to_update->create_timestamp;
        nlohmann::json md = order_to_update->to_json();
        md["ack_latency_ms"] = ack_latency_ms;
        engine.create_system_log(
            this->symbol, LOG_LEVEL_INFO, "order acknowledged",
            Core::TRADING_LOG_EVENT_TYPE_ORDER_ACKNOWLEDGED, md,
            order_to_update->strategy_id, order_to_update->exchange_name,
            order_to_update->market_type);
        engine.update_order(*order_to_update);
    }
}

void Core::TradeTicker::emit_fill_without_matching_order(
    Engine &engine, const OrderFillInfo &fill_info, const char *phase) {
    // No cooldown: every orphan fill is a potential missed pick-off, so each
    // one is logged individually. Caller holds trading_mutex, so accessing
    // `orders` / `orders_removed_from_state` is safe. Sample up to 10 ids from
    // each list to disambiguate "no orders at all" from "wrong order_id format
    // from venue" without blowing up the row.
    nlohmann::json active_ids = nlohmann::json::array();
    size_t active_sample = 0;
    for (const auto &o : this->orders) {
        if (active_sample >= 10) break;
        active_ids.push_back(std::to_string(o.id));
        ++active_sample;
    }
    nlohmann::json removed_ids = nlohmann::json::array();
    size_t removed_sample = 0;
    for (const auto &o : this->orders_removed_from_state) {
        if (removed_sample >= 10) break;
        removed_ids.push_back(std::to_string(o.id));
        ++removed_sample;
    }
    nlohmann::json md = {
        {"order_id", std::to_string(fill_info.order_id)},
        {"phase", phase},
        {"side", fill_info.side == Core::OrderSide::BUY ? "buy" : "sell"},
        {"symbol", this->symbol},
        {"exchange_name", fill_info.exchange_name},
        {"market_type", fill_info.market_type},
        {"strategy_id", fill_info.strategy_id},
        {"fill_qty", fill_info.fill_amount.toDouble()},
        {"fill_price", fill_info.price.toDouble()},
        {"searched_active_count", this->orders.size()},
        {"searched_removed_count", this->orders_removed_from_state.size()},
        {"sample_active_order_ids", active_ids},
        {"sample_removed_order_ids", removed_ids},
    };
    const long long end_ms = this->event_end_time_ms.load();
    if (end_ms > 0) {
        md["ms_to_resolution"] = end_ms - engine.get_time();
    }
    engine.create_system_log(
        this->symbol, Core::LOG_LEVEL_ERROR, "fill for unknown order",
        Core::TRADING_LOG_EVENT_TYPE_FILL_WITHOUT_MATCHING_ORDER, md,
        fill_info.strategy_id, fill_info.exchange_name, fill_info.market_type);
}

void Core::TradeTicker::erase_order_from_state(
    Engine &engine, const std::vector<Order>::iterator &order_it) {
    auto order_id = order_it->id;

    // Clear cancel ack tracking if this order has a pending cancel
    this->unacked_new_cancels.erase(get_order_cancel_key(*order_it));

    if (!order_it->is_order_creation_acknowledged) {
        if (order_it->is_trade_close_order) {
            this->unacked_exit_order_count.fetch_sub(1,
                                                     std::memory_order_relaxed);
        } else {
            this->unacked_enter_order_count.fetch_sub(
                1, std::memory_order_relaxed);
        }
    }

    // Decrement order book counts and notional
    const double remaining_qty =
        (order_it->quantity - order_it->filled_quantity).toDouble();
    const double notional = order_it->price.toDouble() * remaining_qty;
    if (order_it->side == Core::OrderSide::BUY) {
        this->bid_order_count_by_price[order_it->price.toDouble()] -= 1;
        this->active_bid_count -= 1;
        this->active_bid_notional -= notional;
        if (order_it->is_prediction_market_order &&
            !order_it->clob_id.empty()) {
            auto key =
                get_clob_price_key(order_it->clob_id, order_it->price_str);
            auto it = this->bid_order_count_by_clob_price.find(key);
            if (it != this->bid_order_count_by_clob_price.end()) {
                it->second -= 1;
                if (it->second <= 0) {
                    this->bid_order_count_by_clob_price.erase(it);
                }
            }
        }
    } else {
        this->ask_order_count_by_price[order_it->price.toDouble()] -= 1;
        this->active_ask_count -= 1;
        this->active_ask_notional -= notional;
        if (order_it->is_prediction_market_order &&
            !order_it->clob_id.empty()) {
            auto key =
                get_clob_price_key(order_it->clob_id, order_it->price_str);
            auto it = this->ask_order_count_by_clob_price.find(key);
            if (it != this->ask_order_count_by_clob_price.end()) {
                it->second -= 1;
                if (it->second <= 0) {
                    this->ask_order_count_by_clob_price.erase(it);
                }
            }
        }
    }

    this->orders.erase(order_it);
    this->order_count.fetch_sub(1, std::memory_order_relaxed);
}

void Core::TradeTicker::enter_trade_order_filled(Engine &engine,
                                                 OrderFillInfo &fill_info) {
    // Collect all the data we need while holding the lock
    std::optional<Core::Trade> trade_to_update;
    std::optional<Core::Trade> new_trade;
    std::optional<Core::Order> order_to_update;
    bool order_fully_filled = false;
    bool create_new_trade = false;

    {
        std::lock_guard<std::mutex> lock(this->trading_mutex);

        Order *order_ptr = nullptr;
        bool from_removed_state = false;

        auto order_it = std::find_if(
            orders.begin(), orders.end(),
            [&](const Order &order) { return order.id == fill_info.order_id; });

        if (order_it != orders.end()) {
            order_ptr = &(*order_it);
        } else {
            auto removed_it = std::find_if(
                orders_removed_from_state.begin(),
                orders_removed_from_state.end(), [&](const Order &order) {
                    return order.id == fill_info.order_id;
                });
            if (removed_it == orders_removed_from_state.end()) {
                this->emit_fill_without_matching_order(engine, fill_info,
                                                       "enter");
                return;
            }
            order_ptr = &(*removed_it);
            from_removed_state = true;
            fill_info.is_order_cancelled_before_fill = true;
            fill_info.order_cancel_timestamp = removed_it->cancel_timestamp;
            const long long fill_received_at_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
            nlohmann::json md;
            md["order_id"] = std::to_string(fill_info.order_id);
            md["side"] =
                fill_info.side == Core::OrderSide::BUY ? "buy" : "sell";
            md["phase"] = "enter";
            md["cancelled_at_ms"] = fill_info.order_cancel_timestamp;
            md["fill_received_at_ms"] = fill_received_at_ms;
            md["fill_delay_ms"] =
                fill_received_at_ms - fill_info.order_cancel_timestamp;
            md["fill_qty"] = fill_info.fill_amount.toDouble();
            md["fill_price"] = fill_info.price.toDouble();
            engine.create_system_log(
                this->symbol, LOG_LEVEL_WARNING,
                "Late fill on cancelled order (enter)",
                TRADING_LOG_EVENT_TYPE_LATE_FILL_ON_CANCELLED, md,
                fill_info.strategy_id, fill_info.exchange_name,
                fill_info.market_type);
        }

        Order &order_ref = *order_ptr;

        mpfr::mpreal fill_delta =
            Core::Utils::get_order_fill_size_delta(order_ref, fill_info);

        if (fill_delta <= MPREAL_ZERO) {
            return;
        }

        if (!from_removed_state) {
            const double order_price = order_ref.price.toDouble();
            const double old_remaining =
                (order_ref.quantity - order_ref.filled_quantity).toDouble();
            const double new_remaining =
                (order_ref.quantity - fill_info.fill_amount).toDouble();
            if (order_ref.side == Core::OrderSide::BUY) {
                this->active_bid_notional -= old_remaining * order_price;
                this->active_bid_notional += new_remaining * order_price;
            } else {
                this->active_ask_notional -= old_remaining * order_price;
                this->active_ask_notional += new_remaining * order_price;
            }
        }

        order_ref.filled_quantity = fill_info.fill_amount;
        order_ref.last_fill_timestamp = fill_info.order_fill_timestamp;
        order_ref.fills.push_back(fill_info);

        if (fill_info.sizing_coeff_delta == MPREAL_ZERO) {
            engine.create_system_log(
                this->symbol, LOG_LEVEL_ERROR,
                "Sizing coeff delta is zero for order id: " +
                    std::to_string(fill_info.order_id),
                Core::TRADING_LOG_EVENT_TYPE_ENGINE_STATE_MISMATCH,
                fill_info.to_json(), fill_info.strategy_id);
            return;
        }

        auto trade_optional =
            this->find_active_trade_for_fill(fill_info, order_ref);

        if (!trade_optional.has_value()) {
            create_new_trade = true;
            new_trade = Trade::from_order_fill(fill_info, fill_delta);
            active_trades.push_back(*new_trade);
            this->active_trade_count.fetch_add(1, std::memory_order_relaxed);
        } else {
            // Make a copy of the trade, modify it, then update the original
            Core::Trade updated_trade_copy = trade_optional.value().get();
            updated_trade_copy.on_enter_fill(fill_info, fill_delta);

            // Update the original trade in the vector with our changes
            trade_optional.value().get() = updated_trade_copy;

            trade_to_update = updated_trade_copy;
        }

        // Prepare order data
        order_to_update = order_ref;

        bool is_order_fully_filled = order_ref.filled_quantity >=
                                     order_ref.quantity - MPREAL_EPSILON_E_10;

        if (is_order_fully_filled) {
            order_to_update->ack_state_removal = engine.get_time();
            order_fully_filled = true;
            if (from_removed_state) {
                orders_removed_from_state.erase(
                    std::remove_if(orders_removed_from_state.begin(),
                                   orders_removed_from_state.end(),
                                   [&](const Order &o) {
                                       return o.id == fill_info.order_id;
                                   }),
                    orders_removed_from_state.end());
            } else {
                this->erase_order_from_state(engine, order_it);
            }
        }
    }  // Lock is released here

    // Now make all engine calls outside of the ticker lock
    if (create_new_trade && new_trade.has_value()) {
        engine.add_trade_to_risk_engine(*new_trade);
        engine.create_system_log(
            this->symbol, LOG_LEVEL_INFO,
            "Created new trade from: " + std::to_string(order_to_update->id),
            Core::TRADING_LOG_EVENT_TYPE_TRADE_OPENED, fill_info.to_json(),
            fill_info.strategy_id, order_to_update->exchange_name,
            order_to_update->market_type, true);
    } else if (trade_to_update.has_value()) {
        engine.create_system_log(
            this->symbol, LOG_LEVEL_INFO,
            "Adding fill to existing trade: " +
                std::to_string(fill_info.order_id),
            Core::TRADING_LOG_EVENT_TYPE_TRADE_FILL_APPENDED,
            fill_info.to_json(), fill_info.strategy_id,
            trade_to_update->exchange_name, trade_to_update->market_type, true);
        engine.update_trade_in_risk_engine(*trade_to_update);
    }

    // Order-related engine calls
    if (order_to_update.has_value()) {
        this->create_order_log(engine, *order_to_update,
                               Core::TRADING_LOG_EVENT_TYPE_ORDER_FILLED);
        engine.add_order_fill_to_logs(fill_info);
        engine.update_order(*order_to_update);
        engine.update_order_in_risk_engine(*order_to_update);
    }

    if (order_fully_filled) {
        engine.remove_order_from_risk_engine(order_to_update->id);
    }
}

void Core::TradeTicker::exit_trade_order_filled(Engine &engine,
                                                OrderFillInfo &fill_info) {
    // Collect all the data we need while holding the lock
    std::optional<Core::Trade> trade_to_update;
    std::optional<Core::Order> order_to_update;
    bool trade_completed = false;
    bool order_fully_filled = false;
    Core::Trade completed_trade;

    {
        std::lock_guard<std::mutex> lock(this->trading_mutex);

        Order *order_ptr = nullptr;
        bool from_removed_state = false;

        auto order_it = std::find_if(
            orders.begin(), orders.end(),
            [&](const Order &order) { return order.id == fill_info.order_id; });

        if (order_it != orders.end()) {
            order_ptr = &(*order_it);
        } else {
            auto removed_it = std::find_if(
                orders_removed_from_state.begin(),
                orders_removed_from_state.end(), [&](const Order &order) {
                    return order.id == fill_info.order_id;
                });
            if (removed_it == orders_removed_from_state.end()) {
                this->emit_fill_without_matching_order(engine, fill_info,
                                                       "exit");
                return;
            }
            order_ptr = &(*removed_it);
            from_removed_state = true;
            fill_info.is_order_cancelled_before_fill = true;
            fill_info.order_cancel_timestamp = removed_it->cancel_timestamp;
            const long long fill_received_at_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
            nlohmann::json md;
            md["order_id"] = std::to_string(fill_info.order_id);
            md["side"] =
                fill_info.side == Core::OrderSide::BUY ? "buy" : "sell";
            md["phase"] = "exit";
            md["cancelled_at_ms"] = fill_info.order_cancel_timestamp;
            md["fill_received_at_ms"] = fill_received_at_ms;
            md["fill_delay_ms"] =
                fill_received_at_ms - fill_info.order_cancel_timestamp;
            md["fill_qty"] = fill_info.fill_amount.toDouble();
            md["fill_price"] = fill_info.price.toDouble();
            engine.create_system_log(
                this->symbol, LOG_LEVEL_WARNING,
                "Late fill on cancelled order (exit)",
                TRADING_LOG_EVENT_TYPE_LATE_FILL_ON_CANCELLED, md,
                fill_info.strategy_id, fill_info.exchange_name,
                fill_info.market_type);
        }

        Order &order_ref = *order_ptr;

        mpfr::mpreal fill_delta =
            Core::Utils::get_order_fill_size_delta(order_ref, fill_info);

        if (fill_delta <= MPREAL_ZERO) {
            // can happen when exchange sends order fills in incorrect order
            return;
        }

        if (!from_removed_state) {
            const double order_price = order_ref.price.toDouble();
            const double old_remaining =
                (order_ref.quantity - order_ref.filled_quantity).toDouble();
            const double new_remaining =
                (order_ref.quantity - fill_info.fill_amount).toDouble();
            if (order_ref.side == Core::OrderSide::BUY) {
                this->active_bid_notional -= old_remaining * order_price;
                this->active_bid_notional += new_remaining * order_price;
            } else {
                this->active_ask_notional -= old_remaining * order_price;
                this->active_ask_notional += new_remaining * order_price;
            }
        }

        order_ref.filled_quantity = fill_info.fill_amount;
        order_ref.last_fill_timestamp = fill_info.order_fill_timestamp;
        order_ref.fills.push_back(fill_info);

        auto trade_optional =
            this->get_trade_by_trade_id(order_ref.trade_id.value());

        if (!trade_optional.has_value()) {
            engine.create_system_log(
                this->symbol, LOG_LEVEL_ERROR,
                "Trade not found in active trades vector for trade id: " +
                    std::to_string(order_ref.trade_id.value()),
                Core::TRADING_LOG_EVENT_TYPE_ENGINE_STATE_MISMATCH,
                fill_info.to_json(), fill_info.strategy_id);
            return;
        }

        // Make a copy of the trade, modify it, then update the original
        Core::Trade updated_trade_copy = trade_optional.value().get();
        updated_trade_copy.on_exit_fill(fill_info);

        // Update the original trade in the vector with our changes
        trade_optional.value().get() = updated_trade_copy;

        // Prepare data for engine updates (using our modified copy)
        order_to_update = order_ref;
        trade_to_update = updated_trade_copy;

        if (updated_trade_copy.closed_quantity >=
            updated_trade_copy.open_quantity - MPREAL_EPSILON_E_10) {
            trade_completed = true;
            completed_trade = updated_trade_copy;
            order_to_update->ack_state_removal = engine.get_time();
            this->move_trade_to_completed(completed_trade,
                                          fill_info.order_fill_timestamp);
        }

        if (order_ref.filled_quantity >=
            order_ref.quantity - MPREAL_EPSILON_E_10) {
            order_to_update->ack_state_removal = engine.get_time();
            order_fully_filled = true;
            if (from_removed_state) {
                orders_removed_from_state.erase(
                    std::remove_if(orders_removed_from_state.begin(),
                                   orders_removed_from_state.end(),
                                   [&](const Order &o) {
                                       return o.id == fill_info.order_id;
                                   }),
                    orders_removed_from_state.end());
            } else {
                this->erase_order_from_state(engine, order_it);
            }
        }
    }

    if (order_fully_filled) {
        engine.remove_order_from_risk_engine(order_to_update->id);
    }

    if (trade_completed) {
        this->finalize_completed_trade(engine, completed_trade,
                                       fill_info.strategy_id);
    } else if (trade_to_update.has_value()) {
        engine.create_system_log(
            this->symbol, LOG_LEVEL_INFO,
            "Trade on " + this->symbol + " partially closed: " +
                std::to_string(trade_to_update->closed_quantity.toDouble()) +
                "/" + std::to_string(trade_to_update->open_quantity.toDouble()),
            Core::TRADING_LOG_EVENT_TYPE_TRADE_PARTIALLY_CLOSED,
            fill_info.to_json(), fill_info.strategy_id,
            trade_to_update->exchange_name, trade_to_update->market_type);
        engine.update_trade_in_risk_engine(*trade_to_update);
    }

    // Order-related engine calls
    if (order_to_update.has_value()) {
        this->create_order_log(engine, *order_to_update,
                               Core::TRADING_LOG_EVENT_TYPE_ORDER_FILLED);
        engine.add_order_fill_to_logs(fill_info);
        engine.update_order(*order_to_update);
        engine.update_order_in_risk_engine(*order_to_update);
    }
}

void Core::TradeTicker::mm_trade_order_filled(Engine &engine,
                                              OrderFillInfo &fill_info) {
    std::optional<Core::Trade> trade_to_update;
    std::optional<Core::Trade> new_trade;
    std::optional<Core::Order> order_to_update;
    bool order_fully_filled = false;
    bool create_new_trade = false;
    bool trade_completed = false;
    Core::Trade completed_trade;
    // Residual from overshoot: e.g. long 10, sell 15 → close at 10,
    // open new short with 5.
    std::optional<Core::Trade> residual_trade;

    {
        std::lock_guard<std::mutex> lock(this->trading_mutex);

        Order *order_ptr = nullptr;
        bool from_removed_state = false;

        auto order_it = std::find_if(
            orders.begin(), orders.end(),
            [&](const Order &order) { return order.id == fill_info.order_id; });

        if (order_it != orders.end()) {
            order_ptr = &(*order_it);
        } else {
            auto removed_it = std::find_if(
                orders_removed_from_state.begin(),
                orders_removed_from_state.end(), [&](const Order &order) {
                    return order.id == fill_info.order_id;
                });
            if (removed_it == orders_removed_from_state.end()) {
                return;
            }
            order_ptr = &(*removed_it);
            from_removed_state = true;
            fill_info.is_order_cancelled_before_fill = true;
            fill_info.order_cancel_timestamp = removed_it->cancel_timestamp;
        }

        Order &order_ref = *order_ptr;

        mpfr::mpreal fill_delta =
            Core::Utils::get_order_fill_size_delta(order_ref, fill_info);

        if (fill_delta <= MPREAL_ZERO) {
            std::cout << "Fill delta is zero" << std::endl;
            return;
        }

        if (!from_removed_state) {
            const double order_price = order_ref.price.toDouble();
            const double old_remaining =
                (order_ref.quantity - order_ref.filled_quantity).toDouble();
            const double new_remaining =
                (order_ref.quantity - fill_info.fill_amount).toDouble();
            if (order_ref.side == Core::OrderSide::BUY) {
                this->active_bid_notional -= old_remaining * order_price;
                this->active_bid_notional += new_remaining * order_price;
            } else {
                this->active_ask_notional -= old_remaining * order_price;
                this->active_ask_notional += new_remaining * order_price;
            }
        }

        order_ref.filled_quantity = fill_info.fill_amount;
        order_ref.last_fill_timestamp = fill_info.order_fill_timestamp;
        order_ref.fills.push_back(fill_info);

        auto trade_optional =
            this->find_active_trade_for_fill(fill_info, order_ref);

        bool fill_is_buy = (fill_info.side == OrderSide::BUY);

        if (!trade_optional.has_value()) {
            // No active MM trade — create one from this fill
            create_new_trade = true;
            new_trade = Trade::from_order_fill(fill_info, fill_delta);
            active_trades.push_back(*new_trade);
            this->active_trade_count.fetch_add(1, std::memory_order_relaxed);
        } else {
            Core::Trade &active_trade = trade_optional.value().get();
            bool trade_is_long = (active_trade.type == TradeType::LONG);
            bool same_side = (trade_is_long && fill_is_buy) ||
                             (!trade_is_long && !fill_is_buy);

            if (same_side) {
                // Same side as trade — accumulate (enter)
                Core::Trade updated = active_trade;
                updated.on_enter_fill(fill_info, fill_delta);
                active_trade = updated;
                trade_to_update = updated;
            } else {
                // Opposite side — reduce (exit)
                mpfr::mpreal remaining =
                    active_trade.open_quantity - active_trade.closed_quantity;
                mpfr::mpreal exit_qty = std::min(fill_delta, remaining);
                mpfr::mpreal overshoot = fill_delta - exit_qty;

                // Cap the exit fill to remaining open quantity
                Core::OrderFillInfo capped_fill = fill_info;
                capped_fill.fill_amount = exit_qty;

                Core::Trade updated = active_trade;
                updated.on_exit_fill(capped_fill);
                active_trade = updated;

                if (updated.closed_quantity >=
                    updated.open_quantity - MPREAL_EPSILON_E_10) {
                    // Trade completed — round trip done
                    trade_completed = true;
                    completed_trade = updated;
                    this->move_trade_to_completed(
                        completed_trade, fill_info.order_fill_timestamp);

                    // Overshoot: residual starts a new trade on opposite side
                    if (overshoot > MPREAL_EPSILON_E_10) {
                        Core::OrderFillInfo residual_fill = fill_info;
                        residual_fill.fill_amount = overshoot;
                        residual_fill.sizing_coeff_delta = mpfr::mpreal("1.0");
                        residual_trade =
                            Trade::from_order_fill(residual_fill, overshoot);
                        active_trades.push_back(*residual_trade);
                        this->active_trade_count.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                } else {
                    trade_to_update = updated;
                }
            }
        }

        order_to_update = order_ref;

        if (order_ref.filled_quantity >=
            order_ref.quantity - MPREAL_EPSILON_E_10) {
            order_to_update->ack_state_removal = engine.get_time();
            order_fully_filled = true;
            if (from_removed_state) {
                orders_removed_from_state.erase(
                    std::remove_if(orders_removed_from_state.begin(),
                                   orders_removed_from_state.end(),
                                   [&](const Order &o) {
                                       return o.id == fill_info.order_id;
                                   }),
                    orders_removed_from_state.end());
            } else {
                this->erase_order_from_state(engine, order_it);
            }
        }
    }  // Lock released

    if (create_new_trade && new_trade.has_value()) {
        engine.add_trade_to_risk_engine(*new_trade);
    } else if (trade_completed) {
        this->finalize_completed_trade(engine, completed_trade,
                                       fill_info.strategy_id);
        if (residual_trade.has_value()) {
            engine.add_trade_to_risk_engine(*residual_trade);
        }
    } else if (trade_to_update.has_value()) {
        engine.update_trade_in_risk_engine(*trade_to_update);
    }

    if (order_to_update.has_value()) {
        this->create_order_log(engine, *order_to_update,
                               Core::TRADING_LOG_EVENT_TYPE_ORDER_FILLED);
        engine.add_order_fill_to_logs(fill_info);
        engine.update_order(*order_to_update);
        engine.update_order_in_risk_engine(*order_to_update);
    }

    if (order_fully_filled) {
        engine.remove_order_from_risk_engine(order_to_update->id);
    }
}

void Core::TradeTicker::record_fill(const OrderFillInfo &fill_info) {
    int outcome_idx = 0;  // default for crypto/stocks (single outcome)
    if (!fill_info.clob_token_id.empty()) {
        auto it = clob_id_to_outcome_index.find(fill_info.clob_token_id);
        if (it != clob_id_to_outcome_index.end()) {
            outcome_idx = it->second;
        }
    }
    recent_fills.push(FillEvent{outcome_idx, fill_info.fill_amount.toDouble(),
                                fill_info.price.toDouble(), fill_info.side,
                                fill_info.order_fill_timestamp},
                      fill_info.order_fill_timestamp);
}

void Core::TradeTicker::update_outcome_trade_flow(int outcome_index,
                                                  bool is_buy, double size,
                                                  double price, long long ts) {
    if (outcome_index < 0 || outcome_index >= MAX_POLYMARKET_OUTCOMES) return;
    auto &flow = outcome_trade_flow[outcome_index];
    if (is_buy) {
        double old = flow.buy_volume.load(std::memory_order_relaxed);
        while (!flow.buy_volume.compare_exchange_weak(
            old, old + size, std::memory_order_relaxed)) {
        }
        flow.buy_count.fetch_add(1, std::memory_order_relaxed);
    } else {
        double old = flow.sell_volume.load(std::memory_order_relaxed);
        while (!flow.sell_volume.compare_exchange_weak(
            old, old + size, std::memory_order_relaxed)) {
        }
        flow.sell_count.fetch_add(1, std::memory_order_relaxed);
    }
    flow.last_trade_price.store(price, std::memory_order_relaxed);
    flow.last_trade_ts.store(ts, std::memory_order_release);
}

std::optional<Core::TradeTicker::OutcomeTradeFlowSnapshot>
Core::TradeTicker::read_outcome_trade_flow(int outcome_index) const {
    if (outcome_index < 0 || outcome_index >= MAX_POLYMARKET_OUTCOMES)
        return std::nullopt;
    const auto &flow = outcome_trade_flow[outcome_index];
    return OutcomeTradeFlowSnapshot{
        flow.buy_volume.load(std::memory_order_relaxed),
        flow.sell_volume.load(std::memory_order_relaxed),
        flow.buy_count.load(std::memory_order_relaxed),
        flow.sell_count.load(std::memory_order_relaxed),
        flow.last_trade_price.load(std::memory_order_relaxed),
        flow.last_trade_ts.load(std::memory_order_acquire),
    };
}

void Core::TradeTicker::on_order_filled_ack(Engine &engine,
                                            OrderFillInfo &fill_info) {
    if (this->is_market_making_strategy.load()) {
        this->mm_trade_order_filled(engine, fill_info);
    } else if (!fill_info.trade_id.has_value()) {
        this->enter_trade_order_filled(engine, fill_info);
    } else {
        this->exit_trade_order_filled(engine, fill_info);
    }
    record_fill(fill_info);
}

std::vector<Core::Trade> Core::TradeTicker::get_trades(bool require_lock) {
    if (this->active_trade_count.load(std::memory_order_relaxed) == 0) {
        return {};
    }
    return this->with_lock(require_lock,
                           [this]() { return this->active_trades; });
}

void Core::TradeTicker::add_synthethic_order_for_event_creation(
    const Order &order) {
    std::lock_guard<std::mutex> lock(this->trading_mutex);
    this->orders.push_back(order);
}

std::vector<Core::Order> Core::TradeTicker::get_orders(bool require_lock) {
    if (this->order_count.load(std::memory_order_relaxed) == 0) {
        return {};
    }
    return this->with_lock(require_lock, [this]() { return this->orders; });
}

void Core::TradeTicker::create_order_log(Engine &engine, const Order &order,
                                         const std::string &event_type) {
    nlohmann::json md = order.to_json();
    // Resolution-boundary context for prediction-market order events, so every
    // create/fill/cancel can be placed relative to the 5m boundary in
    // diagnostics. Only set when this ticker has a market end time (>0).
    const long long end_ms = this->event_end_time_ms.load();
    if (end_ms > 0) {
        md["ms_to_resolution"] = end_ms - engine.get_time();
        // Self-contained settlement-lookup keys for post-hoc fill scoring:
        // condition_id + outcome_index + event window let each fill be joined
        // to its resolution (winner per condition) and bucketed by window
        // without re-deriving from external state. outcome_index/name are
        // re-derived from the order's clob_id so they are always populated
        // (order.to_json's outcome_name can be blank on close/synthetic legs).
        md["condition_id"] = this->condition_id;
        md["event_start_time_ms"] = this->event_start_time_ms.load();
        md["event_end_time_ms"] = end_ms;
        auto oit = this->clob_id_to_outcome_index.find(order.clob_id);
        if (oit != this->clob_id_to_outcome_index.end()) {
            md["outcome_index"] = oit->second;
            if (oit->second >= 0 &&
                oit->second <
                    static_cast<int>(this->outcome_names_by_index.size())) {
                md["outcome_name"] = this->outcome_names_by_index[oit->second];
            }
        }
    }
    if (event_type == Core::TRADING_LOG_EVENT_TYPE_ORDER_CREATED) {
        engine.create_system_log(this->symbol, LOG_LEVEL_INFO,
                                 "Create order: " + this->symbol, event_type,
                                 md, order.strategy_id, order.exchange_name,
                                 order.market_type, true);
    } else if (event_type == TRADING_LOG_EVENT_TYPE_ORDER_FILLED) {
        engine.create_system_log(this->symbol, LOG_LEVEL_INFO,
                                 "Filled order: " + this->symbol, event_type,
                                 md, order.strategy_id, order.exchange_name,
                                 order.market_type, true);
    } else if (event_type == TRADING_LOG_EVENT_TYPE_ORDER_CANCELLED) {
        engine.create_system_log(this->symbol, LOG_LEVEL_INFO,
                                 "Cancel order: " + this->symbol, event_type,
                                 md, order.strategy_id, order.exchange_name,
                                 order.market_type, true);
    }
}

Core::TradeTicker::TradeTicker(
    std::vector<ExchangeAndMarket> exchange_and_markets, std::string symbol,
    std::string quote_asset, std::string base_asset, int strategy_id)
    : symbol(symbol),
      quote_asset(quote_asset),
      base_asset(base_asset),
      strategy_id(strategy_id) {
    for (const auto &exchange_and_market : exchange_and_markets) {
        this->exchange_and_markets.push_back(exchange_and_market);
        this->exchange_market_map[Utils::get_exchange_market_key(
            exchange_and_market.exchange_name,
            exchange_and_market.market_type)] = exchange_and_market;
    }

    for (int i = 0; i < MAX_POLYMARKET_OUTCOMES; i++) {
        outcome_exchange_inventory[i].store(0.0, std::memory_order_relaxed);
    }

    const char *data_dir_root_env = std::getenv("DATA_DIR_ROOT");
    if (!data_dir_root_env) {
        throw std::runtime_error("DATA_DIR_ROOT is not set");
    }
    std::string data_dir_root = data_dir_root_env;
    if (data_dir_root.empty()) {
        throw std::runtime_error("DATA_DIR_ROOT is not set");
    }

    std::string live_stop_loss_file = data_dir_root + "/live/" + this->symbol +
                                      "/" + std::to_string(this->strategy_id) +
                                      "/persistent_state/state.json";

    if (std::filesystem::exists(live_stop_loss_file)) {
        std::ifstream file(live_stop_loss_file);
        if (file.is_open()) {
            nlohmann::json j;
            file >> j;
            if (j.contains("last_stop_loss_triggered_time_ms")) {
                this->last_stop_loss_triggered_time_ms.store(
                    j["last_stop_loss_triggered_time_ms"].get<long long>());
            }
            file.close();
        }
    }
}

void Core::TradeTicker::configure_from_strategy(
    const StrategyConfig &strategy) {
    if (strategy.is_market_making_strategy) {
        this->is_market_making_strategy.store(true);
    }
    this->account_mode = account_mode_from_str(strategy.account_mode);
}

void Core::TradeTicker::populate_polymarket_fields(
    const ::TradeTicker &db_ticker) {
    this->slug = db_ticker.slug;
    this->condition_id = db_ticker.condition_id;
    this->is_active.store(db_ticker.is_active);
    this->is_closed.store(db_ticker.is_closed);
    this->is_accepting_orders.store(db_ticker.is_accepting_orders);
    this->is_order_book_enabled.store(db_ticker.is_order_book_enabled);
    this->order_price_min_tick_size.store(db_ticker.order_price_min_tick_size);
    this->order_min_size.store(db_ticker.order_min_size);
    this->event_start_time_ms.store(db_ticker.event_start_time_ms);
    this->event_end_time_ms.store(db_ticker.event_end_time_ms);
    this->accepting_orders_timestamp_ms.store(
        db_ticker.accepting_orders_timestamp_ms);
    this->is_neg_risk.store(db_ticker.is_neg_risk);
    this->num_outcomes.store(db_ticker.num_outcomes);
    this->outcome_names_by_index = db_ticker.outcome_names;
    this->clob_ids_by_index = db_ticker.clob_token_ids;
    // Only flag as PM if the DB row actually carries PM identifiers. The
    // backtest init pipeline calls this function for every ticker (PM and
    // crypto alike); previously this unconditionally set the PM flag,
    // routing crypto tickers through polymarket-style integer-quantity
    // rounding in round_and_stringify_order and silently zeroing
    // sub-1-unit quantities (e.g. 0.26 SOL → 0).
    this->is_trading_on_prediction_market.store(
        !db_ticker.condition_id.empty());

    this->clob_id_to_outcome_index.clear();
    for (int i = 0; i < (int)this->clob_ids_by_index.size(); i++) {
        this->clob_id_to_outcome_index[this->clob_ids_by_index[i]] = i;
    }
}

void Core::TradeTicker::populate_polymarket_fields_from_meta(
    const PolymarketEventMeta &meta) {
    this->slug = meta.slug;
    this->condition_id = meta.condition_id;
    this->is_active.store(meta.active);
    this->is_closed.store(meta.closed);
    this->is_accepting_orders.store(meta.accepting_orders);
    this->is_order_book_enabled.store(true);
    this->order_price_min_tick_size.store(meta.order_price_min_tick_size);
    this->order_min_size.store(meta.order_min_size);
    this->event_start_time_ms.store(meta.event_start_time_ms);
    this->event_end_time_ms.store(meta.event_end_time_ms);
    this->accepting_orders_timestamp_ms.store(
        meta.accepting_orders_timestamp_ms);
    this->is_neg_risk.store(meta.is_neg_risk);
    this->num_outcomes.store(meta.num_outcomes);
    this->outcome_names_by_index = meta.outcome_names;
    this->clob_ids_by_index = meta.clob_token_ids;
    this->is_trading_on_prediction_market.store(true);

    this->clob_id_to_outcome_index.clear();
    for (int i = 0; i < (int)this->clob_ids_by_index.size(); i++) {
        this->clob_id_to_outcome_index[this->clob_ids_by_index[i]] = i;
    }
    this->strike_price = meta.strike_price;
    this->underlying_symbol = meta.underlying_symbol;
    this->product_type = meta.product_type;
}

void Core::TradeTicker::update_outcome_best_bid_ask(int outcome_index,
                                                    double best_bid,
                                                    double best_ask,
                                                    long long timestamp_ms) {
    if (outcome_index < 0 || outcome_index >= MAX_POLYMARKET_OUTCOMES) return;

    auto &bba = outcome_bba[outcome_index];
    uint32_t s = bba.seq.load(std::memory_order_relaxed);
    bba.seq.store(s + 1, std::memory_order_release);

    bba.best_bid.store(best_bid, std::memory_order_relaxed);
    bba.best_ask.store(best_ask, std::memory_order_relaxed);
    bba.timestamp_ms.store(timestamp_ms, std::memory_order_relaxed);

    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count();
    bba.local_receive_time_ms.store(now, std::memory_order_relaxed);

    bba.seq.store(s + 2, std::memory_order_release);
}

std::optional<Core::TradeTicker::OutcomeBBASnapshot>
Core::TradeTicker::read_outcome_bba(int outcome_index) const {
    if (outcome_index < 0 || outcome_index >= MAX_POLYMARKET_OUTCOMES)
        return std::nullopt;
    auto &bba = outcome_bba[outcome_index];
    uint32_t s0 = bba.seq.load(std::memory_order_acquire);
    if (s0 & 1) return std::nullopt;

    OutcomeBBASnapshot snap;
    snap.best_bid = bba.best_bid.load(std::memory_order_relaxed);
    snap.best_ask = bba.best_ask.load(std::memory_order_relaxed);
    snap.timestamp_ms = bba.timestamp_ms.load(std::memory_order_relaxed);
    snap.local_receive_time_ms =
        bba.local_receive_time_ms.load(std::memory_order_relaxed);

    uint32_t s1 = bba.seq.load(std::memory_order_acquire);
    if (s0 != s1) return std::nullopt;
    if (snap.local_receive_time_ms <= 0) return std::nullopt;
    return snap;
}

void Core::TradeTicker::update_outcome_exchange_inventory(int outcome_index,
                                                          double inventory) {
    if (outcome_index < 0 || outcome_index >= MAX_POLYMARKET_OUTCOMES) return;
    outcome_exchange_inventory[outcome_index].store(inventory,
                                                    std::memory_order_relaxed);
}

double Core::TradeTicker::get_outcome_exchange_inventory(
    int outcome_index) const {
    if (outcome_index < 0 || outcome_index >= MAX_POLYMARKET_OUTCOMES)
        return 0.0;
    return outcome_exchange_inventory[outcome_index].load(
        std::memory_order_relaxed);
}

bool Core::TradeTicker::is_prediction_market_active() const {
    if (!this->is_trading_on_prediction_market.load()) return true;
    long long now_ms = Core::Engine::getInstance().get_time();
    long long start = this->accepting_orders_timestamp_ms.load();
    long long end_time = this->event_end_time_ms.load();
    return now_ms >= start && now_ms < end_time;
}

bool Core::TradeTicker::is_polymarket_event_ongoing(
    long long current_time_ms) const {
    long long start = this->event_start_time_ms.load();
    long long end_time = this->event_end_time_ms.load();
    return current_time_ms >= start && current_time_ms < end_time;
}

double Core::TradeTicker::get_net_polymarket_position_on_binary_market(
    const std::string &clob_id) const {
    double current_position = 0.0;
    double other_position = 0.0;
    for (const auto &trade : this->active_trades) {
        double qty = (trade.open_quantity - trade.closed_quantity).toDouble();
        if (trade.clob_token_id == clob_id) {
            current_position += qty;
        } else {
            other_position += qty;
        }
    }
    return current_position - other_position;
}

void Core::TradeTicker::add_signal(const Core::Signal &signal,
                                   Core::Strategy &strategy) {
    strategy.mark_received_update();
    std::lock_guard<std::mutex> lock(this->trading_mutex);
    if (this->signals.size() > 0) {
        this->prev_signal = this->signals.back();
    }
    this->signals.push_back(signal);
    this->curr_signal = signal;

    if (signal.signal_strength != MPREAL_ZERO) {
        this->is_signal_actionable.store(true);
    } else {
        this->is_signal_actionable.store(false);
    }
}

void Core::TradeTicker::add_signal(const Core::Signal &signal,
                                   Core::Strategy &strategy,
                                   size_t max_signals) {
    this->add_signal(signal, strategy);

    std::lock_guard<std::mutex> lock(this->trading_mutex);
    if (this->signals.size() > max_signals) {
        auto last = this->signals.back();
        this->signals.clear();
        this->signals.push_back(last);
    }
}

void Core::TradeTicker::add_signal_by_model(const std::string &model_name,
                                            const Core::Signal &signal,
                                            Core::Strategy &strategy) {
    strategy.mark_received_update();
    std::lock_guard<std::mutex> lock(this->trading_mutex);
    this->signals_by_model[model_name] = signal;
}

std::optional<Core::Signal> Core::TradeTicker::get_signal_by_model(
    const std::string &model_name) const {
    auto it = this->signals_by_model.find(model_name);
    if (it == this->signals_by_model.end()) return std::nullopt;
    return it->second;
}

std::optional<Core::Signal> Core::TradeTicker::get_prediction() {
    if (this->signals.size() == 0) {
        return std::nullopt;
    }
    return this->signals.back();
}

std::vector<Core::Signal> Core::TradeTicker::get_predictions() {
    return this->signals;
}

mpfr::mpreal Core::TradeTicker::get_sizing_coeff_deltas() {
    mpfr::mpreal ret = MPREAL_ZERO;
    for (const auto &trade : this->active_trades) {
        if (trade.type_internal == Core::TradeTypeInternal::HEDGE) {
            continue;
        }
        ret += trade.sizing_coeff_delta *
               (1 - (trade.closed_quantity / trade.open_quantity));
    }
    return ret;
}

mpfr::mpreal Core::TradeTicker::get_enter_order_sizing_coeff_deltas() {
    mpfr::mpreal ret = MPREAL_ZERO;
    for (const auto &order : this->orders) {
        if (order.hedge_for_trade_id.has_value()) {
            continue;
        }
        if (!order.is_trade_close_order) {
            ret += order.sizing_coeff_delta;
        }
    }
    return ret;
}

void Core::TradeTicker::sync_persistent_state_to_disk() {
    const char *data_dir_root_env = std::getenv("DATA_DIR_ROOT");
    if (!data_dir_root_env) {
        throw std::runtime_error("DATA_DIR_ROOT is not set");
    }
    std::string data_dir_root = data_dir_root_env;
    if (data_dir_root.empty()) {
        throw std::runtime_error("DATA_DIR_ROOT is not set");
    }

    std::string live_stop_loss_dir = data_dir_root + "/live/" + this->symbol +
                                     "/" + std::to_string(this->strategy_id) +
                                     "/persistent_state";

    std::string live_stop_loss_file = live_stop_loss_dir + "/state.json";

    if (!std::filesystem::exists(live_stop_loss_dir)) {
        std::filesystem::create_directories(live_stop_loss_dir);
    }

    std::ofstream file(live_stop_loss_file);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file for writing: " +
                                 live_stop_loss_file);
    }

    nlohmann::json j;
    j["last_stop_loss_triggered_time_ms"] =
        this->last_stop_loss_triggered_time_ms.load();
    file << j.dump(4);
    file.close();
}

void Core::TradeTicker::on_create_order(Engine &engine, Order &order) {
    if (engine.is_order_creation_disabled()) {
        return;
    }
    this->on_create_order_impl(
        engine,
        order);  // virtual, different implementations for live and backtest
    this->create_order_log(engine, order,
                           Core::TRADING_LOG_EVENT_TYPE_ORDER_CREATED);
}

void Core::TradeTicker::on_cancel_order(Engine &engine, Order &order) {
    this->create_order_log(engine, order,
                           Core::TRADING_LOG_EVENT_TYPE_ORDER_CANCELLED);
    this->on_cancel_order_impl(engine,
                               order);  // virtual, different implementations
                                        // for live and backtest
}

void Core::TradeTicker::on_cancel_rm_from_state(Engine &engine,
                                                const Order &order,
                                                bool creation_failed) {
    std::optional<Core::Order> order_to_update;
    bool order_found = false;

    {
        std::lock_guard<std::mutex> lock(this->trading_mutex);

        auto order_it =
            std::find_if(this->orders.begin(), this->orders.end(),
                         [&order](const Order &o) { return o.id == order.id; });
        if (order_it == this->orders.end()) {
            this->unacked_new_cancels.erase(get_order_cancel_key(order));
            engine.create_system_log(
                this->symbol, LOG_LEVEL_ERROR,
                "on_cancel_rm_from_state: Order not found in orders: " +
                    order.symbol,
                Core::TRADING_LOG_EVENT_TYPE_ORDER_STATE_VERIFY_MISMATCH,
                order.to_json(), order.strategy_id, order.exchange_name,
                order.market_type);
            return;
        }

        order_found = true;
        order_to_update = *order_it;
        this->erase_order_from_state(engine, order_it);

        if (order.trade_id.has_value()) {
            for (auto &trade : this->active_trades) {
                if (trade.id == order.trade_id.value()) {
                    trade.exit_order_ids.erase(
                        std::remove(trade.exit_order_ids.begin(),
                                    trade.exit_order_ids.end(), order.id),
                        trade.exit_order_ids.end());

                    if (order.cancel_reason.has_value()) {
                        if (order.cancel_reason.value() ==
                            Core::OrderCancelReason::EXPIRED) {
                            trade.num_expired_exit_orders += 1;
                        }
                    }
                }
            }
        }

        order_to_update->ack_state_removal = engine.get_time();

        if (order_to_update->is_prediction_market_order) {
            this->orders_removed_from_state.push_back(*order_to_update);
        }

        if (creation_failed) {
            const auto &key =
                Core::Utils::get_symbol_exchange_market_account_key(
                    order.symbol, order.exchange_name, order.market_type,
                    order.account_id);
            this->failed_create_order_times[key] =
                std::chrono::steady_clock::now();
        }
    }

    if (order_found && order_to_update.has_value()) {
        engine.add_order_cancel_to_logs(*order_to_update);
        engine.update_order(*order_to_update);
        engine.remove_order_from_risk_engine(order.id);
    }
}

std::optional<long long> Core::TradeTicker::get_ms_since_failed_create_order(
    const std::string &exchange_name, const std::string &market_type,
    const std::string &symbol, long long account_id) {
    const auto &key = Core::Utils::get_symbol_exchange_market_account_key(
        symbol, exchange_name, market_type, account_id);
    if (this->failed_create_order_times.find(key) ==
        this->failed_create_order_times.end()) {
        return std::nullopt;
    }
    auto now = std::chrono::steady_clock::now();
    auto time_since_failure =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now - this->failed_create_order_times[key]);
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               time_since_failure)
        .count();
}

void Core::TradeTicker::on_order_cancel_success(Engine &engine,
                                                const Order &order,
                                                bool creation_failed,
                                                bool require_lock) {
    this->on_cancel_rm_from_state(engine, order, creation_failed);
}

void Core::TradeTicker::recover_trade_from_disk(Engine &engine,
                                                const Trade &trade) {
    std::lock_guard<std::mutex> lock(this->trading_mutex);
    this->active_trades.push_back(trade);
    this->active_trade_count.fetch_add(1, std::memory_order_relaxed);
}

void Core::TradeTicker::recover_order_from_disk(Engine &engine,
                                                const Order &order) {
    std::lock_guard<std::mutex> lock(this->trading_mutex);
    this->orders.push_back(order);
    this->order_count.fetch_add(1, std::memory_order_relaxed);
    if (!order.is_order_creation_acknowledged) {
        if (order.is_trade_close_order) {
            this->unacked_exit_order_count.fetch_add(1,
                                                     std::memory_order_relaxed);
        } else {
            this->unacked_enter_order_count.fetch_add(
                1, std::memory_order_relaxed);
        }
    }
    const double remaining_qty =
        (order.quantity - order.filled_quantity).toDouble();
    const double notional = order.price.toDouble() * remaining_qty;
    if (order.side == Core::OrderSide::BUY) {
        this->bid_order_count_by_price[order.price.toDouble()] += 1;
        this->active_bid_count += 1;
        this->active_bid_notional += notional;
        if (order.is_prediction_market_order && !order.clob_id.empty()) {
            this->bid_order_count_by_clob_price[get_clob_price_key(
                order.clob_id, order.price_str)] += 1;
        }
    } else {
        this->ask_order_count_by_price[order.price.toDouble()] += 1;
        this->active_ask_count += 1;
        this->active_ask_notional += notional;
        if (order.is_prediction_market_order && !order.clob_id.empty()) {
            this->ask_order_count_by_clob_price[get_clob_price_key(
                order.clob_id, order.price_str)] += 1;
        }
    }
}

std::optional<std::chrono::steady_clock::time_point>
Core::TradeTicker::get_last_trade_closed_timepoint() {
    std::lock_guard<std::mutex> lock(this->trading_mutex);
    if (this->trade_completion_times.size() == 0) {
        return std::nullopt;
    }
    return this->trade_completion_times.back();
}

void Core::TradeTicker::inventory_mismatch_remove_trade(Engine &engine,
                                                        const Trade &trade) {
    std::lock_guard<std::mutex> lock(this->trading_mutex);
    this->active_trades.erase(
        std::remove_if(this->active_trades.begin(), this->active_trades.end(),
                       [&trade](const Trade &t) { return t.id == trade.id; }),
        this->active_trades.end());
    this->active_trade_count.store(this->active_trades.size(),
                                   std::memory_order_relaxed);
}

bool Core::TradeTicker::manually_remove_trade_by_id(
    unsigned long long trade_id) {
    std::lock_guard<std::mutex> lock(this->trading_mutex);
    size_t before = this->active_trades.size();
    this->active_trades.erase(
        std::remove_if(this->active_trades.begin(), this->active_trades.end(),
                       [trade_id](const Trade &t) { return t.id == trade_id; }),
        this->active_trades.end());
    this->active_trade_count.store(this->active_trades.size(),
                                   std::memory_order_relaxed);
    return this->active_trades.size() < before;
}

void Core::TradeTicker::inventory_mismatch_update_trade_size(
    Engine &engine, const Trade &trade, mpfr::mpreal new_quantity) {
    std::lock_guard<std::mutex> lock(this->trading_mutex);
    for (auto &t : this->active_trades) {
        if (t.id == trade.id) {
            t.open_quantity = new_quantity;
            t.closed_quantity = 0;
        }
    }
}

void Core::TradeTicker::move_trade_to_completed(Core::Trade &trade,
                                                long long close_time_ms) {
    trade.close_time_ms = close_time_ms;
    Core::Trade completed = trade;
    this->trade_completion_times.push_back(std::chrono::steady_clock::now());

    if (this->is_trading_on_prediction_market.load() &&
        completed.has_unfinalized_fills()) {
        this->recently_completed_polymarket_trades.push_back(completed);
    }

    active_trades.erase(
        std::remove_if(
            active_trades.begin(), active_trades.end(),
            [&completed](const Trade &t) { return t.id == completed.id; }),
        active_trades.end());
    this->active_trade_count.store(active_trades.size(),
                                   std::memory_order_relaxed);
}

void Core::TradeTicker::finalize_completed_trade(Engine &engine,
                                                 Core::Trade &completed_trade,
                                                 long long strategy_id) {
    Trade::calculate_pnl_and_fees(engine, completed_trade);
    engine.create_system_log(
        this->symbol, LOG_LEVEL_INFO,
        "Trade on " + this->symbol + " completed with PnL: " +
            std::to_string(completed_trade.pnl.toDouble()) + "$",
        Core::TRADING_LOG_EVENT_TYPE_TRADE_CLOSED, completed_trade.to_json(),
        strategy_id, completed_trade.exchange_name,
        completed_trade.market_type);

    engine.update_strategy_virtual_balance(completed_trade);
    engine.add_trade_to_logs(completed_trade);
    engine.remove_trade_from_risk_engine(completed_trade.id);
}

namespace {

std::string build_force_close_reason_detail(Core::ForceCloseReason reason,
                                            const nlohmann::json &context) {
    switch (reason) {
        case Core::ForceCloseReason::REMAINING_NOTIONAL_BELOW_MIN: {
            const auto remaining = context.value("remaining_notional", 0.0);
            const auto min_notional = context.value("min_notional", 0.0);
            const auto mark_price = context.value("mark_price", 0.0);
            return "remaining notional " + std::to_string(remaining) +
                   " below min " + std::to_string(min_notional) + " at price " +
                   std::to_string(mark_price);
        }
    }
    return "unknown force-close reason";
}

}  // namespace

void Core::TradeTicker::force_close_trade(Engine &engine,
                                          unsigned long long trade_id,
                                          Core::ForceCloseReason reason,
                                          const nlohmann::json &context) {
    Core::Trade completed_trade;
    long long strategy_id = 0;
    bool found = false;

    {
        std::lock_guard<std::mutex> lock(this->trading_mutex);
        for (auto &trade : this->active_trades) {
            if (trade.id == trade_id) {
                trade.closed_quantity = trade.open_quantity;
                completed_trade = trade;
                strategy_id = trade.strategy_id;
                found = true;
                this->move_trade_to_completed(completed_trade,
                                              engine.get_time());
                break;
            }
        }
    }

    if (found) {
        nlohmann::json metadata =
            context.is_object() ? context : nlohmann::json::object();
        metadata["trade_id"] = trade_id;
        metadata["reason_code"] = Core::force_close_reason_to_str(reason);
        metadata["reason_detail"] =
            build_force_close_reason_detail(reason, metadata);

        engine.create_system_log(
            this->symbol, LOG_LEVEL_WARNING,
            "Trade force-closed: " +
                std::string(Core::force_close_reason_to_str(reason)),
            Core::TRADING_LOG_EVENT_TYPE_TRADE_FORCE_CLOSED, metadata,
            strategy_id, completed_trade.exchange_name,
            completed_trade.market_type, true);

        this->finalize_completed_trade(engine, completed_trade, strategy_id);
        return;
    }

    nlohmann::json metadata =
        context.is_object() ? context : nlohmann::json::object();
    metadata["trade_id"] = trade_id;
    metadata["reason_code"] = Core::force_close_reason_to_str(reason);
    metadata["reason_detail"] =
        build_force_close_reason_detail(reason, metadata);
    metadata["outcome"] = "trade_not_found";

    engine.create_system_log(this->symbol, LOG_LEVEL_ERROR,
                             "Trade force-close failed: trade not found",
                             Core::TRADING_LOG_EVENT_TYPE_TRADE_FORCE_CLOSED,
                             metadata, 0, "", "", true);
}

bool Core::TradeTicker::is_cancel_already_sent(const std::string &cancel_key) {
    auto order_it = this->unacked_new_cancels.find(cancel_key);
    if (order_it == this->unacked_new_cancels.end()) {
        return false;
    }
    auto now = std::chrono::steady_clock::now();
    auto time_since_cancel_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now - order_it->second.first)
            .count();

    if (is_trading_on_prediction_market.load()) {
        if (time_since_cancel_ms > 1000) {
            this->unacked_new_cancels.erase(order_it);
            return false;
        }
    } else if (time_since_cancel_ms > 5000) {
        this->unacked_new_cancels.erase(order_it);
        return false;
    }
    return true;
}

bool Core::TradeTicker::is_cancel_already_sent(const Order &order) {
    return is_cancel_already_sent(get_order_cancel_key(order));
}

void Core::TradeTicker::mark_order_to_be_created(
    Engine &engine, Order &order, StrategyDecision &strategy_decision,
    Strategy &strategy) {
    if (order.quantity.toDouble() == 0) {
        return;
    }

    engine.register_order_in_risk_engine(order);

    this->orders.push_back(order);
    this->order_count.fetch_add(1, std::memory_order_relaxed);
    if (order.is_trade_close_order) {
        this->unacked_exit_order_count.fetch_add(1, std::memory_order_relaxed);
    } else {
        this->unacked_enter_order_count.fetch_add(1, std::memory_order_relaxed);
    }
    if (order.trade_id.has_value()) {
        for (auto &trade : this->active_trades) {
            if (trade.id == order.trade_id.value()) {
                trade.exit_order_ids.push_back(order.id);
            }
        }
    }
    strategy_decision.orders.push_back(order);
    engine.add_order_to_logs(order);
    if (order.is_trade_close_order) {
        strategy.record_exit_order_sent();
        this->record_exit_order_sent();
    } else {
        strategy.record_enter_order_sent();
        this->record_enter_order_sent();
    }
    const auto &key = Core::Utils::get_symbol_exchange_market_key(
        order.symbol, order.exchange_name, order.market_type);

    this->create_order_times[key] = std::chrono::steady_clock::now();
    const double notional = (order.price * order.quantity).toDouble();
    if (order.side == Core::OrderSide::BUY) {
        this->bid_order_count_by_price[order.price.toDouble()] += 1;
        this->active_bid_count += 1;
        this->active_bid_notional += notional;
        if (order.is_prediction_market_order && !order.clob_id.empty()) {
            this->bid_order_count_by_clob_price[get_clob_price_key(
                order.clob_id, order.price_str)] += 1;
        }
    } else {
        this->ask_order_count_by_price[order.price.toDouble()] += 1;
        this->active_ask_count += 1;
        this->active_ask_notional += notional;
        if (order.is_prediction_market_order && !order.clob_id.empty()) {
            this->ask_order_count_by_clob_price[get_clob_price_key(
                order.clob_id, order.price_str)] += 1;
        }
    }
}

void Core::TradeTicker::mark_order_to_be_cancelled(
    Engine &engine, Order &order, StrategyDecision &strategy_decision) {
    if (!engine.is_backtest) {
        if (!this->is_cancel_ready_to_be_sent(order)) {
            return;
        }
    }

    const std::string cancel_key = get_order_cancel_key(order);
    if (this->is_cancel_already_sent(cancel_key)) {
        return;
    }
    this->unacked_new_cancels[cancel_key] =
        std::make_pair(std::chrono::steady_clock::now(), order);
    auto order_it =
        std::find_if(this->orders.begin(), this->orders.end(),
                     [&cancel_key](const Order &o) {
                         return get_order_cancel_key(o) == cancel_key;
                     });
    if (order_it != this->orders.end()) {
        order_it->last_cancel_request_time_ms = engine.get_time();
        if (order_it->cancel_timestamp == 0) {
            order_it->cancel_timestamp = engine.get_time();
        }
        strategy_decision.cancelled_orders.push_back(*order_it);
    } else {
        strategy_decision.cancelled_orders.push_back(order);
    }
}

std::optional<long long>
Core::TradeTicker::get_exchange_and_market_type_account_id(
    const std::string &exchange_name, const std::string &market_type) {
    const std::string &key =
        Utils::get_exchange_market_key(exchange_name, market_type);
    const auto it = this->exchange_market_map.find(key);
    if (it == this->exchange_market_map.end()) {
        return std::nullopt;
    }
    return it->second.account_id;
}

std::tuple<Core::OrderConstructionResult, std::optional<Core::Order>>
Core::TradeTicker::construct_order(Engine &engine, Strategy &strategy,
                                   Core::CreateOrderParams &params) {
    auto body = [&]() -> std::tuple<Core::OrderConstructionResult,
                                    std::optional<Core::Order>> {
        if (strategy.stop_making_markets.load()) {
            return {Core::STRATEGY_STOP_MAKING_MARKETS, std::nullopt};
        }

        if (params.is_trade_close_order) {
            // Strategy-level exit order rate limiting
            if (params.limit_strategy_level_exit_order_rate &&
                params.max_strategy_level_exit_orders_per_10s.has_value() &&
                strategy.get_recent_exit_orders_count_10s() >=
                    params.max_strategy_level_exit_orders_per_10s.value()) {
                return {Core::STRATEGY_EXIT_RATE_LIMITED, std::nullopt};
            }
            // Ticker-level exit order rate limiting
            if (params.limit_ticker_level_exit_order_rate &&
                params.max_ticker_level_exit_orders_per_10s.has_value() &&
                this->get_recent_exit_orders_count_10s() >=
                    params.max_ticker_level_exit_orders_per_10s.value()) {
                return {Core::TICKER_EXIT_RATE_LIMITED, std::nullopt};
            }
        } else {
            // Strategy-level enter order rate limiting
            if (params.limit_strategy_level_enter_order_rate &&
                params.max_strategy_level_enter_orders_per_10s.has_value() &&
                strategy.get_recent_enter_orders_count_10s() >=
                    params.max_strategy_level_enter_orders_per_10s.value()) {
                return {Core::STRATEGY_ENTER_RATE_LIMITED, std::nullopt};
            }
            // Ticker-level enter order rate limiting
            if (params.limit_ticker_level_enter_order_rate &&
                params.max_ticker_level_enter_orders_per_10s.has_value() &&
                this->get_recent_enter_orders_count_10s() >=
                    params.max_ticker_level_enter_orders_per_10s.value()) {
                return {Core::TICKER_ENTER_RATE_LIMITED, std::nullopt};
            }
        }

        if (params.apply_cooldown_after_failed_order) {
            const auto time_since_failed =
                this->get_ms_since_failed_create_order(
                    params.exchange_name, params.market_type, params.symbol,
                    params.account_id.value_or(-1));
            if (time_since_failed.has_value()) {
                const long long cooldown_threshold =
                    params.is_trade_close_order
                        ? params.failed_order_cooldown_ms_exit.value_or(10000)
                        : params.failed_order_cooldown_ms_enter.value_or(
                              120000);
                if (time_since_failed.value() < cooldown_threshold) {
                    return {params.is_trade_close_order
                                ? Core::FAILED_ORDER_COOLDOWN_EXIT
                                : Core::FAILED_ORDER_COOLDOWN_ENTER,
                            std::nullopt};
                }
            }
        }

        if (params.sizing_coeff_delta.toDouble() <= 0) {
            return {Core::INVALID_SIZING_COEFF_DELTA, std::nullopt};
        }

        if (params.automatically_add_orderbook_prices_to_params) {
            const auto best_bid_ask_with_qty = engine.get_best_bid_ask_with_qty(
                params.symbol, params.exchange_name, params.market_type,
                60000LL * 3LL);
            if (!best_bid_ask_with_qty.has_value()) {
                return {Core::MISSING_ORDERBOOK_PRICES, std::nullopt};
            }
            params.best_bid_price = best_bid_ask_with_qty->bid_price;
            params.best_ask_price = best_bid_ask_with_qty->ask_price;
        }

        if (params.prevent_overlapping_orders) {
            if (params.is_prediction_market_order && !params.clob_id.empty()) {
                // Use polymarket rounding for overlap check so the key matches
                // what round_and_stringify_order will produce for the map
                // insertion
                bool round_up = (params.side == Core::OrderSide::SELL);
                double rounded_price = this->round_polymarket_price(
                    params.price.toDouble(), round_up);
                int price_precision = StrMath::step_size_to_precision(
                    this->order_price_min_tick_size.load());
                std::string price_str = StrMath::to_fixed_string(
                    mpfr::mpreal(rounded_price), price_precision);
                std::string key = get_clob_price_key(params.clob_id, price_str);

                if (params.side == Core::OrderSide::BUY) {
                    if (this->bid_order_exists_on_book(key)) {
                        return {Core::OVERLAPPING_BID_ORDER_EXISTS,
                                std::nullopt};
                    }
                } else {
                    if (this->ask_order_exists_on_book(key)) {
                        return {Core::OVERLAPPING_ASK_ORDER_EXISTS,
                                std::nullopt};
                    }
                }
            } else {
                const std::string symbol_for_rounding =
                    params.route_order_to_alt_symbol.has_value()
                        ? params.route_order_to_alt_symbol.value()
                        : params.symbol;

                if (!engine.is_backtest) {
                    params.price = engine.round_price(
                        symbol_for_rounding, params.exchange_name,
                        params.market_type, params.price);
                }
                if (params.side == Core::OrderSide::BUY) {
                    if (this->bid_order_exists_on_book(
                            params.price.toDouble())) {
                        return {Core::OVERLAPPING_BID_ORDER_EXISTS,
                                std::nullopt};
                    }
                } else {
                    if (this->ask_order_exists_on_book(
                            params.price.toDouble())) {
                        return {Core::OVERLAPPING_ASK_ORDER_EXISTS,
                                std::nullopt};
                    }
                }
            }
        }

        if (params.max_notional_resting_on_book.has_value()) {
            double current_notional = (params.side == Core::OrderSide::BUY)
                                          ? this->active_bid_notional
                                          : this->active_ask_notional;
            if (current_notional >=
                params.max_notional_resting_on_book.value()) {
                return {Core::MAX_NOTIONAL_RESTING_ON_BOOK_EXCEEDED,
                        std::nullopt};
            }
        }

        if (params.is_trade_close_order) {
            if (params.max_unacked_exit_orders.has_value()) {
                if (this->unacked_exit_order_count.load(
                        std::memory_order_relaxed) >=
                    params.max_unacked_exit_orders.value()) {
                    return {Core::MAX_UNACKED_EXIT_ORDERS_EXCEEDED,
                            std::nullopt};
                }
            }
        } else {
            if (params.max_unacked_enter_orders.has_value()) {
                if (this->unacked_enter_order_count.load(
                        std::memory_order_relaxed) >=
                    params.max_unacked_enter_orders.value()) {
                    return {Core::MAX_UNACKED_ENTER_ORDERS_EXCEEDED,
                            std::nullopt};
                }
            }
        }

        if (params.ensure_post_only) {
            if (params.side == Core::OrderSide::SELL) {
                if (!params.best_bid_price.has_value() ||
                    params.price.toDouble() <= params.best_bid_price.value()) {
                    return {Core::POST_ONLY_VALIDATION_FAILED_SELL,
                            std::nullopt};
                }
            } else {
                if (!params.best_ask_price.has_value() ||
                    params.price.toDouble() >= params.best_ask_price.value()) {
                    return {Core::POST_ONLY_VALIDATION_FAILED_BUY,
                            std::nullopt};
                }
            }
        }

        if (params.prevent_price_exceeding_threshold) {
            if (!params.safe_price_above_current_price_threshold.has_value() ||
                !params.best_bid_price.has_value() ||
                !params.best_ask_price.has_value()) {
                std::string error_message =
                    "safe_price_above_current_price_threshold and "
                    "best_bid_price and best_ask_price must be set";
                std::abort();
            }

            if (params.side == Core::OrderSide::SELL) {
                double min_price =
                    params.best_bid_price.value() *
                    (1 -
                     params.safe_price_above_current_price_threshold.value());
                if (params.price.toDouble() < min_price) {
                    params.price = params.best_bid_price.value();
                }
            } else {
                double max_price =
                    params.best_ask_price.value() *
                    (1 +
                     params.safe_price_above_current_price_threshold.value());
                if (params.price.toDouble() > max_price) {
                    params.price = params.best_ask_price.value();
                }
            }
        }

        if (params.is_maximum_slippage_enabled) {
            if (!params.signal_start_price.has_value()) {
                std::string error_message = "signal_start_price must be set";
                std::abort();
            }

            if (!params.maximum_slippage_coeff.has_value()) {
                std::string error_message =
                    "maximum_slippage_coeff must be set";
                std::abort();
            }

            if (params.side == Core::OrderSide::BUY) {
                if (params.price.toDouble() >
                    params.signal_start_price.value() *
                        (1 + params.maximum_slippage_coeff.value())) {
                    return {Core::MAXIMUM_SLIPPAGE_EXCEEDED_BUY, std::nullopt};
                }
            } else {
                if (params.price.toDouble() <
                    params.signal_start_price.value() *
                        (1 - params.maximum_slippage_coeff.value())) {
                    return {Core::MAXIMUM_SLIPPAGE_EXCEEDED_SELL, std::nullopt};
                }
            }
        }

        if (params.only_post_if_price_within_threshold) {
            if (!params.price_within_threshold_percent.has_value() ||
                !params.best_bid_price.has_value() ||
                !params.best_ask_price.has_value()) {
                std::string error_message =
                    "price_within_threshold_percent and best_bid_price and "
                    "best_ask_price must be set";
                std::abort();
            }

            double order_price = params.price.toDouble();
            double threshold = params.price_within_threshold_percent.value();

            if (params.side == Core::OrderSide::BUY) {
                double diff_pct =
                    (params.best_bid_price.value() - order_price) /
                    params.best_bid_price.value();
                if (diff_pct > threshold) {
                    return {Core::PRICE_WITHIN_THRESHOLD_EXCEEDED_BUY,
                            std::nullopt};
                }
            } else {
                double diff_pct =
                    (order_price - params.best_ask_price.value()) /
                    params.best_ask_price.value();
                if (diff_pct > threshold) {
                    return {Core::PRICE_WITHIN_THRESHOLD_EXCEEDED_SELL,
                            std::nullopt};
                }
            }
        }

        if (params.ensure_inventory_exists_or_borrowable) {
            if (params.side == Core::OrderSide::SELL) {
                long long account_id = params.account_id.value_or(-1);
                const auto inventory = engine.get_inventory(
                    params.exchange_name, params.market_type, params.base_asset,
                    account_id);
                const auto borrowable_inventory =
                    engine.get_borrowable_inventory(
                        params.base_asset, params.exchange_name,
                        params.market_type, account_id);

                bool has_inventory =
                    inventory.has_value() && inventory->free.toDouble() > 0;
                bool is_borrowable = borrowable_inventory.has_value() &&
                                     borrowable_inventory.value() > 0;

                if (!has_inventory && !is_borrowable) {
                    return {Core::MISSING_INVENTORY_OR_BORROWABLE,
                            std::nullopt};
                }
            }
        }

        if (!engine.is_backtest && params.is_randomized_size) {
            if (!params.randomized_size_lower_bound.has_value() ||
                !params.randomized_size_upper_bound.has_value()) {
                std::string error_message =
                    "randomized_size_lower_bound and "
                    "randomized_size_upper_bound must be set";
                std::abort();
            }

            if (params.price * params.quantity >
                params.randomized_size_upper_bound.value()) {
                double old_quantity = params.quantity.toDouble();
                double randomized_quantity =
                    MathUtils::randomize_number_between_range_v2(
                        params.randomized_size_lower_bound.value(),
                        params.randomized_size_upper_bound.value());

                if (params.price.toDouble() > 0.0) {
                    double new_quantity =
                        randomized_quantity / params.price.toDouble();
                    params.quantity = new_quantity;
                    params.sizing_coeff_delta =
                        (new_quantity / old_quantity) *
                        params.sizing_coeff_delta.toDouble();
                }
            }
        }

        if (params.is_randomized_delay_between_orders && !engine.is_backtest) {
            const auto &key = Core::Utils::get_symbol_exchange_market_key(
                params.symbol, params.exchange_name, params.market_type);
            const auto it = this->next_allowed_order_timestamp.find(key);
            if (it != this->next_allowed_order_timestamp.end()) {
                if (std::chrono::steady_clock::now() < it->second) {
                    return {Core::RANDOMIZED_DELAY_BETWEEN_ORDERS_ACTIVE,
                            std::nullopt};
                }
            }
        }

        if (params.randomize_expiration_time && !engine.is_backtest) {
            if (!params.randomized_expiration_time_ms_lower_bound.has_value() ||
                !params.randomized_expiration_time_ms_upper_bound.has_value()) {
                std::string error_message =
                    "randomized_expiration_time_ms_lower_bound and "
                    "randomized_expiration_time_ms_upper_bound must be set";
                std::abort();
            }

            params.expires_on_ms =
                params.timestamp +
                static_cast<
                    long long>(MathUtils::randomize_number_between_range_v2(
                    params.randomized_expiration_time_ms_lower_bound.value(),
                    params.randomized_expiration_time_ms_upper_bound.value()));
        }

        auto account_id = this->get_exchange_and_market_type_account_id(
            params.exchange_name, params.market_type);
        if (!account_id.has_value() && !params.account_id.has_value()) {
            engine.create_system_log(
                this->symbol, LOG_LEVEL_ERROR,
                "Unable to find account id for: " + params.exchange_name + " " +
                    params.market_type,
                Core::TRADING_LOG_EVENT_TYPE_ACCOUNT_ERROR, nlohmann::json(),
                params.strategy_id);
            return {Core::MISSING_ACCOUNT_ID_TICKER, std::nullopt};
        }
        params.account_id = params.account_id.has_value()
                                ? params.account_id.value()
                                : account_id.value();

        auto [engine_result, order_optional] =
            engine.construct_order(strategy, params);
        if (engine_result != Core::VALID || !order_optional.has_value()) {
            return {engine_result, std::nullopt};
        }

        if (params.is_stop_loss_order) {
            this->mark_last_stop_loss_triggered_time(engine);
        }

        if (!engine.is_backtest && params.is_randomized_delay_between_orders) {
            const auto &key = Core::Utils::get_symbol_exchange_market_key(
                params.symbol, params.exchange_name, params.market_type);

            if (!params.randomized_delay_between_orders_ms_lower_bound
                     .has_value() ||
                !params.randomized_delay_between_orders_ms_upper_bound
                     .has_value()) {
                std::string error_message =
                    "randomized_delay_between_orders_ms_lower_bound and "
                    "randomized_delay_between_orders_ms_upper_bound must be "
                    "set";
                std::abort();
            }

            this->next_allowed_order_timestamp[key] =
                std::chrono::steady_clock::now() +
                std::chrono::milliseconds(
                    MathUtils::randomize_number_between_range_v2(
                        params.randomized_delay_between_orders_ms_lower_bound
                            .value(),
                        params.randomized_delay_between_orders_ms_upper_bound
                            .value()));
        }

        strategy.mark_received_update();

        this->round_and_stringify_order(engine, order_optional.value());
        return {Core::VALID, order_optional.value()};
    };  // end body lambda

    auto result = body();
    const auto construction_result = std::get<0>(result);
    if (construction_result != Core::VALID) {
        const char *reason_code =
            construction_result_to_reason_code(construction_result);
        const std::string key = std::to_string(params.strategy_id) + "|" +
                                this->symbol + "|" + reason_code;
        g_order_blocked_emitter.emit_if_allowed(
            key, ORDER_BLOCKED_COOLDOWN_MS, [&](uint64_t suppressed) {
                nlohmann::json md;
                md["reason_code"] = reason_code;
                md["strategy_id"] = params.strategy_id;
                md["symbol"] = this->symbol;
                md["exchange_name"] = params.exchange_name;
                md["market_type"] = params.market_type;
                md["side"] =
                    (params.side == Core::OrderSide::BUY) ? "BUY" : "SELL";
                md["is_trade_close_order"] = params.is_trade_close_order;
                md["events_suppressed_since_last_emit"] = suppressed;
                engine.create_system_log(
                    this->symbol, Core::LOG_LEVEL_INFO, "order blocked",
                    Core::TRADING_LOG_EVENT_TYPE_ORDER_BLOCKED, md,
                    params.strategy_id, params.exchange_name,
                    params.market_type);
            });
    }
    return result;
}

long long Core::TradeTicker::get_time_ms_since_last_stop_loss_triggered(
    Engine &engine) {
    return engine.get_time() - this->last_stop_loss_triggered_time_ms.load();
}

mpfr::mpreal Core::TradeTicker::get_trade_exit_qty_on_book(const Trade &trade) {
    mpfr::mpreal ret = MPREAL_ZERO;
    for (const auto &order : this->get_active_orders_ref()) {
        if (order.trade_id.has_value() && order.trade_id.value() == trade.id) {
            ret += order.quantity;
        }
    }
    return ret;
}

mpfr::mpreal Core::TradeTicker::get_trade_remaining_qty(const Trade &trade) {
    return trade.open_quantity - trade.closed_quantity -
           this->get_trade_exit_qty_on_book(trade);
}

int Core::TradeTicker::get_num_active_trades() {
    std::lock_guard<std::mutex> lock(this->trading_mutex);
    return this->active_trades.size();
}

int Core::TradeTicker::get_num_active_orders() {
    std::lock_guard<std::mutex> lock(this->trading_mutex);
    return this->orders.size();
}

int Core::TradeTicker::get_num_active_orders_no_lock() {
    return this->orders.size();
}

int Core::TradeTicker::get_num_active_orders_and_trades() {
    std::lock_guard<std::mutex> lock(this->trading_mutex);
    return this->active_trades.size() + this->orders.size();
}

long long Core::TradeTicker::get_last_sent_order_timestamp_ms(
    const std::string &exchange_name, const std::string &market_type,
    bool require_lock) {
    if (require_lock) {
        std::lock_guard<std::mutex> lock(this->trading_mutex);
        if (this->orders.size() == 0) {
            return 0;
        }

        const auto &key = Core::Utils::get_symbol_exchange_market_key(
            this->symbol, exchange_name, market_type);

        const auto it = this->create_order_times.find(key);
        if (it == this->create_order_times.end()) {
            return 0;
        }
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   it->second.time_since_epoch())
            .count();
    }

    if (this->orders.size() == 0) {
        return 0;
    }

    const auto &key = Core::Utils::get_symbol_exchange_market_key(
        this->symbol, exchange_name, market_type);

    const auto it = this->create_order_times.find(key);
    if (it == this->create_order_times.end()) {
        return 0;
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               it->second.time_since_epoch())
        .count();
}

std::optional<long long> Core::TradeTicker::get_time_ms_since_last_order_sent(
    long long current_time_ms, const std::string &exchange_name,
    const std::string &market_type, bool require_lock) {
    auto last_sent_order_timestamp_ms = this->get_last_sent_order_timestamp_ms(
        exchange_name, market_type, require_lock);
    if (last_sent_order_timestamp_ms == 0) {
        return std::nullopt;
    }
    return current_time_ms - last_sent_order_timestamp_ms;
}

mpfr::mpreal Core::TradeTicker::get_sizing_coeff_in_enter_orders() {
    mpfr::mpreal ret = MPREAL_ZERO;
    for (const auto &order : this->orders) {
        if (!order.is_trade_close_order) {
            ret += order.sizing_coeff_delta;
        }
    }
    return ret;
}

mpfr::mpreal Core::TradeTicker::get_sizing_coeff_in_exit_orders() {
    mpfr::mpreal ret = MPREAL_ZERO;
    for (const auto &order : this->orders) {
        if (order.is_trade_close_order) {
            ret += order.sizing_coeff_delta;
        }
    }
    return ret;
}

mpfr::mpreal Core::TradeTicker::get_bids_notional_size() {
    mpfr::mpreal ret = MPREAL_ZERO;
    for (const auto &order : this->orders) {
        if (order.side == Core::OrderSide::BUY) {
            ret += order.quantity * order.price;
        }
    }
    return ret;
}

mpfr::mpreal Core::TradeTicker::get_asks_notional_size() {
    mpfr::mpreal ret = MPREAL_ZERO;
    for (const auto &order : this->orders) {
        if (order.side == Core::OrderSide::SELL) {
            ret += order.quantity * order.price;
        }
    }
    return ret;
}

std::optional<mpfr::mpreal> Core::TradeTicker::get_total_inventory_notional() {
    mpfr::mpreal ret = MPREAL_ZERO;
    const auto &price =
        Core::Engine::getInstance().get_price(this->symbol, "binance", "swap");
    for (const auto &trade : this->active_trades) {
        if (trade.type == Core::TradeType::LONG) {
            ret += trade.open_quantity;
        } else {
            ret -= trade.open_quantity;
        }
    }
    if (!price.has_value()) {
        return std::nullopt;
    }

    return ret * price.value();
}

int Core::TradeTicker::get_num_enter_orders_by_signal_id(
    const unsigned long long signal_id, const std::string &exchange_name,
    const std::string &market_type) {
    return std::count_if(this->orders.begin(), this->orders.end(),
                         [&](const Order &order) {
                             return !order.is_trade_close_order &&
                                    order.signal_id == signal_id &&
                                    order.exchange_name == exchange_name &&
                                    order.market_type == market_type;
                         });
}

int Core::TradeTicker::get_num_enter_orders(const std::string &exchange_name,
                                            const std::string &market_type) {
    return std::count_if(this->orders.begin(), this->orders.end(),
                         [&](const Order &order) {
                             return !order.is_trade_close_order &&
                                    order.exchange_name == exchange_name &&
                                    order.market_type == market_type;
                         });
}

int Core::TradeTicker::get_num_enter_orders() {
    return std::count_if(
        this->orders.begin(), this->orders.end(),
        [&](const Order &order) { return !order.is_trade_close_order; });
}

void Core::TradeTicker::record_exit_order_sent() {
    this->recent_exit_order_times.push_back(std::chrono::steady_clock::now());
}

void Core::TradeTicker::record_enter_order_sent() {
    this->recent_enter_order_times.push_back(std::chrono::steady_clock::now());
}

void Core::TradeTicker::prune_recent_order_times() {
    const auto max_age = std::chrono::seconds(60);
    const auto cutoff = std::chrono::steady_clock::now() - max_age;
    while (!this->recent_exit_order_times.empty() &&
           this->recent_exit_order_times.front() < cutoff) {
        this->recent_exit_order_times.pop_front();
    }
    while (!this->recent_enter_order_times.empty() &&
           this->recent_enter_order_times.front() < cutoff) {
        this->recent_enter_order_times.pop_front();
    }
}

size_t Core::TradeTicker::purge_stale_removed_orders(long long now_ms,
                                                     long long grace_ms) {
    const size_t before = this->orders_removed_from_state.size();
    this->orders_removed_from_state.erase(
        std::remove_if(this->orders_removed_from_state.begin(),
                       this->orders_removed_from_state.end(),
                       [now_ms, grace_ms](const Order &order) {
                           if (order.ack_state_removal <= 0) {
                               return false;
                           }
                           return now_ms >= order.ack_state_removal + grace_ms;
                       }),
        this->orders_removed_from_state.end());
    return before - this->orders_removed_from_state.size();
}

int Core::TradeTicker::get_recent_exit_orders_count(
    std::chrono::seconds window) const {
    const auto cutoff = std::chrono::steady_clock::now() - window;
    int count = 0;
    for (auto it = this->recent_exit_order_times.rbegin();
         it != this->recent_exit_order_times.rend(); ++it) {
        if (*it >= cutoff) {
            ++count;
        } else {
            break;
        }
    }
    return count;
}

int Core::TradeTicker::get_recent_exit_orders_count_10s() const {
    return this->get_recent_exit_orders_count(std::chrono::seconds(10));
}

int Core::TradeTicker::get_recent_enter_orders_count(
    std::chrono::seconds window) const {
    const auto cutoff = std::chrono::steady_clock::now() - window;
    int count = 0;
    for (auto it = this->recent_enter_order_times.rbegin();
         it != this->recent_enter_order_times.rend(); ++it) {
        if (*it >= cutoff) {
            ++count;
        } else {
            break;
        }
    }
    return count;
}

int Core::TradeTicker::get_recent_enter_orders_count_10s() const {
    return this->get_recent_enter_orders_count(std::chrono::seconds(10));
}

void Core::TradeTicker::ack_signal_ready_for_trading(
    const Core::Signal &signal) {
    if (this->processed_signal_timestamps.find(signal.event_time_ms) !=
        this->processed_signal_timestamps.end()) {
        return;
    }
    this->processed_signal_timestamps.insert(signal.event_time_ms);

    Core::Engine::getInstance().create_system_log(
        this->symbol, LOG_LEVEL_INFO,
        "Signal ready for trading: " + std::to_string(signal.bar_close_time_ms),
        Core::TRADING_LOG_EVENT_TYPE_SIGNAL_PROCESSED, signal.to_json(),
        this->strategy_id);
}

bool Core::TradeTicker::has_hedge_order_for_trade(const Trade &trade) {
    for (const auto &order : this->orders) {
        if (order.hedge_for_trade_id.has_value() &&
            order.hedge_for_trade_id.value() == trade.id) {
            return true;
        }
    }
    return false;
}

bool Core::TradeTicker::has_directional_trade_for_hedge_trade(
    const Trade &trade) {
    if (!trade.hedge_for_trade_id.has_value()) {
        throw std::runtime_error(
            "has_directional_trade_for_hedge_trade: Hedge for trade id is not "
            "set");
    }

    for (const auto &trade_2nd : this->active_trades) {
        if (trade_2nd.type_internal ==
                Core::TradeTypeInternal::DIRECTIONAL_TRADE &&
            trade_2nd.id == trade.hedge_for_trade_id.value()) {
            return true;
        }
    }
    return false;
}

mpfr::mpreal Core::TradeTicker::get_trades_hedge_sizing_coeff_deltas(
    const Core::Trade &trade) {
    mpfr::mpreal ret = MPREAL_ZERO;
    for (const auto &trade_2nd : this->active_trades) {
        if (trade_2nd.type_internal == Core::TradeTypeInternal::HEDGE &&
            trade_2nd.hedge_for_trade_id.has_value() &&
            trade_2nd.hedge_for_trade_id.value() == trade.id) {
            ret += trade_2nd.sizing_coeff_delta *
                   (1 - (trade_2nd.closed_quantity / trade_2nd.open_quantity));
        }
    }

    for (const auto &order : this->orders) {
        if (order.hedge_for_trade_id.has_value() &&
            order.hedge_for_trade_id.value() == trade.id) {
            ret += order.sizing_coeff_delta *
                   (1 - (order.filled_quantity / order.quantity));
        }
    }
    return ret;
}

mpfr::mpreal Core::TradeTicker::get_directional_trades_sizing_coeff_delta(
    const Core::Trade &trade) {
    return trade.sizing_coeff_delta *
           (1 - (trade.closed_quantity / trade.open_quantity));
}

bool Core::TradeTicker::hedge_trade_or_order_exists_for_symbol(
    const Core::Trade &directional_trade, const std::string &symbol) {
    for (const auto &trade : this->active_trades) {
        if (trade.type_internal == Core::TradeTypeInternal::HEDGE &&
            trade.hedge_for_trade_id.has_value() &&
            trade.hedge_for_trade_id.value() == directional_trade.id &&
            trade.symbol == symbol) {
            return true;
        }
    }
    for (const auto &order : this->orders) {
        if (order.hedge_for_trade_id.has_value() &&
            order.hedge_for_trade_id.value() == directional_trade.id &&
            order.route_order_to_alt_symbol.has_value() &&
            order.route_order_to_alt_symbol.value() == symbol) {
            return true;
        }
    }
    return false;
}

bool Core::TradeTicker::trade_has_order_on_book(unsigned long long trade_id) {
    for (const auto &order : this->orders) {
        if (order.trade_id.has_value() && order.trade_id.value() == trade_id) {
            return true;
        }
    }
    return false;
}

void Core::TradeTicker::mark_last_stop_loss_triggered_time(Engine &engine) {
    this->last_stop_loss_triggered_time_ms.store(engine.get_time());
}

bool Core::TradeTicker::bid_order_exists_on_book(double price) {
    auto it = this->bid_order_count_by_price.find(price);
    return it != this->bid_order_count_by_price.end() && it->second > 0;
}

bool Core::TradeTicker::ask_order_exists_on_book(double price) {
    auto it = this->ask_order_count_by_price.find(price);
    return it != this->ask_order_count_by_price.end() && it->second > 0;
}

std::string Core::TradeTicker::get_clob_price_key(
    const std::string &clob_id, const std::string &price_str) {
    return clob_id + ":" + price_str;
}

bool Core::TradeTicker::bid_order_exists_on_book(
    const std::string &clob_price_key) {
    auto it = this->bid_order_count_by_clob_price.find(clob_price_key);
    return it != this->bid_order_count_by_clob_price.end() && it->second > 0;
}

bool Core::TradeTicker::ask_order_exists_on_book(
    const std::string &clob_price_key) {
    auto it = this->ask_order_count_by_clob_price.find(clob_price_key);
    return it != this->ask_order_count_by_clob_price.end() && it->second > 0;
}

void Core::TradeTicker::set_trade_kv_cache(
    const Trade &trade,
    const std::unordered_map<std::string, double> &kv_cache) {
    for (auto &trade_2nd : this->active_trades) {
        if (trade_2nd.id == trade.id) {
            trade_2nd.kv_cache = kv_cache;
        }
    }
}

mpfr::mpreal Core::TradeTicker::get_max_available_qty_after_inventory_state(
    const Trade &trade, Engine &engine) {
    const auto inventory = engine.get_inventory(
        trade.exchange_name, trade.market_type, trade.symbol, trade.account_id);
    if (!inventory.has_value()) {
        return 0;
    }

    const auto price =
        engine.get_price(trade.symbol, trade.exchange_name, trade.market_type);
    if (!price.has_value()) {
        return 0;
    }

    const auto quantity_for_1usd_of_asset = 1.0 / price.value();
    const auto inventory_free = inventory->free - quantity_for_1usd_of_asset;
    const auto trade_remaining_qty = this->get_trade_remaining_qty(trade);
    return std::min(inventory_free, trade_remaining_qty);
}

std::optional<Core::Signal> Core::TradeTicker::get_last_actionable_signal() {
    if (!this->is_signal_actionable.load()) {
        return std::nullopt;
    }
    std::lock_guard<std::mutex> lock(this->trading_mutex);
    return this->curr_signal;
}

std::optional<Core::Signal> Core::TradeTicker::get_last_signal() {
    std::lock_guard<std::mutex> lock(this->trading_mutex);
    return this->curr_signal;
}

void Core::TradeTicker::recalculate_order_book_state() {
    std::lock_guard<std::mutex> lock(this->trading_mutex);

    this->active_bid_count = 0;
    this->active_ask_count = 0;
    this->active_bid_notional = 0.0;
    this->active_ask_notional = 0.0;
    this->bid_order_count_by_price.clear();
    this->ask_order_count_by_price.clear();
    this->bid_order_count_by_clob_price.clear();
    this->ask_order_count_by_clob_price.clear();

    int unacked_enter_count = 0;
    int unacked_exit_count = 0;
    for (const auto &order : this->orders) {
        const double remaining_qty =
            (order.quantity - order.filled_quantity).toDouble();
        const double notional = order.price.toDouble() * remaining_qty;

        if (order.side == Core::OrderSide::BUY) {
            this->bid_order_count_by_price[order.price.toDouble()] += 1;
            this->active_bid_count += 1;
            this->active_bid_notional += notional;
            if (order.is_prediction_market_order && !order.clob_id.empty()) {
                this->bid_order_count_by_clob_price[get_clob_price_key(
                    order.clob_id, order.price_str)] += 1;
            }
        } else {
            this->ask_order_count_by_price[order.price.toDouble()] += 1;
            this->active_ask_count += 1;
            this->active_ask_notional += notional;
            if (order.is_prediction_market_order && !order.clob_id.empty()) {
                this->ask_order_count_by_clob_price[get_clob_price_key(
                    order.clob_id, order.price_str)] += 1;
            }
        }

        if (!order.is_order_creation_acknowledged) {
            if (order.is_trade_close_order) {
                unacked_exit_count += 1;
            } else {
                unacked_enter_count += 1;
            }
        }
    }

    this->order_count.store(static_cast<int>(this->orders.size()),
                            std::memory_order_relaxed);
    this->unacked_enter_order_count.store(unacked_enter_count,
                                          std::memory_order_relaxed);
    this->unacked_exit_order_count.store(unacked_exit_count,
                                         std::memory_order_relaxed);
}

double Core::TradeTicker::round_polymarket_price(double price,
                                                 bool round_up) const {
    double tick = this->order_price_min_tick_size.load();
    if (tick <= 0) {
        return price;
    }
    double ticks = price / tick;
    double rounded_ticks =
        round_up ? std::ceil(ticks - 1e-6) : std::floor(ticks + 1e-6);
    return rounded_ticks * tick;
}

double Core::TradeTicker::round_polymarket_quantity(double quantity) const {
    return std::floor(quantity);
}

void Core::TradeTicker::on_create_orders_bulk(Engine &engine,
                                              std::vector<Order *> &orders) {
    // default implementation for virtual method
    for (auto *order : orders) {
        this->on_create_order(engine, *order);
    }
}

void Core::TradeTicker::on_cancel_orders_bulk(Engine &engine,
                                              std::vector<Order *> &orders) {
    // default implementation for virtual method
    for (auto *order : orders) {
        this->on_cancel_order(engine, *order);
    }
}

void Core::TradeTicker::round_and_stringify_order(Engine &engine,
                                                  Order &order) const {
    if (this->is_trading_on_prediction_market.load()) {
        bool round_up = (order.side == OrderSide::SELL);
        double rounded_price =
            this->round_polymarket_price(order.price.toDouble(), round_up);
        double rounded_quantity =
            this->round_polymarket_quantity(order.quantity.toDouble());

        order.price = mpfr::mpreal(rounded_price);
        order.quantity = mpfr::mpreal(rounded_quantity);

        int price_precision = StrMath::step_size_to_precision(
            this->order_price_min_tick_size.load());
        order.price_str =
            StrMath::to_fixed_string(order.price, price_precision);
        order.quantity_str = StrMath::to_fixed_string(order.quantity, 0);
    } else if (engine.is_backtest) {
        order.price_str = order.price.toString();
        order.quantity_str = order.quantity.toString();
    } else {
        engine.round_quantity_and_price(order);
    }
}

void Core::TradeTicker::confirm_polymarket_fills(
    Engine &engine, const std::string &taker_order_id) {
    int confirmed_count = 0;
    int already_finalized_count = 0;
    std::vector<unsigned long long> ttl_evicted_trade_ids;

    {
        std::lock_guard<std::mutex> lock(this->trading_mutex);

        for (auto &trade : this->active_trades) {
            for (auto &fill : trade.open_fills) {
                if (fill.taker_order_id == taker_order_id) {
                    if (fill.is_polymarket_fill_confirmed) {
                        already_finalized_count++;
                    } else {
                        fill.is_polymarket_fill_confirmed = true;
                        confirmed_count++;
                    }
                }
            }
            for (auto &fill : trade.close_fills) {
                if (fill.taker_order_id == taker_order_id) {
                    if (fill.is_polymarket_fill_confirmed) {
                        already_finalized_count++;
                    } else {
                        fill.is_polymarket_fill_confirmed = true;
                        confirmed_count++;
                    }
                }
            }
        }

        const long long now_ms = DatetimeUtils::get_current_time_ms();

        for (auto it = this->recently_completed_polymarket_trades.begin();
             it != this->recently_completed_polymarket_trades.end();) {
            if (now_ms - it->close_time_ms > POLYMARKET_ROLLBACK_TTL_MS) {
                ttl_evicted_trade_ids.push_back(it->id);
                it = this->recently_completed_polymarket_trades.erase(it);
                continue;
            }

            for (auto &fill : it->open_fills) {
                if (fill.taker_order_id == taker_order_id) {
                    if (fill.is_polymarket_fill_confirmed) {
                        already_finalized_count++;
                    } else {
                        fill.is_polymarket_fill_confirmed = true;
                        confirmed_count++;
                    }
                }
            }
            for (auto &fill : it->close_fills) {
                if (fill.taker_order_id == taker_order_id) {
                    if (fill.is_polymarket_fill_confirmed) {
                        already_finalized_count++;
                    } else {
                        fill.is_polymarket_fill_confirmed = true;
                        confirmed_count++;
                    }
                }
            }

            if (!it->has_unfinalized_fills()) {
                it = this->recently_completed_polymarket_trades.erase(it);
            } else {
                ++it;
            }
        }
    }

    for (auto trade_id : ttl_evicted_trade_ids) {
        engine.create_system_log(
            this->symbol, LOG_LEVEL_ERROR,
            "confirm_polymarket_fills: TTL-evicted stale rollback trade " +
                std::to_string(trade_id) +
                " (no finalization received within " +
                std::to_string(POLYMARKET_ROLLBACK_TTL_MS / 1000) + "s)",
            Core::TRADING_LOG_EVENT_TYPE_POLYMARKET_FILL_CONFIRMATION_TIMEOUT,
            nlohmann::json(), this->strategy_id);
    }

    if (already_finalized_count > 0) {
        engine.create_system_log(
            this->symbol, LOG_LEVEL_ERROR,
            "confirm_polymarket_fills: " +
                std::to_string(already_finalized_count) +
                " fill(s) already finalized for taker_order_id=" +
                taker_order_id,
            Core::TRADING_LOG_EVENT_TYPE_POLYMARKET_ORDER_STATE_VERIFY,
            nlohmann::json(), this->strategy_id);
    }
    if (confirmed_count == 0 && already_finalized_count == 0) {
        engine.create_system_log(
            this->symbol, LOG_LEVEL_ERROR,
            "confirm_polymarket_fills: no fills found for taker_order_id=" +
                taker_order_id,
            Core::TRADING_LOG_EVENT_TYPE_POLYMARKET_ORDER_STATE_VERIFY,
            nlohmann::json(), this->strategy_id);
    }
}

void Core::TradeTicker::revert_polymarket_fills(
    Engine &engine, const std::string &taker_order_id) {
    std::vector<unsigned long long> trades_to_remove_from_active;
    std::vector<Core::Trade> trades_to_restore;
    bool found_any = false;

    {
        std::lock_guard<std::mutex> lock(this->trading_mutex);

        for (auto &trade : this->active_trades) {
            std::vector<OrderFillInfo> enter_fills_to_revert;
            std::vector<OrderFillInfo> exit_fills_to_revert;

            for (const auto &fill : trade.open_fills) {
                if (fill.taker_order_id == taker_order_id &&
                    !fill.is_polymarket_fill_confirmed) {
                    enter_fills_to_revert.push_back(fill);
                }
            }
            for (const auto &fill : trade.close_fills) {
                if (fill.taker_order_id == taker_order_id &&
                    !fill.is_polymarket_fill_confirmed) {
                    exit_fills_to_revert.push_back(fill);
                }
            }

            if (!enter_fills_to_revert.empty() ||
                !exit_fills_to_revert.empty()) {
                found_any = true;
            }

            for (const auto &fill : exit_fills_to_revert) {
                trade.revert_exit_fill(fill);
            }
            for (const auto &fill : enter_fills_to_revert) {
                trade.revert_enter_fill(fill);
                if (trade.count_open_fills() == 0) {
                    trades_to_remove_from_active.push_back(trade.id);
                }
            }
        }

        for (auto trade_id : trades_to_remove_from_active) {
            this->active_trades.erase(
                std::remove_if(
                    this->active_trades.begin(), this->active_trades.end(),
                    [trade_id](const Trade &t) { return t.id == trade_id; }),
                this->active_trades.end());
        }
        if (!trades_to_remove_from_active.empty()) {
            this->active_trade_count.store(
                static_cast<int>(this->active_trades.size()),
                std::memory_order_relaxed);
        }

        for (auto it = this->recently_completed_polymarket_trades.begin();
             it != this->recently_completed_polymarket_trades.end();) {
            std::vector<OrderFillInfo> enter_fills_to_revert;
            std::vector<OrderFillInfo> exit_fills_to_revert;

            for (const auto &fill : it->open_fills) {
                if (fill.taker_order_id == taker_order_id &&
                    !fill.is_polymarket_fill_confirmed) {
                    enter_fills_to_revert.push_back(fill);
                }
            }
            for (const auto &fill : it->close_fills) {
                if (fill.taker_order_id == taker_order_id &&
                    !fill.is_polymarket_fill_confirmed) {
                    exit_fills_to_revert.push_back(fill);
                }
            }

            if (enter_fills_to_revert.empty() && exit_fills_to_revert.empty()) {
                ++it;
                continue;
            }

            found_any = true;

            for (const auto &fill : exit_fills_to_revert) {
                it->revert_exit_fill(fill);
            }
            for (const auto &fill : enter_fills_to_revert) {
                it->revert_enter_fill(fill);
            }

            if (it->count_open_fills() == 0) {
                it = this->recently_completed_polymarket_trades.erase(it);
            } else {
                trades_to_restore.push_back(*it);
                it = this->recently_completed_polymarket_trades.erase(it);
                this->active_trades.push_back(trades_to_restore.back());
                this->active_trade_count.store(
                    static_cast<int>(this->active_trades.size()),
                    std::memory_order_relaxed);
            }
        }
    }

    if (!trades_to_remove_from_active.empty()) {
        for (auto trade_id : trades_to_remove_from_active) {
            engine.remove_trade_from_risk_engine(trade_id);
        }
        engine.create_system_log(
            this->symbol, LOG_LEVEL_INFO,
            "Reverted polymarket enter fills, removed " +
                std::to_string(trades_to_remove_from_active.size()) +
                " trade(s)",
            Core::TRADING_LOG_EVENT_TYPE_POLYMARKET_FILL_REVERTED,
            nlohmann::json(), this->strategy_id);
    }

    for (const auto &restored : trades_to_restore) {
        engine.update_strategy_virtual_balance_revert(restored);
        engine.add_trade_to_risk_engine(restored);
        engine.create_system_log(
            this->symbol, LOG_LEVEL_INFO,
            "Reverted polymarket exit fill, restored trade " +
                std::to_string(restored.id) + " to active",
            Core::TRADING_LOG_EVENT_TYPE_POLYMARKET_FILL_REVERTED,
            restored.to_json(), this->strategy_id);
    }

    if (!found_any) {
        engine.create_system_log(
            this->symbol, LOG_LEVEL_ERROR,
            "revert_polymarket_fills: no unfinalized fills found for "
            "taker_order_id=" +
                taker_order_id + " (already confirmed or already reverted)",
            Core::TRADING_LOG_EVENT_TYPE_POLYMARKET_ORDER_STATE_VERIFY,
            nlohmann::json(), this->strategy_id);
    }
}

long long Core::TradeTicker::get_time_elapsed_since_event_start_ms() const {
    return Core::Engine::getInstance().get_time() -
           this->event_start_time_ms.load();
}

long long Core::TradeTicker::get_time_left_in_event_ms() const {
    return this->event_end_time_ms.load() -
           Core::Engine::getInstance().get_time();
}