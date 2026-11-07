#include "strategies/common/equity_execution.h"

#include <algorithm>
#include <cstdio>
#include <ctime>

#include "chrono_utils.h"
#include "market_data/utils.h"

namespace EquityExecution {

std::string session_date_utc(long long time_ms) {
    const std::time_t seconds = static_cast<std::time_t>(time_ms / 1000);
    std::tm tm = ChronoUtils::gmtime_safe(seconds);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday);
    return std::string(buf);
}

std::optional<int> minutes_since_open(long long now_ms) {
    if (!MarketData::is_time_ms_within_real_trading_hours(now_ms)) {
        return std::nullopt;
    }
    const auto open_ms = MarketData::get_market_open_time_ms(now_ms);
    if (!open_ms.has_value()) return std::nullopt;
    return static_cast<int>((now_ms - open_ms.value()) / 60000);
}

bool has_open_position(Core::TradeTicker& trade_ticker) {
    for (const auto& trade : trade_ticker.get_trades(false)) {
        if (trade_ticker.get_trade_remaining_qty(trade) > 0) return true;
    }
    return false;
}

mpfr::mpreal open_quantity(Core::TradeTicker& trade_ticker) {
    mpfr::mpreal qty = 0;
    for (const auto& trade : trade_ticker.get_trades(false)) {
        const auto remaining = trade_ticker.get_trade_remaining_qty(trade);
        if (remaining > 0) qty += remaining;
    }
    return qty;
}

bool has_stale_trade(Core::TradeTicker& trade_ticker,
                     const std::string& session) {
    for (const auto& trade : trade_ticker.get_trades(false)) {
        if (trade_ticker.get_trade_remaining_qty(trade) <= 0) continue;
        if (session_date_utc(trade.open_time_ms) != session) return true;
    }
    return false;
}

void cancel_expired_orders(long long now_ms, Core::Engine& engine,
                           Core::Strategy& strategy,
                           Core::TradeTicker& trade_ticker,
                           Core::StrategyDecision& decision) {
    for (auto& order : trade_ticker.get_orders(false)) {
        if (order.expires_on_ms < now_ms &&
            !trade_ticker.is_cancel_already_sent(order)) {
            trade_ticker.mark_order_to_be_cancelled(engine, order, decision);
        }
    }
}

int count_live_orders(Core::TradeTicker& trade_ticker, long long now_ms,
                      std::optional<unsigned long long> trade_id) {
    int live = 0;
    for (const auto& order : trade_ticker.get_orders(false)) {
        if (trade_id.has_value()) {
            if (!order.is_trade_close_order || !order.trade_id.has_value() ||
                order.trade_id.value() != trade_id.value()) {
                continue;
            }
        } else if (order.is_trade_close_order) {
            continue;
        }
        if (order.expires_on_ms <= now_ms) continue;
        if (trade_ticker.is_cancel_already_sent(order)) continue;
        ++live;
    }
    return live;
}

bool post_entry(long long now_ms, Core::Engine& engine,
                Core::Strategy& strategy, Core::TradeTicker& trade_ticker,
                Core::StrategyDecision& decision, Core::OrderSide side,
                double limit_price, double notional, double sizing_coeff_delta,
                long long ttl_ms) {
    if (limit_price <= 0 || notional < kMinSliceNotional) return false;

    Core::CreateOrderParams params{
        .symbol = trade_ticker.symbol,
        .type = Core::OrderType::LIMIT,
        .side = side,
        .price = limit_price,
        .prediction = Core::Signal{},
        .quantity = mpfr::mpreal(notional / limit_price),
        .exchange_name = kExchange,
        .market_type = kMarketType,
        .strategy_id = strategy.strategy_config.id,
        .expires_on_ms = now_ms + ttl_ms,
        .sizing_coeff_delta = sizing_coeff_delta,
        .trade_id = std::nullopt,
        .reduce_only = false,
        .timestamp = now_ms,
        .quote_asset = trade_ticker.quote_asset,
        .base_asset = trade_ticker.base_asset,
        .allowed_to_bypass_risk_engine = false,
        .must_pass_min_notional_filter = true,
        .is_trade_close_order = false};

    auto [result, order] =
        trade_ticker.construct_order(engine, strategy, params);
    if (result != Core::OrderConstructionResult::VALID || !order.has_value()) {
        return false;
    }
    trade_ticker.mark_order_to_be_created(engine, order.value(), decision,
                                          strategy);
    return true;
}

int work_out_position(long long now_ms, Core::Engine& engine,
                       Core::Strategy& strategy,
                       Core::TradeTicker& trade_ticker,
                       Core::StrategyDecision& decision, double limit_price,
                       int slices, long long ttl_ms) {
    int posted = 0;
    if (limit_price <= 0) return posted;

    for (const auto& trade : trade_ticker.get_trades(false)) {
        const auto remaining = trade_ticker.get_trade_remaining_qty(trade);
        if (remaining <= 0) continue;

        const double notional = (remaining * limit_price).toDouble();
        int use_slices = static_cast<int>(notional / kMinSliceNotional);
        use_slices = std::max(1, std::min(slices, use_slices));

        const int live = count_live_orders(trade_ticker, now_ms, trade.id);
        const int to_post = use_slices - live;
        if (to_post <= 0) continue;
        const auto qty = remaining / use_slices;

        for (int k = 0; k < to_post; ++k) {
            Core::CreateOrderParams params{
                .symbol = trade_ticker.symbol,
                .type = Core::OrderType::LIMIT,
                .side = trade.type == Core::TradeType::SHORT
                            ? Core::OrderSide::BUY
                            : Core::OrderSide::SELL,
                .price = limit_price,
                .prediction = Core::Signal{},
                .quantity = qty,
                .exchange_name = kExchange,
                .market_type = kMarketType,
                .strategy_id = strategy.strategy_config.id,
                .expires_on_ms = now_ms + ttl_ms,
                .sizing_coeff_delta = qty / trade.open_quantity,
                .trade_id = trade.id,
                .reduce_only = false,
                .timestamp = now_ms,
                .quote_asset = trade_ticker.quote_asset,
                .base_asset = trade_ticker.base_asset,
                .allowed_to_bypass_risk_engine = true,
                .must_pass_min_notional_filter = true,
                .is_trade_close_order = true};

            auto [result, order] =
                trade_ticker.construct_order(engine, strategy, params);
            if (result != Core::OrderConstructionResult::VALID ||
                !order.has_value()) {
                break;
            }
            trade_ticker.mark_order_to_be_created(engine, order.value(),
                                                  decision, strategy);
            ++posted;
        }
    }
    return posted;
}

}  // namespace EquityExecution
