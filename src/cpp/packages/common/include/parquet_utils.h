#pragma once
#include <arrow/api.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "historical_funding_rate_tick.h"
#include <unordered_set>

#include "market_data/alt_data_types.h"
#include "market_data/sec_filing.h"

namespace MarketData {
struct OhlcvDatasetTick;
}

struct AggrTradeDatasetTick;
struct PolymarketTradeTick;

namespace Core {
struct BacktestPredictionEvent;
}

namespace arrow {
class Table;
}  // namespace arrow

namespace ParquetUtils {

// Time span a hive-partitioned parquet path covers, in epoch ms.
//
// Bars are partitioned one file per month; older trees still hold one file per
// day, and both layouts coexist while a tree is migrated in place. Callers
// prune files against a requested window, so the span has to be the month when
// `day=` is absent -- treating a month file as a single day drops every bar
// after the 1st, silently, which is worse than failing to read it.
//
// Returns false when the path carries no year=/month= partition at all.
bool parse_partition_window(const std::string& path, long long& start_ms,
                            long long& span_ms);

std::shared_ptr<arrow::Table> scan_partitions(
    const std::string& base_path,
    const std::chrono::system_clock::time_point& start_time,
    const std::optional<std::chrono::system_clock::time_point>& end_time =
        std::nullopt);

long long get_latest_row_timestamp(const std::string& base_path,
                                   const std::string& time_column_name);

// The partition a tick lands in, e.g. "year=2026/month=08". Callers that track
// which months are dirty MUST derive the key from here rather than reproducing
// the format: a marker that disagrees with the writer stops that month being
// written, silently.
std::string ohlcv_partition_month_key(long long kline_open_time_ms);

int write_ohlcv_ticks_partitioned(
    const std::string& base_path,
    const std::vector<MarketData::OhlcvDatasetTick>& ticks);

int write_ohlcv_ticks_partitioned_minimal(
    const std::string& base_path,
    const std::vector<MarketData::OhlcvDatasetTick>& ticks);

std::shared_ptr<arrow::Table> scan_partitions_fast(
    const std::string& base_path, const std::string& time_column,
    const std::optional<std::chrono::system_clock::time_point>& start_time,
    const std::optional<std::chrono::system_clock::time_point>& end_time =
        std::nullopt);

std::vector<PolymarketTradeTick> load_polymarket_trades(
    const std::string& base_path, long long start_ts_ms, long long end_ts_ms);

std::vector<Core::BacktestPredictionEvent> load_prediction_ticks(
    const std::string& path, const std::string& model_name,
    long long start_ts_ms, long long end_ts_ms);

std::vector<AggrTradeDatasetTick> load_aggr_trades_parquet(
    const std::string& base_path, long long start_ts_ms, long long end_ts_ms);

std::vector<HistoricalFundingRateTick> load_funding_rate_ticks(
    const std::string& base_path, long long start_ts_ms, long long end_ts_ms);

// (timestamp, value_column) rows from a year=/month=/day= partitioned tree under
// base_path (or base_path/parquets), ascending, with timestamps in [start, end].
std::vector<MarketData::TimestampedValue> load_timestamped_values(
    const std::string& base_path, const std::string& value_column,
    long long start_ts_ms, long long end_ts_ms);

// SEC filings for one issuer from one form-family tree, e.g.
//   alt/filings/sec/registrations/0000320193
// Filings are filtered on FILED time here; the observable time (filed + the
// form's latency pad) is applied by SecDataset, which is the only thing a
// strategy is allowed to time against.
std::vector<MarketData::SecFiling> load_sec_filings(const std::string& base_path,
                                                    long long start_ts_ms,
                                                    long long end_ts_ms);

// Point-in-time symbol -> CIK rows from alt/filings/sec/symbol_cik_map.
std::vector<MarketData::SymbolCikRow> load_symbol_cik_map(
    const std::string& path);

// Every split for one ticker, from alt/corporate_actions/splits/<TICKER>.parquet.
// Unfiltered on purpose: the as-of cut belongs to AltDataManager, which is the
// only thing a strategy is allowed to time against. Returned oldest first.
std::vector<MarketData::StockSplitEvent> load_us_equity_stock_splits(const std::string& path);

// Every XBRL fact for one issuer from alt/filings/sec/facts/<CIK>/parquets/...,
// optionally narrowed to `tags` (empty means all -- an issuer's full history is
// tens of thousands of rows, so callers should narrow).
// Unfiltered on time on purpose: the as-of cut and the amendment rule belong to
// the module, which is the only thing a strategy is allowed to time against.
std::vector<MarketData::XbrlFact> load_us_equity_sec_facts(
    const std::string& cik_dir,
    const std::unordered_set<std::string>& tags);

}  // namespace ParquetUtils