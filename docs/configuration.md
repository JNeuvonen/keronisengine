# Configuration

## `live_infra` environment variables

### Required

| Variable | Description |
|---|---|
| `DATA_DIR_ROOT` | Root directory for Parquet market data, caches, and state-machine persistence |
| `DB_HOST`, `DB_PORT`, `DB_NAME`, `DB_USER`, `DB_PASSWORD` | PostgreSQL connection parameters |
| `ACCOUNTS_FILE` | Path to JSON file mapping `account_id` to venue credentials |
| `NATS_TOKEN` | NATS authentication token (required unless `ENABLE_NATS_PUBLISHER=0`) |
| `MARKET_DATA_COVERAGE_PATH` | Path to public market-data stream coverage JSON (required unless `DISABLE_PUBLIC_MARKET_DATA_STREAMS=1`) |

### Optional

| Variable | Default | Description |
|---|---|---|
| `ENABLE_NATS_PUBLISHER` | `1` | Set to `0` to run `live_infra` without a NATS server |
| `DISABLE_PUBLIC_MARKET_DATA_STREAMS` | `0` | Set to `1` to skip starting `MarketDataStreamManager` when no coverage JSON is configured |
| `MD_STATE_MACHINES` | unset | Comma-separated list of registered `StrategyStateMachine` names to instantiate in `live_infra` |
| `POLYGON_API_KEY` | unset | Polygon.io / Massive.com API key for US equities market data and catalog refresh |
| `NATS_URL` | `nats://localhost:4222` | NATS server URL |
| `IS_PROD` | unset / `0` | When `1`, non-paper strategies require a fresh `quote_guard` heartbeat in `engine_health` (`engine_instance = 'quote_guard'`) |
| `IS_ORDER_CREATION_DISABLED` | `0` | Set to `1` to block live order submissions |
| `PAPER_SLIPPAGE_BPS` | `3` | Slippage in bps applied by `PaperExchange` |
| `HYPERLIQUID_DIRECT_SIGNING` | `1` | Native Rust Hyperliquid signing (`0` to disable) |
| `HYPERLIQUID_USE_TESTNET` | `0` | Target Hyperliquid testnet (`0` / unset = mainnet) |
| `ASTER_DIRECT_SIGNING` | `1` | Native Aster signing (`0` to disable) |
| `ASTER_USE_TESTNET` | `0` | Target Aster testnet (`0` / unset = mainnet) |
| `POLYMARKET_DIRECT_SIGNING` | `1` | Native Polymarket CLOB signing (`0` to disable) |
| `ENABLE_ENGINE_CONTROL_SERVER` | `0` | Start localhost HTTP control server |
| `ENGINE_CONTROL_SERVER_BIND` | `127.0.0.1` | Control server bind address |
| `ENGINE_CONTROL_SERVER_PORT` | `8090` | Control server listen port |
| `ENABLE_ENGINE_CONTROL_MUTATIONS` | `0` | Enable `POST` mutation endpoints |
| `LOG_DIRECTORY` | unset | Rotating disk log directory (when built with `-DKERONIS_DISK_LOGGING=ON`) |
| `CCXT_PROXY_BASE_URL` | `http://localhost:8000/api/v1/ccxt` | Base URL for `cpp_live_proxy` HTTP endpoints (must include `/api/v1/ccxt`) |
| `CCXT_PROXY_USER_DATA_WS_URL` | `ws://localhost:8040/api/v1/user-data/stream` | Proxy user-data WebSocket (`deploy/.env.example` sets port `8000` for local single-process `uvicorn`) |

## `export_strategy_config`

| Variable | Description |
|---|---|
| `DB_CONNECTION_STR` | libpq connection string used to read strategy rows from PostgreSQL |

## Backtest `config.json`

`backtest_runner` takes a single JSON path (`./build/bin/backtest_runner config.json`) and does not connect to PostgreSQL:

- `strategies`: array of `StrategyConfig` objects (`strategy_name`, `exchanges_list`, `trade_tickers`, `models`, risk caps, order-rate limits).
- `market_data`: Parquet dataset paths (`ohlcv_klines`, `trades`, `aggr_trades`, `order_book_snapshots`, `order_book_updates`, `predictions`).
- `exchanges`: fee rates (`maker_fee_rate`, `taker_fee_rate`, `maker_rebate_rate`) and initial balances per `account_id`.
- `simulation_start_date_ms`, `simulation_end_date_ms`: UTC epoch millisecond bounds.
- `output_dir`: directory where `summary.json`, `trades.json`, and `equity_curve.json` are written (omitted or empty suppresses dumps).
- `market_data_replay`: `load_mode` (`"eager"` or `"streaming"`), `chunk_duration_ms`, and `reader_threads`.

See `examples/quickstart_backtest/generate_sample_data.py` for a complete working example.
