#include "market_making/strategy_registry.h"
#include "strategies/test_quoter/test_quoter.h"

#include <mutex>
#include <optional>
#include <string>

#include "market_making/dtypes/strategy_decision.h"
#include "market_making/ticker.h"

namespace {

// One-off test order sizing. Scenarios that need other sizes should define
// a second test strategy — keep test_quoter trivial.
constexpr double TEST_QUOTER_ORDER_QTY = 0.001;
constexpr double TEST_QUOTER_PRICE_OFFSET_TICKS = 1.0;
// Default tick size when exchange_info isn't yet loaded (BTC/USDT:USDT).
constexpr double TEST_QUOTER_FALLBACK_TICK = 0.1;
// 24h order expiry — well past any test scenario's horizon.
constexpr long long TEST_QUOTER_ORDER_EXPIRY_MS = 24LL * 60 * 60 * 1000;

}  // namespace

std::vector<MarketData::BarClosePolicy>
AlgorithmTestQuoter::get_strategy_required_bar_types() {
    return {};
}

std::vector<Core::ExchangeAndMarket>
AlgorithmTestQuoter::get_signal_exchanges_list() {
    // No signal-source exchanges — the strategy reacts purely to its
    // trade-venue BBA. The scenario harness drives BBA via
    // ``/control/engine/bba`` (engine control server), which invokes the
    // market-event callback below.
    return {};
}

void AlgorithmTestQuoter::compute_predictions(Core::Engine& /*engine*/,
                                              Core::Strategy& /*strategy*/) {
    // No model; no predictions. Callback exists because
    // ``Core::Strategy::compute_predictions_callback`` is non-optional at
    // strategy-wire-up time.
}

std::vector<Core::StrategyDecision>
AlgorithmTestQuoter::on_market_event_callback(
    Core::Engine& engine, Core::Strategy& strategy,
    std::optional<Core::MarketEvent> /*market_event*/) {
    std::vector<Core::StrategyDecision> decisions;
    const long long now_ms = engine.get_time();

    for (auto& [symbol, ticker_ptr] : strategy.trade_tickers) {
        std::lock_guard<std::mutex> lock(ticker_ptr->trading_mutex);

        Core::StrategyDecision decision;
        decision.symbol = symbol;

        // Single-order-per-ticker gate: if any active order exists, skip.
        // Keeps the test surface deterministic — one submit per empty
        // ticker per BBA tick, no re-quoting.
        const auto existing_orders = ticker_ptr->get_orders(false);
        if (!existing_orders.empty()) continue;

        // Venue-specific BBA lookup. ticker's exchange_and_markets is the
        // set of (exchange, market) pairs this ticker trades on — pick the
        // first (test strategies run on exactly one venue).
        if (ticker_ptr->exchange_and_markets.empty()) continue;
        const auto& em = ticker_ptr->exchange_and_markets.front();
        const auto bba = engine.get_bba(symbol, em.exchange_name,
                                        em.market_type);
        if (!bba.has_value()) continue;

        const double best_bid = bba->best_bid_price;
        if (best_bid <= 0.0) continue;

        // Post one tick below best bid — guaranteed to rest (non-marketable)
        // so the scenario can deterministically push a fill via
        // ``/control/push_fill``.
        double tick_size = engine.get_tick_size(symbol, em.exchange_name,
                                                em.market_type);
        if (tick_size <= 0.0) tick_size = TEST_QUOTER_FALLBACK_TICK;
        const double price =
            best_bid - (TEST_QUOTER_PRICE_OFFSET_TICKS * tick_size);
        if (price <= 0.0) continue;

        Core::CreateOrderParams params{
            .symbol = symbol,
            .type = Core::OrderType::LIMIT,
            .side = Core::OrderSide::BUY,
            .price = mpfr::mpreal(price),
            .prediction = std::nullopt,
            .quantity = mpfr::mpreal(TEST_QUOTER_ORDER_QTY),
            .exchange_name = em.exchange_name,
            .market_type = em.market_type,
            .strategy_id = strategy.strategy_config.id,
            .expires_on_ms = now_ms + TEST_QUOTER_ORDER_EXPIRY_MS,
            .sizing_coeff_delta = mpfr::mpreal("1.0"),
            .trade_id = std::nullopt,
            .reduce_only = false,
            .timestamp = now_ms,
            .quote_asset = ticker_ptr->quote_asset,
            .base_asset = ticker_ptr->base_asset,
            .allowed_to_bypass_risk_engine = false,
            .must_pass_min_notional_filter = false,
            .is_trade_close_order = false,
            .prevent_overlapping_orders = true,
            .apply_cooldown_after_failed_order = false,
            .max_unacked_enter_orders = std::nullopt,
        };

        auto [result, order] =
            ticker_ptr->construct_order(engine, strategy, params);
        if (result == Core::OrderConstructionResult::VALID &&
            order.has_value()) {
            auto o = order.value();
            o.post_only = false;  // regular limit — scenarios want a fill
            ticker_ptr->mark_order_to_be_created(engine, o, decision, strategy);
        }

        if (!decision.orders.empty()) {
            decisions.push_back(decision);
        }
    }

    return decisions;
}

namespace {
void bind_test_quoter(Core::Strategy& strategy) {
    strategy.strategy_callback = AlgorithmTestQuoter::on_market_event_callback;
    strategy.compute_predictions_callback = AlgorithmTestQuoter::compute_predictions;
    strategy.required_bar_types = AlgorithmTestQuoter::get_strategy_required_bar_types();
    strategy.signal_exchanges = AlgorithmTestQuoter::get_signal_exchanges_list();
}
}  // namespace

KERONIS_REGISTER_STRATEGY("test_quoter", bind_test_quoter);
