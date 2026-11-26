# Live & Paper Trading

`live_infra` runs both paper trading (`PaperExchange`) and live venue execution. It reads strategy and account rows from PostgreSQL and publishes events over NATS.

## 1. Start PostgreSQL and NATS

`deploy/docker-compose.yml` starts PostgreSQL 16 and NATS bound to `127.0.0.1`. On first boot, Postgres loads `deploy/schema.sql` and seeds a paper account plus the `test_quoter` strategy via `deploy/seed_paper_strategy.sql`:

```bash
cd deploy
cp .env.example .env
cp accounts.example.json accounts.json
chmod 600 accounts.json
docker compose up -d
```

## 2. Run `live_infra`

Start with `IS_ORDER_CREATION_DISABLED=1` (the default in `deploy/.env.example`) to verify database connectivity, NATS, market-data streams, and the control server before enabling orders:

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

## Venues, direct signing, and `IS_PROD`

- **Direct signing:** `HYPERLIQUID_DIRECT_SIGNING`, `ASTER_DIRECT_SIGNING`, and `POLYMARKET_DIRECT_SIGNING` default to `1`. Leave `*_USE_TESTNET` unset/`0` for mainnet, or set to `1` for testnet.
- **Credential lookup note:** Direct-signing order submission currently still looks up credentials via `CCXT_PROXY_BASE_URL` (`ccxt_proxy.cpp`), which is not bundled in this repository. `PaperExchange` does not require live venue signing.
- **`IS_PROD` and `quote_guard`:** Setting `IS_PROD=1` requires a fresh heartbeat in `engine_health` where `engine_instance = 'quote_guard'` for all non-paper strategies. Without one, `live_infra` pulls resting entry quotes (`QuotesPullReason::GUARD_STALE`). Leave `IS_PROD=0` (or unset) for local paper runs.
- **Other venues:** Binance, Bybit, and other non-native venues require an external proxy configured via `CCXT_PROXY_BASE_URL` and `CCXT_PROXY_USER_DATA_WS_URL`.

## Shutdown

A single `SIGTERM` sets `QuotesPullReason::SHUTDOWN`, pulls resting entry quotes, and exits after the main loop drains. A second `SIGTERM` exits immediately.
