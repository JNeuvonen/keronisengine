#pragma once

#include <string>
#include <vector>

#include "market_data/market_event.h"

// Reusable historical-data reader. Wraps the existing
// ParquetUtils::load_aggr_trades_parquet + SqliteDatasets::load_*_ticks
// loaders behind a typed request/response API so multiple consumers
// (full_market_simulator's MarketDataReplayer, future BacktestState
// rewrite, ad-hoc tooling) can share one entry point.
//
// MVP scope: AggrTrades only. OrderBook (needs L2UpdateEvent
// construction) and OHLCV (uses Core::MarketEvent which is a different
// type than MarketData::MarketEvent) are deferred to follow-up steps
// once a scenario actually requires them. Funding is supported as of
// the swap_dip_sm work — events emitted with timestamp_ms shifted by
// FUNDING_POLLING_LATENCY_MS to simulate live polling delay.

namespace MarketData {

enum class HistoricalStreamType {
    AggrTrades,        // raw/trades/{exchange}/{market_type}/{symbol}/...
    Ohlcv,             // derived/bars/{exchange}/{market_type}/exchange/{interval}_ms/{symbol}/...
    PolymarketTrades,  // prediction_markets/polymarket/series/raw_data/{symbol}/
                       //   where `symbol` is the series slug (e.g., "btc-weeklies").
    BookTicker,        // flat_files/tardis/book_ticker/{exchange-name}/{market_type}/{symbol}/
                       //   Tardis Dev layout. exchange resolves to e.g. "binance-futures".
    L2Updates,         // flat_files/tardis/incremental_book_L2/{exchange}/{market_type}/{symbol}/
                       //   Per-row Tardis L2: is_snapshot + side + price + amount. Reader
                       //   groups by timestamp into a single MarketEvent::L2_UPDATE per frame.
    Funding,           // raw/funding/{exchange}/{market_type}/{symbol}/parquets/...
                       //   Cols: (timestamp i64 ms, funding_rate f64).
                       //   Reader shifts emit timestamp by +FUNDING_POLLING_LATENCY_MS so
                       //   SMs see funding events at the same observable delay as in live.
};

// Simulated funding-rate polling latency. In live mode our poller runs
// every few minutes; the funding rate is applied at T_apply but our
// observer sees it some seconds later. To match in historical replay
// we add this offset to every funding event's emit timestamp. SMs
// consume FundingEvent::timestamp_ms which already includes this
// offset, so the same SM code path works in both modes.
inline constexpr long long FUNDING_POLLING_LATENCY_MS = 15000;

struct HistoricalDataRequest {
    std::string exchange;       // "binance", "aster", "polygon", ...
    std::string market_type;    // "swap", "spot", "stock"
    std::string symbol;         // "BTCUSDT", "AAPL"
    HistoricalStreamType type;
    long long start_ts_ms;      // inclusive
    long long end_ts_ms;        // exclusive

    // Required when type == Ohlcv. Picks the on-disk bar interval
    // directory under derived/bars/{exchange}/{market_type}/exchange/.
    // Common values: 60000 (1m), 900000 (15m). Ignored for other types.
    long long ohlcv_interval_ms = 0;

    // Uses an already-resolved dataset directory instead of constructing one
    // from DATA_DIR_ROOT. This keeps readers aligned with simulation configs
    // that point at a portable or non-default market-data root.
    std::string dataset_path_override;
};

// Build the canonical dataset directory for {exchange, market_type,
// symbol, type}. Mirrors the layout that Python tooling
// (data_library/) writes to:
//   {DATA_DIR_ROOT}/raw/trades/{exchange}/{market_type}/{symbol}
// Honors DATA_DIR_ROOT env var (throws std::runtime_error if unset).
std::string dataset_path(const HistoricalDataRequest& req);

// Load all events in [start_ts_ms, end_ts_ms) from the canonical
// dataset for `req`. Auto-detects parquet vs sqlite layout based on
// filesystem presence (parquet preferred). Returns events sorted by
// timestamp (ascending).
//
// Returns empty vector if the time range has no data (NOT an error).
// Throws std::runtime_error with the attempted path embedded if the
// dataset directory is missing entirely.
//
// Pure function: safe to call concurrently from multiple threads with
// different requests.
std::vector<MarketEvent> read_historical(const HistoricalDataRequest& req);

}  // namespace MarketData
