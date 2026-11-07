#include "market_making/strategy_registry.h"
#include "strategies/test_market_maker/test_market_maker.h"

#include <mutex>
#include <optional>
#include <string>

#include "market_making/dtypes/strategy_decision.h"
#include "market_making/ticker.h"

namespace {

constexpr double TEST_MM_ORDER_QTY = 0.001;
constexpr double TEST_MM_PRICE_OFFSET_TICKS = 1.0;
constexpr double TEST_MM_FALLBACK_TICK = 0.1;
constexpr long long TEST_MM_ORDER_EXPIRY_MS = 24LL * 60 * 60 * 1000;

// Build CreateOrderParams — shared between buy + sell sides. Cuts the
// per-side block in half and avoids hand-mirroring the field list.
Core::CreateOrderParams build_params_(
    const std::string& symbol, Core::OrderSide side, double price,
    const Core::ExchangeAndMarket& em, Core::Strategy& strategy,
    Core::TradeTicker& ticker, long long now_ms) {
    return Core::CreateOrderParams{
        .symbol = symbol,
        .type = Core::OrderType::LIMIT,
        .side = side,
        .price = mpfr::mpreal(price),
        .prediction = std::nullopt,
        .quantity = mpfr::mpreal(TEST_MM_ORDER_QTY),
        .exchange_name = em.exchange_name,
        .market_type = em.market_type,
        .strategy_id = strategy.strategy_config.id,
        .expires_on_ms = now_ms + TEST_MM_ORDER_EXPIRY_MS,
        .sizing_coeff_delta = mpfr::mpreal("1.0"),
        .trade_id = std::nullopt,
        .reduce_only = false,
        .timestamp = now_ms,
        .quote_asset = ticker.quote_asset,
        .base_asset = ticker.base_asset,
        .allowed_to_bypass_risk_engine = false,
        .must_pass_min_notional_filter = false,
        .is_trade_close_order = false,
        .prevent_overlapping_orders = true,
        .apply_cooldown_after_failed_order = false,
        .max_unacked_enter_orders = std::nullopt,
    };
}

}  // namespace

std::vector<MarketData::BarClosePolicy>
AlgorithmTestMarketMaker::get_strategy_required_bar_types() {
    return {};
}

std::vector<Core::ExchangeAndMarket>
AlgorithmTestMarketMaker::get_signal_exchanges_list() {
    return {};
}

void AlgorithmTestMarketMaker::compute_predictions(
    Core::Engine& /*engine*/, Core::Strategy& /*strategy*/) {
    // No model.
}

std::vector<Core::StrategyDecision>
AlgorithmTestMarketMaker::on_market_event_callback(
    Core::Engine& engine, Core::Strategy& strategy,
    std::optional<Core::MarketEvent> /*market_event*/) {
    std::vector<Core::StrategyDecision> decisions;
    const long long now_ms = engine.get_time();

    for (auto& [symbol, ticker_ptr] : strategy.trade_tickers) {
        std::lock_guard<std::mutex> lock(ticker_ptr->trading_mutex);

        Core::StrategyDecision decision;
        decision.symbol = symbol;

        // Single-pair gate: if ANY active order exists, skip — keeps
        // the test deterministic at one pair per empty-ticker tick.
        const auto existing_orders = ticker_ptr->get_orders(false);
        if (!existing_orders.empty()) continue;

        if (ticker_ptr->exchange_and_markets.empty()) continue;
        const auto& em = ticker_ptr->exchange_and_markets.front();
        const auto bba = engine.get_bba(symbol, em.exchange_name,
                                        em.market_type);
        if (!bba.has_value()) continue;

        const double best_bid = bba->best_bid_price;
        const double best_ask = bba->best_ask_price;
        if (best_bid <= 0.0 || best_ask <= 0.0) continue;

        double tick_size = engine.get_tick_size(symbol, em.exchange_name,
                                                em.market_type);
        if (tick_size <= 0.0) tick_size = TEST_MM_FALLBACK_TICK;
        const double offset =
            TEST_MM_PRICE_OFFSET_TICKS * tick_size;

        // ── BUY one tick below best bid ────────────────────────────
        const double buy_price = best_bid - offset;
        if (buy_price > 0.0) {
            auto params = build_params_(
                symbol, Core::OrderSide::BUY, buy_price, em, strategy,
                *ticker_ptr, now_ms);
            auto [buy_result, buy_order] =
                ticker_ptr->construct_order(engine, strategy, params);
            if (buy_result == Core::OrderConstructionResult::VALID &&
                buy_order.has_value()) {
                auto o = buy_order.value();
                o.post_only = true;
                ticker_ptr->mark_order_to_be_created(
                    engine, o, decision, strategy);
            }
        }

        // ── SELL one tick above best ask ───────────────────────────
        const double sell_price = best_ask + offset;
        auto sell_params = build_params_(
            symbol, Core::OrderSide::SELL, sell_price, em, strategy,
            *ticker_ptr, now_ms);
        auto [sell_result, sell_order] =
            ticker_ptr->construct_order(engine, strategy, sell_params);
        if (sell_result == Core::OrderConstructionResult::VALID &&
            sell_order.has_value()) {
            auto o = sell_order.value();
            o.post_only = true;
            ticker_ptr->mark_order_to_be_created(engine, o, decision,
                                                  strategy);
        }

        if (!decision.orders.empty()) {
            decisions.push_back(decision);
        }
    }

    return decisions;
}

namespace {
void bind_test_market_maker(Core::Strategy& strategy) {
    strategy.strategy_callback = AlgorithmTestMarketMaker::on_market_event_callback;
    strategy.compute_predictions_callback = AlgorithmTestMarketMaker::compute_predictions;
    strategy.required_bar_types = AlgorithmTestMarketMaker::get_strategy_required_bar_types();
    strategy.signal_exchanges = AlgorithmTestMarketMaker::get_signal_exchanges_list();
}
}  // namespace

KERONIS_REGISTER_STRATEGY("test_market_maker", bind_test_market_maker);
