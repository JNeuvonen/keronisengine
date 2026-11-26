#include "market_making/strategy_helpers.h"
#include "market_making/strategy_registry.h"

#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "market_data/bar_close_policy.h"

namespace {

constexpr size_t kFastWindow = 5;
constexpr size_t kSlowWindow = 20;
constexpr double kOrderQty = 0.01;
constexpr long long kBarIntervalMs = 60'000;

struct SymbolWindow {
    long long last_sample_ms = -1;
    std::deque<double> closes;
    std::optional<double> prev_fast, prev_slow;
};

std::mutex g_mu;
std::unordered_map<std::string, SymbolWindow> g_windows;

double sma(const std::deque<double>& v, size_t n) {
    double sum = 0.0;
    for (size_t i = v.size() - n; i < v.size(); ++i) sum += v[i];
    return sum / static_cast<double>(n);
}

std::vector<Core::StrategyDecision> on_market_event(
    Core::Engine& engine, Core::Strategy& strategy,
    std::optional<Core::MarketEvent>) {
    std::vector<Core::StrategyDecision> decisions;
    const long long now_ms = engine.get_time();

    for (auto& [symbol, ticker_ptr] : strategy.trade_tickers) {
        std::lock_guard<std::mutex> lock(ticker_ptr->trading_mutex);
        if (ticker_ptr->exchange_and_markets.empty()) continue;

        const auto& em = ticker_ptr->exchange_and_markets.front();
        const auto price = engine.get_price(symbol, em.exchange_name, em.market_type);
        if (!price.has_value() || *price <= 0.0) continue;

        bool crossed_up = false, crossed_down = false;
        {
            std::lock_guard<std::mutex> hlock(g_mu);
            auto& w = g_windows[std::to_string(strategy.strategy_config.id) + ":" + symbol];
            if (w.last_sample_ms >= 0 && now_ms - w.last_sample_ms < kBarIntervalMs) continue;
            w.last_sample_ms = now_ms;
            w.closes.push_back(*price);
            if (w.closes.size() > kSlowWindow) w.closes.pop_front();
            if (w.closes.size() < kSlowWindow) continue;

            const double fast = sma(w.closes, kFastWindow);
            const double slow = sma(w.closes, kSlowWindow);
            if (w.prev_fast && w.prev_slow) {
                crossed_up = (*w.prev_fast <= *w.prev_slow) && (fast > slow);
                crossed_down = (*w.prev_fast >= *w.prev_slow) && (fast < slow);
            }
            w.prev_fast = fast;
            w.prev_slow = slow;
        }

        if (!ticker_ptr->get_orders(false).empty()) continue;
        const auto trades = ticker_ptr->get_trades(false);

        Core::StrategyDecision decision;
        decision.symbol = symbol;
        if (trades.empty() && crossed_up) {
            Core::StrategyHelpers::submit_market_order(
                engine, strategy, *ticker_ptr, Core::OrderSide::BUY, *price,
                mpfr::mpreal(kOrderQty), decision);
        } else if (!trades.empty() && crossed_down) {
            Core::StrategyHelpers::close_trade_market(
                engine, strategy, *ticker_ptr, trades.front(), *price, decision);
        }
        if (!decision.orders.empty()) decisions.push_back(std::move(decision));
    }
    return decisions;
}

void bind_sma_crossover(Core::Strategy& strategy) {
    strategy.strategy_callback = on_market_event;
    strategy.compute_predictions_callback = [](Core::Engine&, Core::Strategy&) {};
    strategy.required_bar_types = {MarketData::BarClosePolicy::time_bars(kBarIntervalMs)};
}

}  // namespace

KERONIS_REGISTER_STRATEGY("sma_crossover", bind_sma_crossover);
