# Data, Alt-Data & Historical Modeling

KeronisEngine models market data through three distinct subsystems—each matched to the mutability, keying, and point-in-time invariants of its data:

| Subsystem | Data shape | Keying | Mutability & hot path |
|---|---|---|---|
| **Streaming State Machines** ([State Machines & ONNX](state-machines.md)) | Tick-by-tick microstructure, order flow, cross-symbol features | Symbol / multi-symbol | Stateful in-memory accumulators (`react` → `emit_state`) fed lock-free via `StateMachineDispatcher` |
| **OHLCV Datasets** (`DatasetsManager` / `OhlcvDataset`) | Time / volume bars across crypto & equities | `(symbol, exchange, market_type, interval_ms)` | Mutable columnar SoA arrays updated by `BarAggregator` / WS and written back to Parquet |
| **Point-in-Time Alt-Data** (`AltDataManager` / `SecStore`) | SEC filings, XBRL facts, stock splits, funding, L/S ratios, trading status | **CIK** (filings/facts) or **symbol** (splits/series) | Explicitly preloaded, immutable `shared_ptr` snapshots swapped on reload; **zero reader locks** on decision path |

---

## 1. OHLCV Datasets (`OhlcvDataset` & `DatasetsManager`)

Headers: [`ohlcv_dataset.h`](https://github.com/JNeuvonen/keronisengine/blob/main/src/cpp/packages/market_data/include/market_data/ohlcv_dataset.h) · [`dataset_manager.h`](https://github.com/JNeuvonen/keronisengine/blob/main/src/cpp/packages/market_data/include/market_data/dataset_manager.h) · [`bar_aggregator.h`](https://github.com/JNeuvonen/keronisengine/blob/main/src/cpp/packages/market_data/include/market_data/bar_aggregator.h)

- **Columnar SoA layout:** `OhlcvDataset` stores `klines` alongside parallel contiguous vectors (`open_prices`, `high_prices`, `low_prices`, `close_prices`, `volumes`, `quote_asset_volumes`, `number_of_trades`, `taker_buy_base_volumes`, `taker_buy_quote_volumes`) and an attached `FeatureCache`, protected by a `std::shared_mutex` (`lock_for_read()`).
- **Live bar aggregation:** `BarAggregator::add_trade()` builds live open/closed bars from trade ticks according to `BarClosePolicy` and flushes completed bars into `DatasetsManager`. On trade-stream disconnect, `live_trade_stream_disconnected_reset()` clears affected open bars so partial bars are never committed.
- **Lookahead-safe disk scans:** `read_full_history_from_disk(from_inclusive, to_exclusive)` reads arbitrarily long bar windows straight from Parquet without bloating in-memory retention. `to_exclusive` is **required**: Parquet partition pruning operates at the file level (a monthly partition comes back whole), so row-level truncation is enforced inside this method to prevent backtests from reading future bars.
- **Corporate actions & RTH:** `rescale_for_split(cutoff_ms, ratio)` adjusts in-memory pre-split prices (`price / ratio`) and share volumes (`volume * ratio`). `include_outside_rth_ticks` is configured automatically by venue (Polygon equity datasets retain pre-/post-market bars; other equity venues filter outside Regular Trading Hours).
- **Retry-safe backfill:** `init_sync_bars()` returns `BackfillOutcome { Skipped, Fetched, NoData, Failed }` so throttled or errored backfills are retried rather than marked complete.

---

## 2. Point-in-Time Alt-Data (`AltDataManager` & `SecStore`)

Headers: [`alt_data_manager.h`](https://github.com/JNeuvonen/keronisengine/blob/main/src/cpp/packages/market_data/include/market_data/alt_data_manager.h) · [`alt_data_module.h`](https://github.com/JNeuvonen/keronisengine/blob/main/src/cpp/packages/market_data/include/market_data/alt_data_module.h)

Unlike `DatasetsManager`, alt-data modules are **read-only histories** written to `<DATA_DIR_ROOT>` by external syncers (EDGAR pollers, corporate-action jobs, exchange archive syncers) and loaded into C++ via `FileBackedAltDataModule<Payload>`:

- **Explicit session provisioning:** Call `preload(keys)` at session start; loading never happens on a decision thread. Missing keys are recorded in `keys_without_data()` (never silently swallowed) because missing coverage concentrates in delisted and newly listed names and is a direct survivorship-bias measurement.
- **Lock-free immutable snapshots:** Each key's payload lives behind an immutable `std::shared_ptr`. In live mode, `refresh_changed()` / `refresh_keys()` parses updated files outside the lock and swaps the pointer in under it, so readers never observe half-updated state.
- **Universal `as_of_ms` invariant:** Every accessor takes a decision timestamp (`as_of_ms` / `at_ms`) and refuses to return any event whose observable timestamp is in the future—even though on-disk files in backtests contain the full history.
- **`nullopt` vs. empty:** `std::nullopt` means *"never provisioned or unmapped"* (callers must fail closed); an empty vector means *"provisioned and nothing happened yet"*.

### SEC EDGAR Filings & XBRL Facts (Keyed by CIK)

Headers: [`sec_store.h`](https://github.com/JNeuvonen/keronisengine/blob/main/src/cpp/packages/market_data/include/market_data/sec_store.h) · [`sec_dataset.h`](https://github.com/JNeuvonen/keronisengine/blob/main/src/cpp/packages/market_data/include/market_data/sec_dataset.h) · [`symbol_cik_resolver.h`](https://github.com/JNeuvonen/keronisengine/blob/main/src/cpp/packages/market_data/include/market_data/symbol_cik_resolver.h) · [`us_equity_sec_facts_module.h`](https://github.com/JNeuvonen/keronisengine/blob/main/src/cpp/packages/market_data/include/market_data/us_equity_sec_facts_module.h)

- **Why CIK instead of ticker:** Tickers are recycled after delistings, whereas an SEC CIK stays with one legal issuer forever. `SymbolCikResolver` (owned by `AltDataManager`) resolves `symbol -> CIK` **point-in-time** (`cik_for(symbol, as_of_ms)`) and refuses ambiguous overlaps rather than guessing.
- **SEC Filings (`SecStore` / `SecDataset`):** Stored under `<DATA_DIR_ROOT>/alt/filings/sec`. Sorted by `observable_ts_ms` for binary-search access via `visible_filings(as_of_ms)`, `last_filed_ms(forms, as_of_ms)`, `count_in_window(...)`, and `days_since(...)`. Supports live appends (`SecDataset::add()`) and tracks feed health by `last_poll_ms()` (not last filing time, since an issuer may legitimately file nothing for months).
- **XBRL Facts (`UsEquitySecFactsModule`):** Year-partitioned Parquet trees under `<DATA_DIR_ROOT>/alt/filings/sec/facts/<CIK>/`. Narrows parsed XBRL tags at load time (`set_tags_of_interest()`, default `kSharesOutstandingTag`). `facts_as_of(cik, as_of_ms)` and `shares_outstanding_as_of(cik, as_of_ms)` return only filings where `filed_ts_ms + kFactsDefaultMs <= as_of_ms` and keep only the latest filing per `(taxonomy, tag, unit, period_start, period_end)`—preventing later amendments or reverse-split restatements from leaking backward.

### US Equity Stock Splits (`UsEquityStockSplitsModule`)

Header: [`us_equity_stock_splits_module.h`](https://github.com/JNeuvonen/keronisengine/blob/main/src/cpp/packages/market_data/include/market_data/us_equity_stock_splits_module.h)

- Stored per ticker under `<DATA_DIR_ROOT>/alt/corporate_actions/splits/` (Polygon reference format).
- `splits_as_of(symbol, as_of_ms)` and `cumulative_ratio(symbol, from_ms, to_ms)` allow converting historical filed share counts onto a later price basis without seeing future splits.
- `absent_file_is_empty_payload()` is `true` (most equities never split, unlike missing SEC facts which return `nullopt`).

### Binance Perpetual Series & Trading Status

Headers: [`binance_swap_series_modules.h`](https://github.com/JNeuvonen/keronisengine/blob/main/src/cpp/packages/market_data/include/market_data/binance_swap_series_modules.h) · [`binance_swap_trading_status_module.h`](https://github.com/JNeuvonen/keronisengine/blob/main/src/cpp/packages/market_data/include/market_data/binance_swap_trading_status_module.h)

- **`BinanceSwapFundingModule` (`raw/funding/binance/swap/<SYMBOL>`):** Day-partitioned funding settlement history. `settled(symbol, from_ms, to_ms)` sums settlements in `[from_ms, to_ms)` and returns `nullopt` if `from_ms` falls outside `[first_settlement, last_settlement + 8h]` so pre-listing or stale gaps are never read as zero.
- **`BinanceSwapAccountLongShortRatioModule` (`raw/long_short_ratio/binance/swap/<SYMBOL>`):** 5-minute global account long/short ratios with `latest(symbol, at_ms, max_age_ms)`. Both series modules inherit `TimestampedSeriesModule`, which supports lock-free window widening (`request_history_from_ms()`) so live reloads only parse the history horizon strategies actually need.
- **`BinanceSwapTradingStatusModule` (`alt/trading_status/binance/swap/<SYMBOL>.json`):** Tracks delisting and trading-halt announcements with separate `available_ms`, `effective_ms`, and `settlement_ms` timestamps (`entry_allowed(symbol, at_ms)`, `announced_exit(symbol, at_ms)`).

---

## 3. Custom Datasets & Alt-Data Primitives

KeronisEngine provides two first-class ways to feed custom datasets into strategies without forking the engine:

### Tier 1 — Zero-C++ Point-in-Time Series (`CustomSeriesModule`)

Header: [`custom_series_module.h`](https://github.com/JNeuvonen/keronisengine/blob/main/src/cpp/packages/market_data/include/market_data/custom_series_module.h)

Write JSONL (`.jsonl`) or JSON array (`.json`) files from any Python/cron pipeline into `<DATA_DIR_ROOT>/alt/custom/<dataset_name>/<key>.jsonl` (or a directory `<key>/` of files). Every row requires an integer `available_ts_ms` (UTC ms when the datum became observable) plus one or more numeric fields:

```json
{"available_ts_ms": 1732632000000, "value": 0.85, "confidence": 0.92}
```

Query directly from any strategy via `AltDataManager::instance().custom_series(...)`:

```cpp
auto& flows = MarketData::AltDataManager::instance().custom_series("onchain_flows");
flows.preload({"BTCUSDT", "ETHUSDT"});

// Binary-searches available_ts_ms <= as_of_ms; returns nullopt if unprovisioned or stale.
auto score = flows.latest_value("BTCUSDT", engine.get_time(), "value", /*max_age_ms=*/600'000);
auto recent = flows.window("BTCUSDT", engine.get_time() - 3'600'000, engine.get_time());
```

`CustomSeriesModule` enforces `available_ts_ms <= as_of_ms` by construction, sorts rows on parse, fails loudly on missing `available_ts_ms`, tracks `keys_without_data()`, and participates automatically in `AltDataManager::refresh_all_changed()`.

### Tier 2 — Out-of-Tree Typed Modules (`KERONIS_REGISTER_ALT_DATA`)

Header: [`alt_data_registry.h`](https://github.com/JNeuvonen/keronisengine/blob/main/src/cpp/packages/market_data/include/market_data/alt_data_registry.h)

For structured domain data (custom events, injury reports, option surfaces), subclass `FileBackedAltDataModule<MyPayload>`, register it with `KERONIS_REGISTER_ALT_DATA`, and compile it out-of-tree via `-DKERONIS_STRATEGY_DIRS`:

```cpp
class MyEventsModule final
    : public MarketData::FileBackedAltDataModule<std::vector<MyEvent>> {
   public:
    MarketData::AltDataSource source() const override { return MarketData::AltDataSource::Custom; }
    std::optional<MyEvent> latest_event(const std::string& key, long long as_of_ms) const;
   protected:
    std::string path_for(const std::string& key) const override;
    std::vector<MyEvent> parse(const std::string& path) const override;
};

KERONIS_REGISTER_ALT_DATA("my_events", MyEventsModule);

// Access from any strategy:
auto& mod = MarketData::AltDataManager::instance().custom<MyEventsModule>("my_events");
mod.preload(symbols);
```

---

## 4. Historical Data Reader & On-Disk Layout (`HistoricalReader`)

Header: [`historical_reader.h`](https://github.com/JNeuvonen/keronisengine/blob/main/src/cpp/packages/market_data/include/market_data/historical_reader.h)

`MarketData::read_historical(HistoricalDataRequest)` provides a pure, thread-safe entry point that auto-detects Parquet vs. SQLite (preferring Parquet) and returns timestamp-sorted `MarketEvent` vectors in `[start_ts_ms, end_ts_ms)`:

| `HistoricalStreamType` | Canonical path under `<DATA_DIR_ROOT>` | Notes |
|---|---|---|
| `AggrTrades` | `raw/trades/{exchange}/{market_type}/{symbol}/` | Normalized trade prints |
| `Ohlcv` | `derived/bars/{exchange}/{market_type}/exchange/{interval}_ms/{symbol}/` | Requires `ohlcv_interval_ms` (e.g. `60000`, `900000`) |
| `PolymarketTrades` | `prediction_markets/polymarket/series/raw_data/{symbol}/` | `symbol` is the series slug (e.g. `btc-weeklies`) |
| `BookTicker` | `flat_files/tardis/book_ticker/{exchange}/{market_type}/{symbol}/` | Tardis BBA flat files |
| `L2Updates` | `flat_files/tardis/incremental_book_L2/{exchange}/{market_type}/{symbol}/` | Groups per-row L2 snapshots/deltas into `MarketEvent::L2_UPDATE` frames |
| `Funding` | `raw/funding/{exchange}/{market_type}/{symbol}/parquets/` | Automatically shifts emit timestamp by `+FUNDING_POLLING_LATENCY_MS` (`15000 ms`) to match live polling delay |

### How historical modeling connects to training and live

1. **Offline feature panels (`historical_feature_gen`):** Uses `MergeIterator` to merge multiple historical streams (`AggrTrades`, `BookTicker`, `L2Updates`, `Funding`, `Ohlcv`) in strict timestamp order and feeds them through any registered `StrategyStateMachine` to emit Parquet training panels.
2. **Backtest replay (`backtest_runner`):** Replays Parquet bars (`OhlcvDataset`), trades, order books, and precomputed `market_data.predictions` through `BacktestState`, with `AltDataManager` and `SecStore` enforcing `as_of_ms` visibility on every corporate action, filing, and funding query.
3. **Live & paper (`live_infra`):** Live WebSockets feed `BarAggregator`, `StateMachineDispatcher`, and `RingBufferBook`, while external syncers update `<DATA_DIR_ROOT>/alt/...` on disk and trigger `AltDataManager::refresh_changed()` / `refresh_keys()` without restarting the engine.
