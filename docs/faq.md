# Frequently Asked Questions

Direct answers to common engineering, architecture, and operational questions about KeronisEngine.

## General

### Does KeronisEngine include profitable strategies or alpha?

No. The strategies in the repository (`polymarket_digital`, `binance_onnx_perp`, `cross_venue_arb`, `sma_crossover`) and the streaming feature templates (`windowed_taker_flow`, `spread`) are compiled structural examples showing how to wire callbacks, order books, ONNX inference, and risk checks. They are not meant to be run with real capital as-is.

Proprietary alpha belongs in a separate private directory compiled out-of-tree via `-DKERONIS_STRATEGY_DIRS`.

### Why C++20 instead of Python or Rust?

- **Predictable latency:** No garbage collector pauses on the tick-to-order hot path, direct control over memory layout (`RingBufferBook` uses 16-byte POD slots and 64-bit occupancy bitmaps), and sub-millisecond end-to-end execution.
- **In-process ML inference:** ONNX Runtime runs inside the same process as market data and strategy callbacks—no IPC, serialization, or Python GIL between feature updates and order decisions.
- **Single feature codebase:** The same compiled C++ `StrategyStateMachine` runs in offline Parquet feature generation, historical backtests, paper trading, and live execution.

Where another language is the pragmatic choice, KeronisEngine uses it: Hyperliquid order signing uses a bundled Rust crate (`src/rust/hyperliquid_signer`), and the optional CCXT/IBKR venue proxy (`src/py/cpp_live_proxy`) is Python 3.12 / FastAPI.

### How does KeronisEngine compare to NautilusTrader or HftBacktest?

KeronisEngine is built specifically around single-process C++20 execution with in-process ONNX inference and shared C++ streaming features across training and live trading, plus first-class Polymarket CLOB support. See [Comparison](comparison.md) for a detailed architectural breakdown.

### What is the license?

Apache-2.0, provided as-is without warranty. Arbitrary-precision accounting uses `mpreal.h`, a clean-room Apache-2.0 wrapper over dynamically linked MPFR/GMP.

---

## Models, Features & Parity

### Do I have to use ONNX Runtime, or can I run non-ML strategies?

ONNX Runtime is optional. Strategies are plain C++ callbacks—`polymarket_digital` and `cross_venue_arb` run without loading any model.

If you do use predictive models, any framework that exports `.onnx` (LightGBM, XGBoost, CatBoost, PyTorch, scikit-learn) works out of the box via `OnnxModel::predict()`. Because `compute_predictions_callback` is ordinary C++, you can also call TensorRT, LibTorch, or a custom C++ pricer directly.

### How does KeronisEngine avoid train–serve skew?

You implement streaming features once as a `StrategyStateMachine` subclass in C++.

1. `historical_feature_gen` replays partitioned Parquet through that exact class to write training panels.
2. `backtest_runner` replays Parquet through the same compiled class.
3. `live_infra` feeds live WebSocket ticks into the same compiled class via `StateMachineDispatcher`.

There is no separate Python research re-implementation to drift from production.

### Can a strategy use OHLCV bars instead of (or alongside) feature state machines?

Yes. `MarketDataStreams` fans raw trades out in parallel to both `BarAggregator` (which maintains live `OhlcvDataset` time/volume bars) and `StateMachineDispatcher` (which updates registered feature state machines). Both `compute_predictions_callback` and `strategy_callback` can read OHLCV bars, `StrategyStateMachine::emit_state()`, live order books, or any combination asynchronously.

---

## Performance & Hardware

### What hardware is required to run KeronisEngine?

A single dedicated Linux host (for example, a ~$100/month bare-metal server) is sufficient to ingest full-exchange crypto feeds at 10,000+ messages/second with live ONNX scoring and sub-millisecond tick-to-order latency. No cluster or GPU is required for typical tabular/ONNX CPU inference.

### Why are there separate thread pools for predictions, strategy callbacks, order creation, and cancels?

Isolating workloads prevents slow operations from stalling latency-sensitive ones:

- `MarketDataStreams` parses raw wire JSON via `simdjson` and fans out without blocking on strategy logic.
- `predictions_pool` (`hardware_concurrency()`) runs model inference and publishes scores through `atomic_signal_main`.
- `strategy_thread_pool` (`hardware_concurrency()`) evaluates risk-checked order decisions.
- `order_thread_pool` (200 workers) and `cancel_order_thread_pool` (100 workers) handle blocking venue HTTP/signing I/O separately, so an order burst or slow venue response cannot block cancels or strategy evaluation.

### Does it run on macOS or Windows?

Linux is the target platform for building and running KeronisEngine. On macOS or Windows, build and run inside a Linux VM, WSL2, or a dedicated Linux host. See [Quickstart](quickstart.md).

---

## Venues & Extensibility

### Which venues run natively vs. through `cpp_live_proxy`?

- **Direct native execution in C++ / Rust:** Polymarket CLOB (WebSockets + EIP-712 bulk signing), Hyperliquid (bundled Rust signer), and Aster.
- **Bundled `cpp_live_proxy` (Python / FastAPI):** Binance, Bybit, OKX, Gate.io, Bitget, and Coinbase via CCXT, plus US equities via Interactive Brokers (IBKR Gateway) paired with Massive.com (formerly Polygon.io) market data.
- **Native market data + paper:** Lighter L2 order book feeds with `PaperExchange`.

### How do I keep proprietary strategies private while pulling upstream updates?

Keep your strategies and feature state machines in a separate private repository, register them with `KERONIS_REGISTER_STRATEGY` and `KERONIS_REGISTER_STATE_MACHINE`, and pass `-DKERONIS_STRATEGY_DIRS="/path/to/private"` at CMake configure time. You do not need to fork or patch core engine files. See [Strategies](strategies.md).

### How hard is it to add a new exchange, broker, or custom event feed?

Market data ingestion (`market_data`), core engine (`trading_engine`), and execution (`live_infra` / `cpp_live_proxy`) are separated by narrow interfaces (`Core::TradeTicker` and stream adapters). Adding a CCXT-supported exchange requires proxy configuration only; adding a custom native venue or sportsbook feed plugs into the stream and multi-outcome ticker interfaces while reusing the existing feature, ONNX, risk, and simulation stack.

---

## Backtesting, Paper Trading & Live Operations

### How realistic are backtest and paper fills?

- **`BacktestState`** replays timestamp-ordered Parquet bars, trades, and order books with configurable latency, maker/taker fees, swap leverage, and Polymarket queue-position notional tracking.
- **`PaperExchange`** runs on a dedicated thread inside `live_infra` against live feeds. Taker orders evaluate at `ack + latency` against live depth with `PAPER_SLIPPAGE_BPS` (default `3`), resting remainders fill as maker against quotes and subsequent prints, and cancels immediately prevent later fills.

### How do zero-downtime restarts and safety controls work in live trading?

`live_infra` supports cron-managed standby handoff: a standby process warms up streams and datasets before acquiring `/tmp/live_infra_state_lock`, restores open orders and trades from disk recovery, and takes over when the active process exits on `SIGTERM` or max runtime. Durable `QuoteHolds` survive restarts, `quote_guard` heartbeat checks pull quotes if monitoring drops, and an embedded localhost HTTP control server provides runtime introspection. See [Architecture](architecture.md) and [Live & Paper Trading](live-and-paper.md).
