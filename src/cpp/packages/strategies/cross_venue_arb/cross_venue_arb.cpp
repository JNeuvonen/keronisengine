#include "market_making/strategy_helpers.h"
#include "market_making/strategy_registry.h"

#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace {

// Educational cross-venue lead-lag / spread arbitrage strategy.
// Compares a fast reference price on Binance perpetuals against the traded
// execution venue (e.g., Hyperliquid or Aster) and enters when the dislocation
// exceeds kEntrySpreadBps, exiting once the spread reverts below kExitSpreadBps.
constexpr double kEntrySpreadBps = 12.0;
constexpr double kExitSpreadBps = 2.0;
constexpr double kOrderQty = 0.01;

std::vector<Core::StrategyDecision> on_market_event(
    Core::Engine& engine, Core::Strategy& strategy,
    std::optional<Core::MarketEvent>) {
    std::vector<Core::StrategyDecision> decisions;

    for (auto& [symbol, ticker] : strategy.trade_tickers) {
        std::lock_guard<std::mutex> lock(ticker->trading_mutex);
        if (!ticker->get_orders(false).empty()) continue;
        if (ticker->exchange_and_markets.empty()) continue;

        const auto& em = ticker->exchange_and_markets.front();
        const auto exec_px =
            engine.get_price(symbol, em.exchange_name, em.market_type);
        const auto ref_px = engine.get_price(symbol, "binance", "swap");
        if (!exec_px || !ref_px || *exec_px <= 0.0 || *ref_px <= 0.0) continue;

        const double spread_bps = ((*ref_px - *exec_px) / *exec_px) * 10'000.0;
        const auto trades = ticker->get_trades(false);

        Core::StrategyDecision decision;
        decision.symbol = symbol;

        if (trades.empty() && spread_bps > kEntrySpreadBps) {
            Core::StrategyHelpers::submit_market_order(
                engine, strategy, *ticker, Core::OrderSide::BUY, *exec_px,
                mpfr::mpreal(kOrderQty), decision);
        } else if (!trades.empty() && spread_bps < kExitSpreadBps) {
            Core::StrategyHelpers::close_trade_market(
                engine, strategy, *ticker, trades.front(), *exec_px, decision);
        }

        if (!decision.orders.empty()) decisions.push_back(std::move(decision));
    }

    return decisions;
}

void bind_cross_venue_arb(Core::Strategy& strategy) {
    strategy.strategy_callback = on_market_event;
    strategy.compute_predictions_callback = [](Core::Engine&, Core::Strategy&) {};
    strategy.signal_exchanges = {
        {.exchange_name = "binance", .market_type = "swap", .account_id = 0},
    };
}

}  // namespace

KERONIS_REGISTER_STRATEGY("cross_venue_arb", bind_cross_venue_arb);
