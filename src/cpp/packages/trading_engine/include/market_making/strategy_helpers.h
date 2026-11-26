#pragma once

#include <optional>

#include "market_making/dtypes/strategy_decision.h"
#include "market_making/engine.h"
#include "market_making/strategy.h"
#include "market_making/ticker.h"

namespace Core::StrategyHelpers {

constexpr long long kDefaultOrderExpiryMs = 60LL * 60 * 1000;

inline bool submit_market_order(Engine& engine, Strategy& strategy,
                                TradeTicker& ticker, OrderSide side,
                                double ref_price, const mpfr::mpreal& qty,
                                StrategyDecision& decision,
                                std::optional<unsigned long long> close_trade_id =
                                    std::nullopt) {
    if (ticker.exchange_and_markets.empty() || qty <= 0) return false;
    const auto& em = ticker.exchange_and_markets.front();
    const long long now_ms = engine.get_time();
    const bool is_close = close_trade_id.has_value();

    CreateOrderParams params{
        .symbol = ticker.symbol,
        .type = OrderType::MARKET,
        .side = side,
        .price = mpfr::mpreal(ref_price),
        .prediction = std::nullopt,
        .quantity = qty,
        .exchange_name = em.exchange_name,
        .market_type = em.market_type,
        .strategy_id = strategy.strategy_config.id,
        .expires_on_ms = now_ms + kDefaultOrderExpiryMs,
        .sizing_coeff_delta = mpfr::mpreal("1.0"),
        .trade_id = close_trade_id,
        .reduce_only = is_close,
        .timestamp = now_ms,
        .quote_asset = ticker.quote_asset,
        .base_asset = ticker.base_asset,
        .allowed_to_bypass_risk_engine = false,
        .must_pass_min_notional_filter = false,
        .is_trade_close_order = is_close,
        .prevent_overlapping_orders = !is_close,
        .apply_cooldown_after_failed_order = false,
        .max_unacked_enter_orders = std::nullopt,
    };

    auto [res, order] = ticker.construct_order(engine, strategy, params);
    if (res != OrderConstructionResult::VALID || !order.has_value()) {
        return false;
    }
    auto o = order.value();
    o.post_only = false;
    ticker.mark_order_to_be_created(engine, o, decision, strategy);
    return true;
}

inline bool close_trade_market(Engine& engine, Strategy& strategy,
                               TradeTicker& ticker, const Trade& trade,
                               double ref_price, StrategyDecision& decision) {
    const mpfr::mpreal remaining = trade.open_quantity - trade.closed_quantity;
    const OrderSide close_side =
        trade.type == TradeType::LONG ? OrderSide::SELL : OrderSide::BUY;
    return submit_market_order(engine, strategy, ticker, close_side, ref_price,
                               remaining, decision, trade.id);
}

}  // namespace Core::StrategyHelpers
