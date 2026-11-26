# KeronisEngine

KeronisEngine is an open-source C++20 execution and real-time ML inference engine for electronic markets. It runs streaming feature state machines, in-process ONNX Runtime models, order books, risk checks, and venue execution from a single binary—supporting sub-millisecond tick-to-prediction-to-order paths on commodity hardware.

Built for crypto spot/perpetuals and prediction markets (including native Polymarket CLOB execution and queue-notional backtesting), KeronisEngine uses modular market-data, ticker, and execution interfaces that extend cleanly to new exchanges, binary-outcome markets, or sports-betting venues.

## Core capabilities

| Capability | Details |
|---|---|
| **Sub-ms inference loop** | Flat tick-indexed `RingBufferBook` (~1.5 ns/op `apply_bid` on Core Ultra 9 285K), lock-free event queues, streaming C++ `StrategyStateMachine`s, and in-process `OnnxModel::predict()`. Handles full-exchange feeds (10,000+ msg/s across Binance) with continuous ONNX scoring on a single ~$100 Linux server. |
| **Train / backtest / live parity** | The same C++ state machines and strategy callbacks run in offline Parquet feature generation (`historical_feature_gen`), historical event replay (`backtest_runner`), depth-based paper trading (`PaperExchange`), and live trading (`live_infra`). |
| **Crypto & prediction markets** | Native execution for Hyperliquid (bundled Rust signer), Aster, and Polymarket (CLOB WebSockets, EIP-712 bulk signing, multi-outcome tickers, queue-notional backtesting). Native L2 market data for Lighter. Extensible to any order-book or event-driven venue (e.g. sports-betting exchanges). |
| **Out-of-tree plugins** | Keep proprietary alpha in a private repository. Register strategies (`KERONIS_REGISTER_STRATEGY`) and state machines (`KERONIS_REGISTER_STATE_MACHINE`) via `-DKERONIS_STRATEGY_DIRS=...` without modifying core engine code. |
| **Operational safety** | Durable `QuoteHolds` surviving process restarts, `quote_guard` heartbeat enforcement, cron standby handoff, disk crash recovery, high-precision MPFR accounting, and an embedded localhost HTTP control server. |

## Streaming features → ONNX → order

The core loop pairs a registered `StrategyStateMachine` with `OnnxModel` and `StrategyHelpers`:

```cpp
#include "market_data/state_machine_dispatcher.h"
#include "market_making/strategy_helpers.h"
#include "market_making/strategy_registry.h"
#include "onnx_model.h"

namespace {

OnnxModel& model() {
    static OnnxModel instance("model.onnx");
    return instance;
}

std::vector<Core::StrategyDecision> on_market_event(
    Core::Engine& engine, Core::Strategy& strategy,
    std::optional<Core::MarketEvent>) {
    auto sm = MarketData::StateMachineDispatcher::get_instance().get_machine("taker_flow");
    auto features = sm ? sm->emit_state() : std::nullopt;
    if (!features) return {};

    const float score = model().predict(*features);
    std::vector<Core::StrategyDecision> decisions;

    for (auto& [symbol, ticker] : strategy.trade_tickers) {
        std::lock_guard<std::mutex> lock(ticker->trading_mutex);
        const auto& em = ticker->exchange_and_markets.front();
        const auto price = engine.get_price(symbol, em.exchange_name, em.market_type);
        if (!price || !ticker->get_orders(false).empty()) continue;

        Core::StrategyDecision decision;
        decision.symbol = symbol;
        const auto trades = ticker->get_trades(false);
        if (trades.empty() && score > 0.65f) {
            Core::StrategyHelpers::submit_market_order(
                engine, strategy, *ticker, Core::OrderSide::BUY, *price,
                mpfr::mpreal(0.01), decision);
        } else if (!trades.empty() && score < 0.45f) {
            Core::StrategyHelpers::close_trade_market(
                engine, strategy, *ticker, trades.front(), *price, decision);
        }
        if (!decision.orders.empty()) decisions.push_back(std::move(decision));
    }
    return decisions;
}

void bind_ml_strategy(Core::Strategy& strategy) {
    strategy.strategy_callback = on_market_event;
    strategy.compute_predictions_callback = [](Core::Engine&, Core::Strategy&) {};
}

}  // namespace

KERONIS_REGISTER_STRATEGY("ml_taker", bind_ml_strategy);
```

## Workflow

1. **Feature generation (`historical_feature_gen`):** Replay historical Parquet data through a C++ `StrategyStateMachine` to emit training panels. Train in Python (LightGBM, XGBoost, PyTorch) and export to `.onnx`.
2. **Backtesting (`backtest_runner`):** Replay Parquet bars, trades, and order books through your strategy and `.onnx` model with configurable latency, fees, leverage, and Polymarket queue-notional fills. Emits `summary.json`, `trades.json`, and `equity_curve.json`.
3. **Paper & live execution (`live_infra`):** Validate against live feeds using `PaperExchange` (taker at `ack + latency`, maker against resting quotes and subsequent prints, immediate cancel stop), then switch to live accounts with durable quote holds and HTTP introspection.

## Supported venues & extensibility

| Venue | Market data | Execution | Notes |
|---|---|---|---|
| **Polymarket** | Native CLOB WS | Native (EIP-712 bulk) | Multi-outcome tickers, tick-size events, queue-notional backtesting |
| **Hyperliquid** | Native WS | Native (Rust signer) | Mainnet and testnet (`HYPERLIQUID_USE_TESTNET=1`) |
| **Aster** | Native WS | Native | Mainnet and testnet (`ASTER_USE_TESTNET=1`) |
| **Lighter** | Native REST + L2 WS | `PaperExchange` | Nonce-checked `LighterOrderBook` and `DepthHistory` |
| **Binance / Bybit / others** | Native / Parquet | External proxy | Via `CCXT_PROXY_BASE_URL` (not bundled in this repo) |
| **Custom venues / sports betting** | Modular `MarketDataStreams` | Modular `StateUpdater` / `TradeTicker` | Binary/multi-outcome `TradeTicker` support already built for Polymarket |

## Current scope & limitations

- **Platform:** Linux and C++20 only (no Python strategy API).
- **Standalone today:** Core engine, 225+ unit tests, 7 bundled state machines, ONNX inference, `backtest_runner`, `historical_feature_gen`, and `PaperExchange`.
- **Live execution note:** Live deployments require PostgreSQL and NATS (`deploy/docker-compose.yml` + `deploy/schema.sql`). Direct-signing order submission currently still queries `CCXT_PROXY_BASE_URL` for credential lookup; a standalone file-based credential path is next.

## Documentation

- [Quickstart](quickstart.md) — Docker and native build, tests, and example backtest.
- [Strategies](strategies.md) — out-of-tree strategy registration and complete runnable example.
- [State Machines & ONNX](state-machines.md) — implementing `StrategyStateMachine` and ONNX scoring.
- [Architecture](architecture.md) — module boundaries, `live_infra` topology, standby handoff, and `RingBufferBook` internals.
- [Live & Paper](live-and-paper.md) — Postgres + NATS setup, `PaperExchange`, and `QuoteHolds`.
- [Comparison](comparison.md) — tradeoffs vs. NautilusTrader and HftBacktest.
- [Control Server API](control-server.md) & [Configuration](configuration.md) — HTTP endpoints, env vars, and `config.json`.

Plain Markdown for scripts and agents: [`/llms.txt`](https://engine.keronis.ai/llms.txt) and [`/llms-full.txt`](https://engine.keronis.ai/llms-full.txt).

!!! warning "Disclaimer"
    Apache-2.0 licensed, provided as-is without warranty. Validate strategies in `backtest_runner` and `PaperExchange` before connecting live capital.
