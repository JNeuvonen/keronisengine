# Live & Paper Trading

`live_infra` runs both paper trading (`PaperExchange`) and live venue execution. It reads strategy and account rows from PostgreSQL, publishes events over NATS, and connects to the bundled Python venue proxy (`src/py/cpp_live_proxy`) for CCXT exchanges, IBKR equities, user-data streams, and direct-signing credential lookup.

## 1. Start PostgreSQL, NATS, and the bundled venue proxy

From the repository root, copy the config templates and initialize a local PostgreSQL 16 database with `deploy/schema.sql` and `deploy/seed_paper_strategy.sql` (which seeds a paper account and the `test_quoter` strategy):

```bash
cp deploy/.env.example deploy/.env
cp deploy/accounts.example.json deploy/accounts.json
chmod 600 deploy/accounts.json

psql -d postgres -c "CREATE USER keronis WITH PASSWORD 'keronis_dev';"
createdb -O keronis keronis
psql -U keronis -h 127.0.0.1 -d keronis -f deploy/schema.sql
psql -U keronis -h 127.0.0.1 -d keronis -f deploy/seed_paper_strategy.sql
```

Start `nats-server` bound to localhost with the token from `deploy/.env` (or set `ENABLE_NATS_PUBLISHER=0` in `deploy/.env` to run without NATS):

```bash
nats-server -a 127.0.0.1 -p 4222 --auth keronis_dev_nats_token
```

Run `cpp_live_proxy` in a Python 3.12 virtualenv:

```bash
python3 -m venv .proxy-venv
.proxy-venv/bin/pip install -r src/py/cpp_live_proxy/cpp_live_proxy/requirements.txt
.proxy-venv/bin/pip install --no-deps ./src/py/cpp_live_proxy
ACCOUNTS_JSON_PATH=./deploy/accounts.json USE_TESTNET=1 \
  .proxy-venv/bin/uvicorn cpp_live_proxy.main:app --host 127.0.0.1 --port 8000
```

`deploy/.env.example` sets `DISABLE_PUBLIC_MARKET_DATA_STREAMS=1` so `live_infra` boots locally without a stream coverage file; when connecting live public market data, unset it and point `MARKET_DATA_COVERAGE_PATH` at your coverage JSON.

## 2. Run `live_infra`

Start with `IS_ORDER_CREATION_DISABLED=1` (the default in `deploy/.env.example`) to verify database connectivity, NATS, proxy connectivity, market-data streams, and the control server before enabling orders:

```bash
set -a
source deploy/.env
set +a

mkdir -p "$DATA_DIR_ROOT"
./build/bin/live_infra
```

Check health:

```bash
curl -s http://127.0.0.1:8090/control/health
```

## Paper trading (`PaperExchange`)

When a strategy has `is_paper_trading = true` or its account has `account_type = 'paper'`, orders are routed to `Live::PaperExchange` instead of the live venue:

- `PaperExchange` evaluates orders on its own thread.
- An order is first judged as a taker at `ack + latency` against recorded/live depth, with configurable slippage (`PAPER_SLIPPAGE_BPS`, default `3`).
- Any resting remainder is evaluated as a maker against resting quotes and subsequent trade prints; canceling an order prevents later prints from filling it.
- Paper trades are labeled as paper in PostgreSQL and can transition cleanly to live accounts.

## Supported venues & direct signing

- **Native C++ execution:** Hyperliquid (`HYPERLIQUID_DIRECT_SIGNING=1`, bundled Rust signer), Aster (`ASTER_DIRECT_SIGNING=1`), and Polymarket CLOB (`POLYMARKET_DIRECT_SIGNING=1`, EIP-712 bulk signing). Set `*_USE_TESTNET=1` for testnet.
- **Bundled `cpp_live_proxy` (`src/py/cpp_live_proxy/`):**
  - **Crypto via CCXT:** Binance, Bybit, OKX, Gate.io, Bitget, and Coinbase (orders, inventory, metadata, and `/api/v1/user-data/stream`).
  - **US Equities via IBKR:** Interactive Brokers TWS/Gateway session (`IBKR_GATEWAY_HOST`, `IBKR_ALLOWED_ACCOUNT_IDS`), equity catalog, and order lifecycle routing.
  - **Credentials & Polymarket helpers:** Serves credentials from `deploy/accounts.json` to `live_infra` direct signers, plus Polymarket collateral management and user-data streaming.
- **`IS_PROD` and `quote_guard`:** Setting `IS_PROD=1` requires a fresh heartbeat in `engine_health` where `engine_instance = 'quote_guard'` for all non-paper strategies. Leave `IS_PROD=0` (or unset) for local paper runs.

## Shutdown

A single `SIGTERM` sets `QuotesPullReason::SHUTDOWN`, pulls resting entry quotes, and exits after the main loop drains. A second `SIGTERM` exits immediately.
