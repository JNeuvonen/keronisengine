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

The core loop pairs a registered `StrategyStateMachine` (`windowed_taker_flow`) with `OnnxModel` and `StrategyHelpers` (excerpt from [`binance_onnx_perp.cpp`](https://github.com/JNeuvonen/keronisengine/blob/main/src/cpp/packages/strategies/binance_onnx_perp/binance_onnx_perp.cpp)):

```cpp
void compute_predictions(Core::Engine& engine, Core::Strategy& strategy) {
    auto sm = MarketData::StateMachineDispatcher::get_instance().get_machine(
        "windowed_taker_flow");
    auto features = sm ? sm->emit_state() : std::nullopt;
    if (!features || features->empty()) return;

    float score = 0.5f;
    if (!strategy.strategy_config.strategy_models.empty()) {
        auto model = get_or_load_model(
            strategy.strategy_config.strategy_models.front().path_to_model);
        if (model && features->size() == model->get_input_size()) {
            score = model->predict(*features);
        }
    }

    const long long now_ms = engine.get_time();
    for (auto& [symbol, ticker] : strategy.trade_tickers) {
        ticker->atomic_signal_main.store(score, now_ms);
    }
}
```

See [Strategies](strategies.md) for both flagship compiled examples (`binance_onnx_perp` and `polymarket_digital`).

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
| **Binance / Bybit / OKX / Gate.io / Bitget / Coinbase** | Native / Parquet | Bundled `cpp_live_proxy` | CCXT execution + user-data WS (`src/py/cpp_live_proxy`) |
| **US Equities (IBKR)** | IBKR / Parquet | Bundled `cpp_live_proxy` | Interactive Brokers session & US equity catalog |
| **Sports betting / custom event markets** | Modular streams | Modular ticker & proxy | Multi-outcome ticker architecture built for Polymarket extends cleanly |

## Current scope

- **Platform:** Linux, C++20 (engine), Rust (Hyperliquid signer), and Python 3.12 (bundled `cpp_live_proxy`).
- **Included out of the box:** Core engine, 260+ unit tests, 2 teaching feature state machines (`windowed_taker_flow`, `spread`), 2 flagship strategies (`polymarket_digital`, `binance_onnx_perp`), ONNX inference, `backtest_runner`, `historical_feature_gen`, `PaperExchange`, and the full `deploy/docker-compose.yml` stack (PostgreSQL + NATS + `cpp_live_proxy`).

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
