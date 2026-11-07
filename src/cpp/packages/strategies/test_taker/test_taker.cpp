#include "market_making/strategy_registry.h"
#include "strategies/test_taker/test_taker.h"

#include <mutex>
#include <optional>
#include <string>

#include "market_making/dtypes/strategy_decision.h"
#include "market_making/ticker.h"

namespace {

constexpr double TEST_TAKER_ORDER_QTY = 0.001;
constexpr long long TEST_TAKER_ORDER_EXPIRY_MS = 24LL * 60 * 60 * 1000;

}  // namespace

std::vector<MarketData::BarClosePolicy>
AlgorithmTestTaker::get_strategy_required_bar_types() {
    return {};
}

std::vector<Core::ExchangeAndMarket>
AlgorithmTestTaker::get_signal_exchanges_list() {
    return {};
}

void AlgorithmTestTaker::compute_predictions(Core::Engine& /*engine*/,
                                              Core::Strategy& /*strategy*/) {
    // No model.
}

std::vector<Core::StrategyDecision>
AlgorithmTestTaker::on_market_event_callback(
    Core::Engine& engine, Core::Strategy& strategy,
    std::optional<Core::MarketEvent> /*market_event*/) {
    std::vector<Core::StrategyDecision> decisions;
    const long long now_ms = engine.get_time();

    for (auto& [symbol, ticker_ptr] : strategy.trade_tickers) {
        std::lock_guard<std::mutex> lock(ticker_ptr->trading_mutex);

        Core::StrategyDecision decision;
        decision.symbol = symbol;

        // Single-pending gate: if any unfilled order is still active,
        // skip. Filled MARKET orders aren't in this list (terminal
        // status), so subsequent BBA ticks naturally produce more
        // takes — that's the intended pattern for taker-flow scenarios.
        const auto existing_orders = ticker_ptr->get_orders(false);
        if (!existing_orders.empty()) continue;

        if (ticker_ptr->exchange_and_markets.empty()) continue;
        const auto& em = ticker_ptr->exchange_and_markets.front();
        const auto bba = engine.get_bba(symbol, em.exchange_name,
                                        em.market_type);
        if (!bba.has_value()) continue;

        const double best_ask = bba->best_ask_price;
        if (best_ask <= 0.0) continue;

        // MARKET orders carry a price for engine bookkeeping (slippage
        // limit, notional calc, etc.) but the matching engine ignores
        // it on execution — fills happen at simulator's book. Use the
        // current best_ask as a sane reference.
        Core::CreateOrderParams params{
            .symbol = symbol,
            .type = Core::OrderType::MARKET,
            .side = Core::OrderSide::BUY,
            .price = mpfr::mpreal(best_ask),
            .prediction = std::nullopt,
            .quantity = mpfr::mpreal(TEST_TAKER_ORDER_QTY),
            .exchange_name = em.exchange_name,
            .market_type = em.market_type,
            .strategy_id = strategy.strategy_config.id,
            .expires_on_ms = now_ms + TEST_TAKER_ORDER_EXPIRY_MS,
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
            o.post_only = false;  // taker by definition
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
void bind_test_taker(Core::Strategy& strategy) {
    strategy.strategy_callback = AlgorithmTestTaker::on_market_event_callback;
    strategy.compute_predictions_callback = AlgorithmTestTaker::compute_predictions;
    strategy.required_bar_types = AlgorithmTestTaker::get_strategy_required_bar_types();
    strategy.signal_exchanges = AlgorithmTestTaker::get_signal_exchanges_list();
}
}  // namespace

KERONIS_REGISTER_STRATEGY("test_taker", bind_test_taker);
