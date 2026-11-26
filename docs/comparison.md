# Comparison

How KeronisEngine compares to the two most popular open-source alternatives:

| | KeronisEngine | NautilusTrader | HftBacktest |
|---|---|---|---|
| **Language** | C++20 (+ Rust Hyperliquid signer) | Rust core + Python API | Rust + Python (Numba) |
| **Strategy authoring** | C++20 functions (no fork needed via `-DKERONIS_STRATEGY_DIRS`) | Python or Rust | Python (Numba) or Rust |
| **ML & feature pipeline** | Built-in C++ streaming state machines shared across offline Parquet gen, backtest, and live + in-process ONNX Runtime | External / user-managed | External / user-managed |
| **Paper / live parity** | Same strategy + state machine + ONNX code across backtest, `PaperExchange`, and live | Same `Strategy` / `Actor` class across backtest and live | Backtest-focused with live connectors |
| **Queue-position modeling** | Polymarket queue-notional modeling; CEX backtests model latency, depth, and price-through | L2 / L3 fill models | Full L3 order queue-position modeling |
| **Native live venues** | Hyperliquid, Aster, Polymarket (+ Lighter market data); other venues via external proxy | Binance, Bybit, OKX, IBKR, Polymarket, and more | Binance Futures, Bybit |
| **Target footprint** | Full-exchange feeds (10k+ msg/s) + continuous ML inference on one ~$100 Linux box | Broad multi-asset platform | High-frequency market-making research |

## When to use which

- **Use KeronisEngine** if you are comfortable in C++, want to ingest full-exchange crypto feeds with continuous ONNX inference on a single commodity server, and want your feature computation code to be identical between training and live trading.
- **Use NautilusTrader** if you want to write strategies in Python and want turnkey adapters for a wide catalog of crypto and traditional brokers.
- **Use HftBacktest** if your edge depends on strict L3 queue-position simulation on CEX order books and you prefer Python/Numba research notebooks.
