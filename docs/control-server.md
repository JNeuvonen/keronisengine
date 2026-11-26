# Control Server API

Setting `ENABLE_ENGINE_CONTROL_SERVER=1` starts an embedded `cpp-httplib` HTTP server inside `live_infra` (default `127.0.0.1:8090`).

!!! warning "Access control"
    Keep `ENGINE_CONTROL_SERVER_BIND=127.0.0.1` and `ENABLE_ENGINE_CONTROL_MUTATIONS=0` unless the server is behind an authenticated internal network.

## Read endpoints (`GET`)

| Endpoint | Description |
|---|---|
| `/control/health` | Uptime, readiness, and whether mutations are enabled |
| `/control/engine/strategies` | Loaded strategies, pause state, and quote-pull status |
| `/control/engine/strategy-telemetry` | Per-strategy telemetry and order-rate counters |
| `/control/engine/orders` | Active orders |
| `/control/engine/orders/unacked` | Orders awaiting venue ack |
| `/control/engine/orders/failed-cancels` | Failed cancel requests |
| `/control/engine/trades` | Active trades |
| `/control/engine/exposure` | Aggregate and per-symbol directional exposure |
| `/control/engine/inventory` | Per-account venue inventory |
| `/control/engine/full-account-state` | Combined balances, inventory, and open orders |
| `/control/engine/bba` | Current best bid/ask |
| `/control/engine/orderbook` | L2 order book snapshot |
| `/control/engine/price` | Latest cached price |
| `/control/engine/spread` | Current bid/ask spread |
| `/control/engine/state-snapshot` | Full engine state snapshot |
| `/control/engine/orderbooks-out-of-sync` | Books flagged out of sync |
| `/control/engine/counters` | Internal engine counters |
| `/control/engine/markouts/pending` | Pending fill markout trackers |
| `/control/engine/exchange-info` | Symbol precision and tick/lot rules |
| `/control/engine/time` | Current engine timestamp (ms) |

## Mutating endpoints (`POST`)

Mutating endpoints return `403` unless `ENABLE_ENGINE_CONTROL_MUTATIONS=1` is set. They exist for integration tests and controlled incident operations:

- Order lifecycle: `/control/engine/orders/created-ack`, `/control/engine/orders/filled-ack`, `/control/engine/orders/canceled-ack`, `/control/engine/orders/update`
- Market data injection: `/control/engine/bba`, `/control/engine/price`, `/control/engine/orderbook/init`, `/control/engine/orderbook/full-refresh`, `/control/engine/orderbook/update`
