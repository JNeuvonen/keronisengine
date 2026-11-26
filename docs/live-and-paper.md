# Live & Paper Trading

`live_infra` runs both paper trading (`PaperExchange`) and live venue execution. It reads strategy and account rows from PostgreSQL, publishes events over NATS, and connects to the bundled Python venue proxy (`src/py/cpp_live_proxy`) for CCXT exchanges, IBKR equities, user-data streams, and direct-signing credential lookup.

## 1. Start PostgreSQL, NATS, and the bundled venue proxy

`deploy/docker-compose.yml` starts PostgreSQL 16, NATS, and `ccxt_proxy` (`src/py/cpp_live_proxy`) all bound to `127.0.0.1`. On first boot, Postgres loads `deploy/schema.sql` and seeds a paper account plus the `test_quoter` strategy via `deploy/seed_paper_strategy.sql`:

```bash
cd deploy
cp .env.example .env
cp accounts.example.json accounts.json
chmod 600 accounts.json
docker compose up -d --build
```

You can also run `cpp_live_proxy` directly in a Python 3.12 virtualenv:

```bash
python3 -m venv .proxy-venv
.proxy-venv/bin/pip install -r src/py/cpp_live_proxy/cpp_live_proxy/requirements.txt
.proxy-venv/bin/pip install --no-deps ./src/py/cpp_live_proxy
ACCOUNTS_JSON_PATH=./deploy/accounts.json USE_TESTNET=1 \
  .proxy-venv/bin/uvicorn cpp_live_proxy.main:app --host 127.0.0.1 --port 8000
```

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
