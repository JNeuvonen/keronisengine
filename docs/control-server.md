# Engine Control Server API

When `ENABLE_ENGINE_CONTROL_SERVER=1` is set, `live_infra` starts an embedded HTTP server (default `127.0.0.1:8090`) for live introspection and optional operational control.

!!! danger "Security"
    Keep `ENGINE_CONTROL_SERVER_BIND=127.0.0.1` and `ENABLE_ENGINE_CONTROL_MUTATIONS=0` in production unless exposed strictly behind an authenticated internal network.

## Read Endpoints (`GET`)

| Endpoint | Description |
|---|---|
| `/control/health` | Engine uptime, readiness, and whether mutations are enabled |
| `/control/engine/strategies` | Loaded strategies, pause state, and quote-pull status |
| `/control/engine/strategy-telemetry` | Per-strategy runtime telemetry and order-rate counters |
| `/control/engine/orders` | Active orders (supports query filtering) |
| `/control/engine/orders/unacked` | Orders awaiting venue acknowledgment |
| `/control/engine/orders/failed-cancels` | Cancel requests that failed |
| `/control/engine/trades` | Open and active trades |
| `/control/engine/exposure` | Aggregate and per-symbol directional exposure |
| `/control/engine/inventory` | Per-account venue inventory |
| `/control/engine/full-account-state` | Combined account balances, inventory, and open orders |
| `/control/engine/bba` | Current best bid/ask for a symbol/venue |
| `/control/engine/orderbook` | Current L2 order book snapshot |
| `/control/engine/price` | Latest cached mark/last price |
| `/control/engine/spread` | Current bid/ask spread |
| `/control/engine/state-snapshot` | Full engine state snapshot |
| `/control/engine/orderbooks-out-of-sync` | Books currently flagged out-of-sync |
| `/control/engine/counters` | Internal engine counters |
| `/control/engine/markouts/pending` | Pending fill markout trackers |
| `/control/engine/exchange-info` | Loaded symbol precision and tick/lot rules |
| `/control/engine/time` | Current engine timestamp (ms) |

## Mutating Endpoints (`POST`)

Mutating endpoints return `403` unless `ENABLE_ENGINE_CONTROL_MUTATIONS=1` is set. They are primarily used for integration testing, scenario replay, and controlled incident operations:

- **Order lifecycle injection:** `/control/engine/orders/created-ack`, `/control/engine/orders/filled-ack`, `/control/engine/orders/canceled-ack`, `/control/engine/orders/update`
- **Market data injection:** `/control/engine/bba`, `/control/engine/price`, `/control/engine/orderbook/init`, `/control/engine/orderbook/full-refresh`, `/control/engine/orderbook/update`
