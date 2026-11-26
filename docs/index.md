# Documentation

KeronisEngine is an open-source C++20 trading engine for predictive models and systematic strategies. Streaming feature state machines, in-process ONNX Runtime inference, order books, risk checks, and venue execution run in a single process across historical replay, paper trading, and live execution.

For a visual breakdown of module boundaries, threading, and memory layout, see [Architecture](architecture.md).

## Guides & Reference

### Getting Started

| Page | Covers |
|---|---|
| [Quickstart](quickstart.md) | Native Linux CMake/vcpkg build (`make test`), unit tests, and the synthetic `sma_crossover` backtest. |
| [Comparison](comparison.md) | Architectural tradeoffs relative to NautilusTrader and HftBacktest. |
| [FAQ](faq.md) | Direct Q&A on design choices, ONNX inference, hardware, venues, and live operations. |

### Core Concepts

| Page | Covers |
|---|---|
| [Architecture](architecture.md) | Layer separation, `live_infra` topology, standby handoff, offline Parquet pipeline, and `RingBufferBook` internals. |
| [Strategies](strategies.md) | Walkthroughs of `polymarket_digital`, `binance_onnx_perp`, and `cross_venue_arb`, locking rules, and out-of-tree builds (`-DKERONIS_STRATEGY_DIRS`). |
| [State Machines & ONNX](state-machines.md) | Writing streaming C++ `StrategyStateMachine` classes, warmup guards, live reconnect catch-up, and `OnnxModel::predict()`. |

### Operations & Reference

| Page | Covers |
|---|---|
| [Live & Paper Trading](live-and-paper.md) | Local PostgreSQL + NATS + `cpp_live_proxy` setup, `PaperExchange` fill model, durable `QuoteHolds`, and venue configuration. |
| [Control Server API](control-server.md) | Embedded localhost HTTP endpoints (`/control/engine/*`), health checks, and gated controls. |
| [Configuration](configuration.md) | Environment variables for `live_infra` and `export_strategy_config`, plus backtest `config.json` schema. |

## End-to-end workflow

1. **Offline feature generation (`historical_feature_gen`):** Replay partitioned Parquet data through a compiled `StrategyStateMachine` to emit training panels. Train in Python (LightGBM, XGBoost, PyTorch) and export to `.onnx`.
2. **Historical backtest (`backtest_runner`):** Replay Parquet bars, trades, and order books through your strategy and `.onnx` model with configurable latency, fees, leverage, and Polymarket queue-notional fills. Emits `summary.json`, `trades.json`, and `equity_curve.json`.
3. **Paper and live execution (`live_infra`):** Validate against live feeds with `PaperExchange` (taker at `ack + latency`, maker against resting quotes and subsequent prints, immediate cancel stop), then connect live accounts with durable quote holds and HTTP introspection.

## Supported venues & adapters

| Venue | Market data | Execution | Notes |
|---|---|---|---|
| **Polymarket** | Native CLOB WS | Native (EIP-712 bulk) | Multi-outcome tickers, tick-size events, queue-notional backtesting |
| **Hyperliquid** | Native WS | Native (Rust signer) | Mainnet and testnet (`HYPERLIQUID_USE_TESTNET=1`) |
| **Aster** | Native WS | Native | Mainnet and testnet (`ASTER_USE_TESTNET=1`) |
| **Lighter** | Native REST + L2 WS | `PaperExchange` | Nonce-checked `LighterOrderBook` and `DepthHistory` |
| **Binance / Bybit / OKX / Gate.io / Bitget / Coinbase** | Native / Parquet | Bundled `cpp_live_proxy` | CCXT execution and user-data WebSockets (`src/py/cpp_live_proxy`) |
| **US Equities (Massive.com + IBKR)** | Massive.com (Polygon.io) / Parquet | Bundled `cpp_live_proxy` | Interactive Brokers Gateway and active-plus-delisted US equity catalog |
| **Sports & custom event markets** | Modular streams | Modular ticker & proxy | Multi-outcome order books and stream interfaces |

## Included in the repository

- **Platform:** Linux, C++20 (engine), Rust (Hyperliquid signer), Python 3.12 (bundled `cpp_live_proxy`).
- **Examples & tests:** 260+ Catch2 unit tests, 2 streaming feature templates (`windowed_taker_flow`, `spread`), 3 compiled example strategies (`polymarket_digital`, `binance_onnx_perp`, `cross_venue_arb`), `sma_crossover` quickstart backtest, and `deploy/` SQL schema & config templates.
- **Machine-readable docs:** [`/llms.txt`](https://engine.keronis.ai/llms.txt) and [`/llms-full.txt`](https://engine.keronis.ai/llms-full.txt).

!!! warning "Disclaimer"
    Apache-2.0 licensed, provided as-is without warranty. Validate strategies in `backtest_runner` and `PaperExchange` before connecting live capital.
