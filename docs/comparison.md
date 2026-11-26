# Comparison: KeronisEngine vs. NautilusTrader vs. HFTBacktest

Every serious trading framework makes deliberate architectural tradeoffs. Here is an honest look at where KeronisEngine fits and where other tools may be a better choice.

| Dimension | **KeronisEngine** | **NautilusTrader** | **HFTBacktest** |
|---|---|---|---|
| **Core language** | C++20 (+ Rust signer for Hyperliquid) | Rust core + Python/Cython API | Rust + Python (Numba) |
| **Primary sweet spot** | Full-exchange crypto feeds (10k+ msg/s on a single commodity server) with streaming state-machine features & in-process ONNX inference | Broad multi-asset ecosystem with Python strategy ergonomics | Ultra-faithful L3 tick/queue-position research and latency modeling |
| **Strategy authoring** | Native C++20 functions registered via `KERONIS_REGISTER_STRATEGY` (no fork needed) | Python or Rust | Python (Numba) or Rust |
| **Feature & ML pipeline** | Built-in C++ `StrategyStateMachine`s shared across `historical_feature_gen`, `backtest_runner`, and `live_infra` + in-process ONNX Runtime | External / user-managed | External / user-managed |
| **Order book data structure** | `RingBufferBook`: tick-indexed 16-byte slots, occupancy bitmap + `__builtin_ctzll` walks, lazy OS page commitment | Tree/level-based | HashMap / BTree / tick arrays |
| **Queue-position modeling** | Supported for Polymarket (`order_queue_notional_accumulated`); CEX backtests model depth/latency/price-through rather than full L3 queue priority | L2/L3 fill models | Full L3 queue-position modeling (primary focus) |
| **Paper trading** | Built-in `PaperExchange` (taker at ack + latency, maker against resting quote + subsequent trades, cancels stop later prints) | Simulated / sandbox adapters | Backtest-focused + live connectors |
| **Native live venues** | Hyperliquid (Rust signer), Aster, Polymarket; Lighter L2 market data; other venues via external proxy | Binance, Bybit, OKX, Interactive Brokers, Polymarket, etc. | Binance Futures, Bybit |
| **Operational safety** | Durable `QuoteHolds` (survives restart + stale `quote_guard` heartbeat), localhost HTTP control server, graceful `SIGTERM` quote pull | Risk engine & execution controls | Connector-dependent |

## When to Pick KeronisEngine

- You want to ingest **an entire exchange's live market data** (10,000+ msg/s) on a single ~$100 dedicated box while computing streaming features and running continuous ONNX inference in the same C++ process.
- You care deeply about **zero train/serve feature skew**—running the exact same C++ state-machine code in offline Parquet feature generation (`historical_feature_gen`), backtesting, and live trading.
- You want **crash-resilient live operations** (durable quote holds, localhost HTTP introspection, high-precision MPFR accounting) without writing a C++ engine from scratch.

## When to Pick Something Else

- **Pick NautilusTrader** if you want to write strategies in Python and want turnkey adapters for a wide catalog of traditional and crypto brokers out of the box.
- **Pick HFTBacktest** if your primary edge depends on strict L3 order-queue-position simulation on CEX order books and you prefer Numba/Rust research notebooks.
