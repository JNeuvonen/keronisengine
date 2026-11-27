# Control Server API

Setting `ENABLE_ENGINE_CONTROL_SERVER=1` starts an embedded `cpp-httplib` HTTP server inside `live_infra` (default `127.0.0.1:8090`).

Source: [`src/cpp/packages/engine_control_server/src/server.cpp`](https://github.com/JNeuvonen/keronisengine/blob/main/src/cpp/packages/engine_control_server/src/server.cpp)

!!! warning "Access control"
    Keep `ENGINE_CONTROL_SERVER_BIND=127.0.0.1` and `ENABLE_ENGINE_CONTROL_MUTATIONS=0` unless the server is behind an authenticated internal network.

## Read endpoints (`GET`)

### Engine & market state

| Endpoint | Handler |
|---|---|
| `/control/health` | Uptime, readiness, and mutation-gate status |
| `/control/engine/strategies` | Loaded strategies, pause state, and quote-pull status |
| `/control/engine/strategy-telemetry` | Per-strategy telemetry and order-rate counters |
| `/control/engine/orders` | Active orders |
| `/control/engine/orders/unacked` | Orders awaiting venue ack |
| `/control/engine/orders/failed-cancels` | Failed cancel requests |
| `/control/engine/orders/misc/tracked` | Tracked miscellaneous orders |
| `/control/engine/trades` | Active trades |
| `/control/engine/exposure` | Aggregate and per-symbol directional exposure |
| `/control/engine/inventory` | Per-account venue inventory |
| `/control/engine/borrowable-inventory` | Per-account borrowable inventory |
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
| `/control/engine/polymarket/event` | Polymarket event metadata |
| `/control/engine/polymarket/orderbook/bba` | Polymarket outcome best bid/ask |

### Strategy & state machine reads

| Endpoint | Handler |
|---|---|
| `/control/strategy/orders` | Active orders scoped to a strategy |
| `/control/strategy/trades` | Active trades scoped to a strategy |
| `/control/strategy/virtual-balance` | Strategy virtual balance |
| `/control/strategy/recent-orders` | Recent order history for a strategy |
| `/control/strategy/sm/sync-progress` | Attached state machine sync progress |
| `/control/strategy/sm/features` | Latest feature vector emitted by attached state machine |
| `/control/sm/list` | All registered state machines in `StateMachineDispatcher` |
| `/control/sm/sync-progress` | Sync progress for any registered state machine by name |
| `/control/sm/features` | Latest feature vector for any registered state machine by name |

## Mutating endpoints (`POST`)

All `POST` endpoints return `403` unless `ENABLE_ENGINE_CONTROL_MUTATIONS=1` is set. They exist for integration tests and controlled incident operations.

- **Order lifecycle:** `/control/engine/orders/created-ack`, `/control/engine/orders/filled-ack`, `/control/engine/orders/canceled-ack`, `/control/engine/orders/update`, `/control/engine/orders/misc/track`, `/control/engine/orders/misc/untrack`
- **Market data injection:** `/control/engine/bba`, `/control/engine/price`, `/control/engine/orderbook/init`, `/control/engine/orderbook/full-refresh`, `/control/engine/orderbook/update`, `/control/engine/orderbook/levels-only`, `/control/engine/market-event`
- **Inventory & balances:** `/control/engine/inventory`, `/control/engine/quote-asset`, `/control/engine/borrowable-inventory`, `/control/engine/inventory-and-quote-bulk`
- **Trade management:** `/control/engine/trades/inventory-mismatch-remove`, `/control/engine/trades/manual-remove`, `/control/engine/trades/funding-payment`
- **Polymarket state:** `/control/engine/polymarket/event`, `/control/engine/polymarket/event/remove`, `/control/engine/polymarket/bba`, `/control/engine/polymarket/last-trade`, `/control/engine/polymarket/orderbook/init`, `/control/engine/polymarket/orderbook/snapshot`, `/control/engine/polymarket/orderbook/delta`
- **Deterministic triggers:** `/control/engine/triggers/run-periodic-calls`, `/control/engine/triggers/poll-strategy-states`, `/control/engine/triggers/poll-realtime-subscriptions`, `/control/engine/triggers/process-engine-signals`, `/control/engine/triggers/generate-predictions`, `/control/engine/triggers/sync-persistent-state`, `/control/engine/triggers/flush-trading-logs`, `/control/engine/triggers/emit-state-snapshot`
- **State machine & lifecycle:** `/control/strategy/sm/react`, `/control/sm/react`, `/control/engine/state-ready`, `/control/engine/stop-trading`, `/control/engine/gracefully-stop`, `/control/engine/reset`
