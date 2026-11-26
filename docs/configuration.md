# Configuration & Environment Variables

## Environment Variables (`live_infra`)

### Required

| Variable | Description |
|---|---|
| `DATA_DIR_ROOT` | Root directory for market data, caches, and state-machine persistence |
| `DB_HOST`, `DB_PORT`, `DB_NAME`, `DB_USER`, `DB_PASSWORD` | PostgreSQL connection parameters |
| `ACCOUNTS_FILE` | Path to JSON file mapping `account_id` to venue credentials |
| `NATS_TOKEN` | Required NATS authentication token (`live_infra` exits if unset) |

### Optional / Runtime Switches

| Variable | Default | Description |
|---|---|---|
| `NATS_URL` | `nats://localhost:4222` | NATS server URL |
| `IS_PROD` | *(unset / 0)* | Production flag; when `1`, non-paper strategies require a fresh `quote_guard` heartbeat in `engine_health` (`engine_instance = 'quote_guard'`) |
| `IS_ORDER_CREATION_DISABLED` | `0` | Set to `1` to block all live order submissions |
| `PAPER_SLIPPAGE_BPS` | `3` | Slippage (bps) applied by `PaperExchange` |
| `HYPERLIQUID_DIRECT_SIGNING` | `1` | Native Rust Hyperliquid signing (enabled by default; set `0` to disable) |
| `HYPERLIQUID_USE_TESTNET` | `0` | Route Hyperliquid client to testnet (unset/`0` = mainnet) |
| `ASTER_DIRECT_SIGNING` | `1` | Native Aster signing (enabled by default; set `0` to disable) |
| `ASTER_USE_TESTNET` | `0` | Route Aster client to testnet (unset/`0` = mainnet) |
| `POLYMARKET_DIRECT_SIGNING` | `1` | Native Polymarket CLOB signing (enabled by default; set `0` to disable) |
| `ENABLE_ENGINE_CONTROL_SERVER` | `0` | Enable HTTP control/introspection server |
| `ENGINE_CONTROL_SERVER_BIND` | `127.0.0.1` | Bind address for control server |
| `ENGINE_CONTROL_SERVER_PORT` | `8090` | Listen port for control server |
| `ENABLE_ENGINE_CONTROL_MUTATIONS` | `0` | Allow `POST` mutation endpoints on control server |
| `LOG_DIRECTORY` | *(unset)* | Rotating disk log directory (when built with `-DKERONIS_DISK_LOGGING=ON`) |
| `CCXT_PROXY_BASE_URL` | *(unset)* | External proxy base URL (used for non-native venues and proxy credential lookups) |
| `CCXT_PROXY_USER_DATA_WS_URL` | `ws://localhost:8040/api/v1/user-data/stream` | External proxy user-data WebSocket |

## Environment Variables (`export_strategy_config`)

| Variable | Description |
|---|---|
| `DB_CONNECTION_STR` | libpq connection string used by `export_strategy_config` to read strategy rows from PostgreSQL |

## Backtest Config (`config.json`)

`backtest_runner` takes a single JSON file (`./build/bin/backtest_runner /path/to/config.json`) and never reads PostgreSQL. Key top-level sections:

- **`strategies`**: Array of `StrategyConfig` objects (including `strategy_name`, `exchanges_list`, `trade_tickers`, `models`, risk caps, and order-rate limits). Use `export_strategy_config` to dump a live DB strategy into this format.
- **`market_data`**: Paths to Parquet datasets (`ohlcv_klines`, `trades`, `aggr_trades`, `order_book_snapshots`, `order_book_updates`, `predictions`).
- **`exchanges`**: Fee schedules (`maker_fee_rate`, `taker_fee_rate`, `maker_rebate_rate`) and initial quote-asset balances per `account_id`.
- **`simulation_start_date_ms` / `simulation_end_date_ms`**: Replay window in UTC epoch milliseconds.
- **`market_data_replay`**: `load_mode` (`"eager"` or `"streaming"`), `chunk_duration_ms`, and `reader_threads`.

See [`examples/quickstart_backtest/generate_sample_data.py`](https://github.com/JNeuvonen/keronisengine/blob/main/examples/quickstart_backtest/generate_sample_data.py) for a complete, working `config.json` generator.
