# KeronisEngine

**Focus on finding alpha. Let KeronisEngine handle the execution.**

KeronisEngine is an open-source C++20 trading engine. Write a strategy and its streaming features once in C++, then run the exact same code across historical feature generation, backtesting, paper trading, and live execution.

## Why I built this

Most solo quants and small trading teams spend 80% of their time on plumbing: WebSocket reconnects, order book maintenance, fill reconciliation, risk limits, and debugging why live features don't match the Python notebook that trained the model.

I built KeronisEngine over several years of live crypto trading so I could stop fighting infrastructure and spend my time on alpha. In production, a single ~$100 dedicated server (e.g. Hetzner) ingests all of Binance's live market data at 10,000+ messages/sec while maintaining order books, updating streaming C++ feature state machines, and running continuous in-process ONNX predictions.

You keep your alpha in your own private repository and compile it into the engine with one CMake flag (`-DKERONIS_STRATEGY_DIRS=...`)—no forking required.

## What writing a strategy looks like

```cpp
#include "market_making/strategy_helpers.h"
#include "market_making/strategy_registry.h"
#include "market_data/bar_close_policy.h"

namespace {
std::vector<Core::StrategyDecision> on_market_event(
    Core::Engine& engine, Core::Strategy& strategy,
    std::optional<Core::MarketEvent>) {
    std::vector<Core::StrategyDecision> decisions;
    for (auto& [symbol, ticker] : strategy.trade_tickers) {
        std::lock_guard<std::mutex> lock(ticker->trading_mutex);
        const auto& em = ticker->exchange_and_markets.front();
        const auto price = engine.get_price(symbol, em.exchange_name, em.market_type);
        if (!price || !ticker->get_orders(false).empty()) continue;

        Core::StrategyDecision decision;
        decision.symbol = symbol;
        const auto trades = ticker->get_trades(false);
        if (trades.empty() && should_enter(symbol, *price)) {
            Core::StrategyHelpers::submit_market_order(
                engine, strategy, *ticker, Core::OrderSide::BUY, *price,
                mpfr::mpreal(0.01), decision);
        } else if (!trades.empty() && should_exit(symbol, *price)) {
            Core::StrategyHelpers::close_trade_market(
                engine, strategy, *ticker, trades.front(), *price, decision);
        }
        if (!decision.orders.empty()) decisions.push_back(std::move(decision));
    }
    return decisions;
}

void bind_my_strategy(Core::Strategy& strategy) {
    strategy.strategy_callback = on_market_event;
    strategy.compute_predictions_callback = [](Core::Engine&, Core::Strategy&) {};
    strategy.required_bar_types = {MarketData::BarClosePolicy::time_bars(60'000)};
}
}  // namespace

KERONIS_REGISTER_STRATEGY("my_strategy", bind_my_strategy);
```

## The workflow: research to live

1. **Features (`historical_feature_gen`):** Write a C++ streaming feature state machine once. Replay historical Parquet data through it to generate training panels, then train any model (LightGBM, XGBoost, PyTorch) and export to `.onnx`.
2. **Backtest (`backtest_runner`):** Run your strategy, state machine, and `.onnx` model against historical Parquet data with realistic latency, fees, and leverage. Outputs `summary.json`, `trades.json`, and `equity_curve.json`.
3. **Paper & Live (`live_infra`):** Paper-trade against real live feeds using the built-in `PaperExchange` (models taker fills at ack + latency, maker fills against resting quotes and subsequent prints, and cancels), then switch to live execution with durable quote holds and crash recovery.

## Current status & limitations

Being upfront about what works today and what doesn't:

- **Works out of the box today:** Core engine, unit tests (225+ cases), streaming state machines, ONNX inference, offline `backtest_runner`, `historical_feature_gen`, and `PaperExchange` paper trading.
- **Platform:** Linux and C++20 only. There is no Python strategy API.
- **Live execution caveat:** Native order signing exists for Hyperliquid (bundled Rust signer), Aster, and Polymarket, and native market data exists for Lighter. However, live order submission currently still expects PostgreSQL, NATS, and an external proxy (`CCXT_PROXY_BASE_URL`, not published in this repo) for credential lookup and non-native venues like Binance/Bybit. A standalone file-based credential path is next.

## Next steps

- [Quickstart](quickstart.md) — run the tests and bundled `sma_crossover` backtest in minutes.
- [Strategies](strategies.md) — complete walkthrough of writing and registering external strategies.
- [State Machines & ONNX](state-machines.md) — eliminate train/serve skew with shared C++ features.
- [Architecture](architecture.md) — pop the hood: `RingBufferBook`, `live_infra` threading, standby handoffs, and fill models.
- [Comparison](comparison.md) — honest tradeoffs vs. NautilusTrader and HftBacktest.

Machine-readable Markdown for scripts and coding agents: [`/llms.txt`](https://engine.keronis.ai/llms.txt) and [`/llms-full.txt`](https://engine.keronis.ai/llms-full.txt).

!!! warning "Disclaimer"
    Apache-2.0 licensed, provided as-is without warranty. Always validate strategies in `backtest_runner` and `PaperExchange` before connecting live capital.
