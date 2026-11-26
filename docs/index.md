# KeronisEngine

KeronisEngine is an open-source C++20 trading engine composed of independent modules: a market-data and streaming feature layer (`market_data`), a venue-agnostic order/risk/book core (`trading_engine`), a live runtime orchestrator (`live_infra`), an offline event-driven backtester (`backtest_runner`), and an offline feature generator (`historical_feature_gen`).

The same strategy functions and C++ `StrategyStateMachine` classes run in offline training-set generation, historical replay, paper trading (`PaperExchange`), and live trading. In production it ingests full-exchange crypto feeds (10,000+ messages/sec across Binance) while updating order books, computing streaming features, and scoring ONNX models on a single ~$100 dedicated server.

## How the modules fit together

- **`market_data`** manages WebSocket feeds (`MarketDataStreamManager`), in-memory/Parquet `OhlcvDataset`s (`DatasetsManager`), and `StateMachineDispatcher` + `SMSyncer` for streaming features. It has no dependency on orders, accounts, or execution.
- **`trading_engine`** holds `Core::Engine`, `Core::Strategy`, `Core::TradeTicker`, `RingBufferBook`, risk/rate limits, and `BacktestState`. It does not touch live sockets, PostgreSQL, or NATS.
- **`live_infra`** (`Live::StateUpdater`) wires `market_data` into `Core::Engine` through a lock-free `BlockingConcurrentQueue`, routes `LiveTradeTicker` orders to native signers (Hyperliquid Rust signer, Aster, Polymarket), `PaperExchange`, or an external proxy, reconciles fills/inventory via `AccountStreamManager` and REST, hot-syncs strategy configs from PostgreSQL, and handles disk crash recovery and standby process handoffs.
- **`backtest_runner`** and **`historical_feature_gen`** run standalone against partitioned Parquet files under `DATA_DIR_ROOT`, with no database or network dependencies.

See [Architecture](architecture.md) for the full live runtime and offline pipeline diagrams.

## Supported venues

| Venue | Market data | Order execution | Notes |
|---|---|---|---|
| Hyperliquid | Native WebSocket | Native (Rust `hyperliquid_signer`) | Mainnet and testnet (`HYPERLIQUID_USE_TESTNET=1`) |
| Aster | Native WebSocket | Native | Mainnet and testnet (`ASTER_USE_TESTNET=1`) |
| Polymarket | Native CLOB WS | Native | Queue-notional fill tracking in `BacktestState` |
| Lighter | Native REST + L2 WS | `PaperExchange` | Nonce-checked `LighterOrderBook` and `DepthHistory` |
| Binance, Bybit, others | Native / Parquet | External proxy (`CCXT_PROXY_BASE_URL`) | External proxy is not included in this repo |

## Documentation

- [Quickstart](quickstart.md): build from source (or Docker), run tests, and run the `sma_crossover` backtest.
- [Architecture](architecture.md): module boundaries, `live_infra` threading and handoff model, offline replay pipeline, and `RingBufferBook` internals.
- [Strategies](strategies.md): registering external strategies with `KERONIS_REGISTER_STRATEGY` and `-DKERONIS_STRATEGY_DIRS`.
- [State Machines & ONNX](state-machines.md): implementing `StrategyStateMachine` and running ONNX Runtime inference.
- [Live & Paper Trading](live-and-paper.md): PostgreSQL + NATS setup, `schema.sql`, `PaperExchange`, and `QuoteHolds`.
- [Control Server API](control-server.md): localhost HTTP inspection endpoints.
- [Configuration](configuration.md): environment variables and backtest `config.json`.
- [Comparison](comparison.md): tradeoffs against NautilusTrader and HftBacktest.

Plain Markdown documentation for scripts and agents is available at [`/llms.txt`](https://engine.keronis.ai/llms.txt) and [`/llms-full.txt`](https://engine.keronis.ai/llms-full.txt).

!!! warning "Disclaimer"
    KeronisEngine is licensed under Apache-2.0 and provided as-is without warranty. Validate strategies in `backtest_runner` and `PaperExchange` before running with live exchange credentials.
