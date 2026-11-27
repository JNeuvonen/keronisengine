# Core Primitives & Invariants

Reference for the core C++20 classes across `market_data`, `trading_engine`, and `live_infra`—what each class owns, its threading and locking contract, and the invariants you must preserve when building on or modifying the engine.

---

## `market_data`

### `OhlcvDataset`

Header: [`src/cpp/packages/market_data/include/market_data/ohlcv_dataset.h`](https://github.com/JNeuvonen/keronisengine/blob/main/src/cpp/packages/market_data/include/market_data/ohlcv_dataset.h)

Stores OHLCV history for one `(symbol, exchange, market_type, interval_ms)` series in a columnar Structure-of-Arrays (SoA) layout (`klines` alongside parallel `open_prices`, `high_prices`, `low_prices`, `close_prices`, `volumes`, `quote_asset_volumes`, `number_of_trades`, `taker_buy_base_volumes`, `taker_buy_quote_volumes`) plus an attached `FeatureCache`.

- **Locking:** Guarded by an internal `std::shared_mutex`. Call `lock_for_read()` for multi-vector consistent reads, or use the thread-safe copy accessors (`get_close_prices()`, `get_all_kline_ticks()`, etc.).
- **Lookahead prevention:** `read_full_history_from_disk(from_inclusive, to_exclusive)` requires an explicit `to_exclusive` upper bound. Parquet partition pruning operates at the file level (a monthly partition returns whole), so the row-level cutoff is enforced inside this method to prevent backtest strategies from reading future bars.
- **Backfill outcomes:** `init_sync_bars()` returns `BackfillOutcome { Skipped, Fetched, NoData, Failed }`. `Failed` (rate limit, network, or parse error) is never collapsed into `NoData`, ensuring throttled backfills are retried rather than marked complete.
- **Equities splits & RTH:** `rescale_for_split(cutoff_ms, ratio)` adjusts in-memory bars opening before `cutoff_ms` (`price / ratio`, `volume * ratio`) without modifying disk. `include_outside_rth_ticks` is set automatically by venue: Polygon stock datasets retain pre-/post-market ticks while other equity venues drop outside-RTH bars.

### `DatasetsManager`

Header: [`src/cpp/packages/market_data/include/market_data/dataset_manager.h`](https://github.com/JNeuvonen/keronisengine/blob/main/src/cpp/packages/market_data/include/market_data/dataset_manager.h)

Process-wide singleton (`DatasetsManager::get_instance()`) owning the `Store` of `std::shared_ptr<OhlcvDataset>` instances behind `store_mutex`. Coordinates bar flushes (`add_ohlcv_tick()`), memory retention trimming (`rm_unused_klines()`), and Parquet persistence (`sync_ohlcv_datasets_to_disk()`, guarded by an atomic `is_syncing_to_disk` flag).

### `BarAggregator`

Header: [`src/cpp/packages/market_data/include/market_data/bar_aggregator.h`](https://github.com/JNeuvonen/keronisengine/blob/main/src/cpp/packages/market_data/include/market_data/bar_aggregator.h)

Singleton (`BarAggregator::getInstance()`) aggregating live `NormalizedParsedTrade` ticks (`add_trade()`) into open and closed `Bar`s per `BarClosePolicy` under a `std::shared_mutex`. Flushes closed bars into `DatasetsManager` and resets affected open bars via `live_trade_stream_disconnected_reset(...)` when a trade WebSocket drops.

### `MarketDataStreams`

Header: [`src/cpp/packages/market_data/include/market_data/market_data_streams.h`](https://github.com/JNeuvonen/keronisengine/blob/main/src/cpp/packages/market_data/include/market_data/market_data_streams.h) · Implementation: [`market_data_streams.cpp`](https://github.com/JNeuvonen/keronisengine/blob/main/src/cpp/packages/market_data/src/market_data_streams.cpp)

Singleton managing public WebSocket connections and dedicated per-stream-type consumer threads (`ohlcv_msg_consumer_thread`, `lob_msg_consumer_thread`, `trade_msg_consumer_thread`, `book_ticker_msg_consumer_thread`, `ticker_msg_consumer_thread`, `polymarket_msg_consumer_thread`), plus a 5-second `health_check_loop()` and `bars_utils_thread()`.

- **Zero-allocation BBA pipeline:** Book-ticker feeds write into a fixed array of `MAX_BBA_SLOTS = 16384` seqlock slots (`BbaSlot { std::atomic<uint64_t> seq, char payload[512], size_t length }`). Each connection holds an immutable `BbaSymbolSlots` map (`ankerl::unordered_dense::map` with transparent `std::string_view` hashing) read without locks on the WebSocket callback thread.
- **Fan-out:** Trade frames parsed via `simdjson` (`NormalizedParsedTrade::from_simdjson`) fan out directly to `DataRecorder`, `BarAggregator::add_trade`, `StateMachineDispatcher::enqueue`, and `live_event_callback_`.
- **Dynamic streams:** Supports `open_streams()` and `retire_streams()` on a running process without restart, and `resubscribe_lighter_order_book()` for Lighter L2 sequence-gap recovery.

### `StateMachineDispatcher` & `StateMachineSyncer`

Headers: [`state_machine_dispatcher.h`](https://github.com/JNeuvonen/keronisengine/blob/main/src/cpp/packages/market_data/include/market_data/state_machine_dispatcher.h) · [`sm_syncer.h`](https://github.com/JNeuvonen/keronisengine/blob/main/src/cpp/packages/market_data/include/market_data/sm_syncer.h)

- **`StateMachineDispatcher`** owns a `moodycamel::BlockingConcurrentQueue<MarketEvent>` drained by a single `consumer_thread_`. The consumer caches a local copy of registered machines keyed by `machines_version_`, taking zero mutexes on the hot path. `get_machine(name)` is lockless after startup.
- **`StateMachineSyncer`** runs periodic REST catch-up for each `SMMarketDataDependency`, persists state snapshots to disk, and emits rate-limited `SM_WARMUP_TIMEOUT` alerts (at most once every 5 minutes per unsynced machine).

---

## `trading_engine`

### `Core::Engine`

Header: [`src/cpp/packages/trading_engine/include/market_making/engine.h`](https://github.com/JNeuvonen/keronisengine/blob/main/src/cpp/packages/trading_engine/include/market_making/engine.h)

Process-wide singleton matching the one-process-per-host runtime model (unit tests reset state explicitly between cases). Owns:

- **Thread pools:** `generate_predictions_thread_pool` (`hardware_concurrency()`), `strategy_thread_pool` (`hardware_concurrency()`), `order_thread_pool` (`200` workers), `cancel_order_thread_pool` (`100` workers), and `periodic_tasks_thread_pool` (`max(2, hardware_concurrency() / 4)`).
- **Market state:** `RingBufferBook` instances, BBA/price caches, and `PolymarketActiveEvent` (`MAX_POLYMARKET_EVENT_OUTCOMES = 10` with atomic per-outcome BBA and last-trade prices; `get_outcome_implied_probability()` uses BBA midpoint in live mode and `last_trade_price` in backtest mode).
- **Telemetry & markouts:** `MarkoutJob` fill-markout trackers and `EngineMetricSample` observability batches persisted by `live_infra`.

### `Core::Strategy`

Header: [`src/cpp/packages/trading_engine/include/market_making/strategy.h`](https://github.com/JNeuvonen/keronisengine/blob/main/src/cpp/packages/trading_engine/include/market_making/strategy.h)

Binds strategy callbacks via plain function pointers (`StrategyCallback`, `ComputePredictionsCallback`, `OnFillCallback`, `ApplyPrecomputedPredictionsCallback`, `MintContractsCallback`) and manages per-strategy `trade_tickers`, order-rate windows, and virtual balance.

- **Locking:** Hold `lock_on_pred_market_strat()` (a `std::shared_lock<std::shared_mutex>` on `trade_tickers_mutex`) while iterating `trade_tickers` on dynamic prediction-market strategies.
- **Live hot-reload:** When a PostgreSQL strategy row changes, `live_infra` waits until `is_idle()` (`!is_prediction_thread_running && !is_generating_strategy_decisions`), then transfers existing tickers, orders, trades, and positions via `adopt_runtime_state()` and `adopt_moving_state()`. Strategies where `registers_own_state_machine()` is true are never rebuilt live to avoid registering duplicate state machines under the same name.
- **Pause & quote-pull reasons:** Closed sets under `StrategyPauseReason` (`RISK_KILL_SWITCH`, `DATA_STALE`, `SM_RESET`, `WARMUP`, `OPERATOR_REQUESTED`, etc.) and `QuotesPullReason` (`PULL_SIGNAL`, `GUARD_STALE`, `SHUTDOWN`).

### `Core::TradeTicker` & `AtomicSignal`

Header: [`src/cpp/packages/trading_engine/include/market_making/ticker.h`](https://github.com/JNeuvonen/keronisengine/blob/main/src/cpp/packages/trading_engine/include/market_making/ticker.h)

The execution seam between strategy logic and venues: `live_infra` instantiates `Live::LiveTradeTicker`, while `backtest_runner` instantiates `Core::BacktestTradeTicker`.

- **Lock-free signal handoff:** `AtomicSignal` (`atomic_signal_main`, `atomic_signal_short_horizon`) writes `prediction` with `memory_order_relaxed` followed by `timestamp_ms` with `memory_order_release`, and reads `timestamp_ms` with `memory_order_acquire` before `prediction` (`relaxed`)—allowing `predictions_pool` and `strategy_pool` to communicate without a mutex.
- **Per-ticker locking:** Always hold `std::lock_guard<std::mutex> lock(ticker->trading_mutex)` before inspecting `orders` / `active_trades` or calling `construct_order()`.
- **Fill aggregation:** By default, fills from one signal aggregate into a single `Core::Trade`. Setting `is_individual_fill_individual_trade = true` makes every enter fill create an independent `Trade` with its own open price, TP, and FIFO slot.
- **Prediction-market identity:** Always read `ticker->underlying_symbol` (populated from series metadata); never re-derive the underlying symbol by parsing `series_slug`.
- **Timing constants:** `CEX_MIN_DELAY_BEFORE_CANCEL_MS = 1000` (1s), `POLYMARKET_ROLLBACK_TTL_MS = 7'200'000` (2h).

---

## `live_infra`

### `Live::StateUpdater`

Header: [`src/cpp/packages/live_infra/include/live_infra/state_updater.h`](https://github.com/JNeuvonen/keronisengine/blob/main/src/cpp/packages/live_infra/include/live_infra/state_updater.h)

Top-level live orchestrator (`RunTradeLoop()`). Owns `MarketDataStreamManager`, `live_event_queue_` (`BlockingConcurrentQueue<MarketEvent>`), `PaperExchange`, `QuoteHolds`, and direct-signing clients (Polymarket EIP-712 bulk, Hyperliquid Rust signer, Aster).

- **User-data stream watchdog:** `note_order_accepted_by_venue(account_id)` starts a 90-second expectation that an accepted order appears on the private user-data WebSocket.
- **Two-phase missing-order confirmation:** `confirm_order_not_found(order_key)` requires a second venue "not found" response at least `ORDER_NOT_FOUND_CONFIRM_MS` after the first before removing an order, preventing transient REST lag from dropping live orders.
- **Safe symbol bootstrap:** If venue symbol info fails to load, `SymbolInfoUnavailable` aborts strategy construction rather than creating a tickerless strategy that would orphan disk-restored trades.
- **Signal handling:** `SIGTERM` sets `termination_requested`. Once `main_loop_running` is true, the main loop exits cleanly, pulls resting entry quotes (`QuotesPullReason::SHUTDOWN`), flushes state to disk/Postgres, and releases `/tmp/live_infra_state_lock`.

### `Live::PaperExchange`

Header: [`src/cpp/packages/live_infra/include/live_infra/paper_exchange.h`](https://github.com/JNeuvonen/keronisengine/blob/main/src/cpp/packages/live_infra/include/live_infra/paper_exchange.h)

Dedicated `std::jthread` simulator decoupled from engine internals via the `PaperVenue` callback struct (wired to real books/feeds in `live_infra` and fakes in unit tests).

- **Timing constants:** `kSettleMs = 25` (waits 25 ms past `due_ms` for delayed book publication), `kDepthMaxAgeMs = 2000` (rejects stale depth snapshots), `kRestingCheckIntervalMs = 1000` (1s maker evaluation cadence).
- **Lock order invariant:** Always acquire `mutex_` before `ranges_mutex_`.
- **Cancel-before-print safety:** Each resting order tracks a `TradeRange { low, high, seen, frozen }`. Calling `cancel()` freezes the range immediately so subsequent market prints can never fill a canceled order.
- **Restart idempotency:** `closed_` tracks order IDs already closed or canceled by `PaperExchange` so disk-restored orders are not re-adopted.

### `Live::QuoteHolds`

Header: [`src/cpp/packages/live_infra/include/live_infra/quote_holds.h`](https://github.com/JNeuvonen/keronisengine/blob/main/src/cpp/packages/live_infra/include/live_infra/quote_holds.h)

Durable quote-pull state backed by PostgreSQL (`SELECT DISTINCT ON (COALESCE(key, '')) ... FROM engine_signal WHERE expires_at > NOW()`). Pulls are state, never acknowledged commands, so a restarted process comes back with quotes pulled until the hold expires or a newer `resume_quotes` row is written.

- **Precedence (`pull_reason()`):** `SHUTDOWN` > `PULL_SIGNAL` (global or per `strategy_id`) > `GUARD_STALE` (live non-paper strategies fail closed if `quote_guard` heartbeat age exceeds `kGuardStaleMs = 15000`).
