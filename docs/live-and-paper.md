# Live & Paper Trading

`live_infra` is the runtime process for both **paper trading** (`PaperExchange`) and **live venue execution**.

## 1. Start PostgreSQL and NATS

The `deploy/` directory provides a local Docker Compose stack (bound to `127.0.0.1`) that initializes PostgreSQL with [`deploy/schema.sql`](https://github.com/JNeuvonen/keronisengine/blob/main/deploy/schema.sql) and seeds a paper account + `test_quoter` strategy (`version = '2.0'`, `backtest_json_format_version = '3.0'`) via [`deploy/seed_paper_strategy.sql`](https://github.com/JNeuvonen/keronisengine/blob/main/deploy/seed_paper_strategy.sql):

```bash
cd deploy
cp .env.example .env
docker compose up -d
```

Verify Postgres and NATS are running:

```bash
docker compose ps
```

## 2. Configure Venue Credentials

Copy `deploy/accounts.example.json` to `deploy/accounts.json` and edit it:

```bash
cp deploy/accounts.example.json deploy/accounts.json
```

```json
{
  "hyperliquid_futures_accounts": [
    {
      "account_id": 1,
      "access": "0xYOUR_WALLET_ADDRESS",
      "secret": "0xYOUR_PRIVATE_KEY"
    }
  ],
  "aster_futures_accounts": [],
  "polymarket_accounts": []
}
```

Restrict file permissions:

```bash
chmod 600 deploy/accounts.json
```

## 3. Run `live_infra`

Load your `.env` (including the required `NATS_TOKEN`) and start with `IS_ORDER_CREATION_DISABLED=1` first to verify market data, database sync, and the HTTP control server:

```bash
set -a
source deploy/.env
set +a

mkdir -p "$DATA_DIR_ROOT"
./build/bin/live_infra
```

Check health via the control server:

```bash
curl -s http://127.0.0.1:8090/control/health | jq .
```

## Paper Trading (`PaperExchange`)

When a strategy has `is_paper_trading = true` (or its account has `account_type = 'paper'`), orders are routed to `Live::PaperExchange` instead of the live venue:

- Orders are evaluated on a dedicated thread against live/recorded market depth with simulated ack latency and configurable slippage (`PAPER_SLIPPAGE_BPS`, default `3`).
- Paper trades are labeled as paper in the database and can transition cleanly to live without colliding with live inventory.

## Direct Signing, `IS_PROD`, and Proxy Notes

- **Direct signing flags:** `HYPERLIQUID_DIRECT_SIGNING`, `ASTER_DIRECT_SIGNING`, and `POLYMARKET_DIRECT_SIGNING` are **enabled by default** (`1`). Set `HYPERLIQUID_USE_TESTNET=1` / `ASTER_USE_TESTNET=1` to target testnets (leaving them unset targets mainnet).
- **Credential lookup (pre-release limitation):** Direct-signing order submission currently still fetches account credentials through `CCXT_PROXY_BASE_URL` (`ccxt_proxy.cpp`), which is not bundled in this repository. Paper trading (`PaperExchange`) does not require live venue signing.
- **`IS_PROD` and `quote_guard`:** Setting `IS_PROD=1` enforces a fresh `quote_guard` heartbeat in PostgreSQL's `engine_health` table (`engine_instance = 'quote_guard'`) on all non-paper strategies; without a heartbeat writer, `live_infra` pulls resting quotes (`QuotesPullReason::GUARD_STALE`). Keep `IS_PROD=0` (or unset) for local paper runs.
- **Other venues (Binance, Bybit, etc.):** Require an external HTTP/WS execution proxy configured via `CCXT_PROXY_BASE_URL` and `CCXT_PROXY_USER_DATA_WS_URL`.

## Graceful Shutdown

Sending `SIGTERM` once requests a clean shutdown: `live_infra` pulls resting entry quotes (`QuotesPullReason::SHUTDOWN`) and exits cleanly after the main loop drains. A second `SIGTERM` exits immediately.
