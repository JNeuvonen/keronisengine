# Custom Venues & Feeds

KeronisEngine separates market-data ingest (`market_data`), strategy and risk logic (`trading_engine`), and venue execution (`live_infra` / `cpp_live_proxy`). Adding a new exchange, broker, or custom event feed does not touch `trading_engine`, `StrategyStateMachine`, ONNX inference, or `BacktestState`.

## 1. Fastest path: route through `cpp_live_proxy`

For any exchange supported by CCXT—or any REST/WebSocket broker or sportsbook API where sub-millisecond direct C++ signing is not required—add or configure the adapter in [`src/py/cpp_live_proxy/`](https://github.com/JNeuvonen/keronisengine/tree/main/src/py/cpp_live_proxy):

- **CCXT exchanges:** Add the venue credentials to `deploy/accounts.json` and point `CCXT_PROXY_BASE_URL` at the running `uvicorn` instance. Order submission, cancellation, balance/inventory queries, and user-data WebSocket updates flow through the existing proxy contract without recompiling C++.
- **Broker / equities adapters:** Follow the IBKR adapter pattern under `src/py/cpp_live_proxy/cpp_live_proxy/` to map venue-specific order and position payloads onto the proxy's normalized JSON endpoints.

## 2. Adding a market-data stream (`market_data`)

Market data enters through [`MarketDataStreams`](https://github.com/JNeuvonen/keronisengine/blob/main/src/cpp/packages/market_data/src/market_data_streams.cpp). Each feed parses wire frames (using `simdjson` for JSON feeds) into normalized structs (`NormalizedParsedTrade`, BBA, L2 book snapshots/deltas) and fans them out to four consumers:

1. `DataRecorder` — optional raw capture to disk.
2. `BarAggregator::add_trade(...)` — updates live `OhlcvDataset` time/volume bars.
3. `StateMachineDispatcher::enqueue(MarketEvent(...))` — lock-free push to all registered `StrategyStateMachine` instances.
4. `live_event_callback_(std::move(ev))` — feeds `Live::StateUpdater::live_event_queue_` for order book updates, `PaperExchange`, and strategy callbacks.

Because `MarketEvent` is venue-agnostic (`symbol`, `exchange_name`, `market_type`), any new stream automatically drives existing feature state machines, ONNX models, and `PaperExchange` fill simulation.

## 3. Adding native C++ execution (`live_infra`)

When a venue requires low-latency direct signing (like Polymarket EIP-712 bulk signing, Hyperliquid's Rust signer, or Aster):

1. **Implement the venue client** under [`src/cpp/packages/live_infra/src/exchange_specific/`](https://github.com/JNeuvonen/keronisengine/tree/main/src/cpp/packages/live_infra/src/exchange_specific) handling authentication, order placement, bulk placement (if supported), cancellation, and REST reconciliation.
2. **Wire dispatch in `LiveTradeTicker`:** [`Core::TradeTicker`](https://github.com/JNeuvonen/keronisengine/blob/main/src/cpp/packages/trading_engine/include/market_making/ticker.h) is the execution seam. `Live::LiveTradeTicker` runs order creation on `order_thread_pool` (200 workers) and cancellations on `cancel_order_thread_pool` (100 workers), so blocking venue I/O never stalls strategy callbacks.
3. **Wire private fills in `AccountStreamManager`:** Push order acks, fills, and cancels from the venue's private WebSocket into `Core::Engine` so local inventory and open orders stay synchronized between REST watchdog sweeps.

## 4. Multi-outcome & event markets

For binary or multi-outcome markets (prediction markets, sportsbooks, betting exchanges), `Core::TradeTicker` supports outcome-indexed order books (`read_outcome_bba(outcome)`), per-outcome inventory tracking (`get_outcome_exchange_inventory(outcome)`), dynamic tick sizes (`order_price_min_tick_size`), and `clob_id` / `outcome_name` order tags.

See [`polymarket_digital.cpp`](https://github.com/JNeuvonen/keronisengine/blob/main/src/cpp/packages/strategies/polymarket_digital/polymarket_digital.cpp) for a complete compiled example reading an external reference price (`binance:swap`) and quoting multi-outcome books.
