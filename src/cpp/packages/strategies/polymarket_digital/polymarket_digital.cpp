#include "market_making/strategy_registry.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <optional>
#include <string>

#include "market_making/dtypes/strategy_decision.h"
#include "market_making/engine.h"
#include "market_making/strategy.h"
#include "market_making/ticker.h"

namespace {

// Educational Polymarket short-dated digital options strategy.
// Prices P(S_T > K) from a live Binance reference price and annualized vol
// (replace fair_prob_yes() with OnnxModel::predict() for an ML probability),
// then buys YES or NO on the Polymarket CLOB when edge exceeds kMinEdge.
constexpr double kAnnualVol = 0.60;
constexpr double kMinEdge = 0.025;
constexpr double kOrderShares = 10.0;
constexpr double kMaxOutcomePos = 50.0;
constexpr long long kMinRemainingMs = 15'000;
constexpr long long kOrderExpiryMs = 30'000;
constexpr double kSecondsPerYear = 365.25 * 24.0 * 3600.0;

double normal_cdf(double x) {
    return 0.5 * std::erfc(-x * M_SQRT1_2);
}

double fair_prob_yes(double spot, double strike, double tau_sec) {
    const double tau_yr = tau_sec / kSecondsPerYear;
    const double denom = kAnnualVol * std::sqrt(tau_yr);
    if (denom <= 1e-9) return spot >= strike ? 1.0 : 0.0;
    return std::clamp(normal_cdf(std::log(spot / strike) / denom), 0.01, 0.99);
}

double round_to_tick(double price, double tick) {
    if (tick <= 0.0) tick = 0.01;
    return std::clamp(std::floor(price / tick) * tick, tick, 1.0 - tick);
}

std::vector<Core::StrategyDecision> on_market_event(
    Core::Engine& engine, Core::Strategy& strategy,
    std::optional<Core::MarketEvent>) {
    std::vector<Core::StrategyDecision> decisions;
    const long long now_ms = engine.get_time();

    auto tickers_lock = strategy.lock_on_pred_market_strat();
    for (auto& [symbol, ticker] : strategy.trade_tickers) {
        std::lock_guard<std::mutex> lock(ticker->trading_mutex);
        if (ticker->clob_ids_by_index.size() < 2) continue;
        if (ticker->underlying_symbol.empty() || ticker->strike_price <= 0.0) continue;
        if (!ticker->get_orders(false).empty()) continue;

        const long long remaining_ms = ticker->event_end_time_ms.load() - now_ms;
        if (remaining_ms < kMinRemainingMs) continue;

        const auto spot =
            engine.get_price(ticker->underlying_symbol, "binance", "swap");
        if (!spot || *spot <= 0.0) continue;

        const double p_yes =
            fair_prob_yes(*spot, ticker->strike_price, remaining_ms / 1000.0);
        const double fair[2] = {p_yes, 1.0 - p_yes};
        const double tick = ticker->order_price_min_tick_size.load();

        Core::StrategyDecision decision;
        decision.symbol = symbol;

        for (int outcome = 0; outcome < 2; ++outcome) {
            if (ticker->get_outcome_exchange_inventory(outcome) >= kMaxOutcomePos) {
                continue;
            }
            const auto bba = ticker->read_outcome_bba(outcome);
            if (!bba || bba->best_ask <= 0.0) continue;

            const double edge = fair[outcome] - bba->best_ask;
            if (edge < kMinEdge) continue;

            const double limit_px = round_to_tick(bba->best_ask, tick);
            Core::CreateOrderParams params{
                .symbol = symbol,
                .type = Core::OrderType::LIMIT,
                .side = Core::OrderSide::BUY,
                .price = mpfr::mpreal(limit_px),
                .prediction = std::nullopt,
                .quantity = mpfr::mpreal(kOrderShares),
                .exchange_name = "polymarket",
                .market_type = "spot",
                .strategy_id = strategy.strategy_config.id,
                .expires_on_ms = now_ms + kOrderExpiryMs,
                .sizing_coeff_delta = mpfr::mpreal("1.0"),
                .trade_id = std::nullopt,
                .reduce_only = false,
                .timestamp = now_ms,
                .quote_asset = ticker->quote_asset,
                .base_asset = ticker->base_asset,
                .allowed_to_bypass_risk_engine = false,
                .must_pass_min_notional_filter = false,
                .is_trade_close_order = false,
                .prevent_overlapping_orders = true,
                .apply_cooldown_after_failed_order = false,
                .max_unacked_enter_orders = std::nullopt,
            };

            auto [res, order] = ticker->construct_order(engine, strategy, params);
            if (res == Core::OrderConstructionResult::VALID && order.has_value()) {
                auto o = order.value();
                o.is_prediction_market_order = true;
                o.clob_id = ticker->clob_ids_by_index[outcome];
                o.outcome_name =
                    outcome < static_cast<int>(ticker->outcome_names_by_index.size())
                        ? ticker->outcome_names_by_index[outcome]
                        : (outcome == 0 ? "Yes" : "No");
                ticker->mark_order_to_be_created(engine, o, decision, strategy);
                break;
            }
        }

        if (!decision.orders.empty()) decisions.push_back(std::move(decision));
    }

    return decisions;
}

void bind_polymarket_digital(Core::Strategy& strategy) {
    strategy.strategy_callback = on_market_event;
    strategy.compute_predictions_callback = [](Core::Engine&, Core::Strategy&) {};
    strategy.signal_exchanges = {
        {.exchange_name = "binance", .market_type = "swap", .account_id = 0},
    };
}

}  // namespace

KERONIS_REGISTER_STRATEGY("polymarket_digital", bind_polymarket_digital);
