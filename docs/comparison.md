# Comparison

| Dimension | KeronisEngine | NautilusTrader | HftBacktest |
|---|---|---|---|
| Core language | C++20 (+ Rust Hyperliquid signer) | Rust core + Python/Cython API | Rust + Python (Numba) |
| Strategy authoring | C++20 functions registered via `KERONIS_REGISTER_STRATEGY` | Python or Rust | Python (Numba) or Rust |
| Feature & ML pipeline | C++ `StrategyStateMachine` shared across `historical_feature_gen`, `backtest_runner`, and `live_infra` + in-process ONNX Runtime | External / user-managed | External / user-managed |
| Order book storage | `RingBufferBook` (tick-indexed 16B slots, bitmap + `__builtin_ctzll`, lazy mmap pages) | Level-based | HashMap / BTree / tick arrays |
| Queue-position modeling | Queue notional (`order_queue_notional_accumulated`) for Polymarket; CEX backtests model latency, depth, and price-through rather than full L3 queue priority | L2/L3 fill models | Full L3 queue-position modeling |
| Paper trading | `PaperExchange` (taker at `ack + latency`, maker against quotes + subsequent trades, cancels stop later prints) | Simulated / sandbox adapters | Backtest-focused + live connectors |
| Native live venues | Hyperliquid, Aster, Polymarket; Lighter L2 market data; other venues via external proxy | Binance, Bybit, OKX, IBKR, Polymarket, others | Binance Futures, Bybit |
| Live operational controls | Durable `QuoteHolds` + `quote_guard` heartbeat, localhost HTTP control server, `SIGTERM` quote pull | Risk engine and execution controls | Connector-dependent |

## Tradeoffs

- **KeronisEngine** is built for running full-exchange crypto feeds (10,000+ messages/sec across Binance) with streaming C++ feature state machines and continuous ONNX inference on a single ~$100 server, using the same C++ code offline and live. It is C++-only and does not bundle a Python strategy layer or a multi-exchange proxy.
- **NautilusTrader** is a better fit if you want to write strategies in Python and need built-in adapters for a wide range of traditional and crypto venues.
- **HftBacktest** is a better fit if your research depends on strict L3 queue-position simulation on CEX order books in Python/Numba or Rust.
