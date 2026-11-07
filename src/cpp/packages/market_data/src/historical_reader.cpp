#include "market_data/historical_reader.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>

#include <future>
#include <regex>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/reader.h>

#include "aggr_trade_dataset_tick.h"
#include "parquet_utils.h"
#include "ohlcv_dataset_tick.h"
#include "parquet_utils.h"
#include "polymarket_trade_tick.h"
#include "sqlite_datasets.h"

namespace MarketData {

namespace {

std::string require_data_dir_root() {
    const char* env = std::getenv("DATA_DIR_ROOT");
    if (env == nullptr || *env == '\0') {
        throw std::runtime_error(
            "DATA_DIR_ROOT env var is required for historical data reads");
    }
    return std::string(env);
}

const char* stream_subdir(HistoricalStreamType type) {
    switch (type) {
        case HistoricalStreamType::AggrTrades:
            return "raw/trades";
        case HistoricalStreamType::Ohlcv:
            return "derived/bars";
        case HistoricalStreamType::PolymarketTrades:
            return "prediction_markets/polymarket/series/raw_data";
        case HistoricalStreamType::BookTicker:
            return "flat_files/tardis/book_ticker";
        case HistoricalStreamType::L2Updates:
            return "flat_files/tardis/incremental_book_L2";
        case HistoricalStreamType::Funding:
            return "raw/funding";
    }
    return "raw/trades";
}

// Tardis Dev's internal exchange labels (e.g., "binance-futures") show
// up inside the parquet rows but the on-disk directory tree uses the
// canonical exchange name (the same as our raw/trades/ layout). No
// mapping needed at path-build time — keep this as a passthrough so
// future divergence has a single place to add overrides.
std::string tardis_exchange_segment(const std::string& exchange,
                                     const std::string& /*market_type*/) {
    return exchange;
}

// Inline tick struct — used only by the Tardis BBA path. Exposing
// publicly would invite consumers in production code, which we don't
// want (the engine has its own BBA shape via Core::OrderBook).
struct BookTickerTick {
    long long timestamp_us = 0;   // Tardis source timestamp in microseconds
    double bid_price = 0.0;
    double bid_amount = 0.0;
    double ask_price = 0.0;
    double ask_amount = 0.0;
};

// Tardis L2 raw row — one per price-level update. Multiple rows
// share a `timestamp_us` when a single WS frame contained several
// level changes; the loader groups by timestamp into one MarketEvent.
struct L2RawTick {
    long long timestamp_us = 0;
    bool is_snapshot = false;
    bool is_ask = false;          // false = bid
    double price = 0.0;
    double amount = 0.0;          // 0 = level removed
};

// Load Tardis incremental_book_L2 parquet files in the requested time
// range. Same partition layout as book_ticker.
std::vector<L2RawTick> load_incremental_l2_(
    const std::string& base_path, long long start_ts_ms,
    long long end_ts_ms) {
    namespace fs = std::filesystem;
    if (!fs::exists(base_path)) return {};


    std::vector<fs::path> files;
    for (const auto& entry : fs::recursive_directory_iterator(base_path)) {
        if (!entry.is_regular_file() ||
            entry.path().extension() != ".parquet") continue;
        if (fs::file_size(entry.path()) == 0) continue;
        std::string path_str = entry.path().string();
        std::smatch m;
                long long file_day_ms = 0;
        long long file_span_ms = 0;
        if (!ParquetUtils::parse_partition_window(
                path_str, file_day_ms, file_span_ms))
            continue;
        if (file_day_ms + file_span_ms - 1 < start_ts_ms) continue;
        if (file_day_ms > end_ts_ms) continue;
        files.push_back(entry.path());
    }
    if (files.empty()) return {};

    std::vector<L2RawTick> out;
    arrow::MemoryPool* pool = arrow::default_memory_pool();
    const long long start_us = start_ts_ms * 1000LL;
    const long long end_us = end_ts_ms * 1000LL;

    for (const auto& path : files) {
        auto infile = arrow::io::ReadableFile::Open(path.string());
        if (!infile.ok()) continue;
        auto reader_result =
            parquet::arrow::OpenFile(infile.ValueOrDie(), pool);
        if (!reader_result.ok()) continue;
        auto reader = std::move(reader_result.ValueOrDie());

        // Stream row-groups individually — avoids ReadTable's full-file
        // materialization (Tardis L2 day = ~139M rows / GB-scale).
        const int num_row_groups = reader->num_row_groups();

        // Dictionary-or-plain string handle for the `side` column.
        auto is_ask_at = [](const std::shared_ptr<arrow::Array>& arr,
                             int64_t i) -> bool {
            if (arr->type_id() == arrow::Type::DICTIONARY) {
                auto dict_arr =
                    std::static_pointer_cast<arrow::DictionaryArray>(arr);
                auto indices =
                    std::static_pointer_cast<arrow::Int32Array>(
                        dict_arr->indices());
                auto dict =
                    std::static_pointer_cast<arrow::StringArray>(
                        dict_arr->dictionary());
                return dict->GetView(indices->Value(i)) == "ask";
            }
            // Plain string (LARGE or normal). GetView avoids string copy.
            if (arr->type_id() == arrow::Type::LARGE_STRING) {
                return std::static_pointer_cast<arrow::LargeStringArray>(
                            arr)
                           ->GetView(i) == "ask";
            }
            return std::static_pointer_cast<arrow::StringArray>(arr)
                       ->GetView(i) == "ask";
        };

        bool past_end = false;
        for (int rg = 0; rg < num_row_groups && !past_end; ++rg) {
            std::shared_ptr<arrow::Table> rg_table;
            if (!reader->ReadRowGroup(rg, &rg_table).ok()) continue;
            auto ts_col = rg_table->GetColumnByName("timestamp");
            auto snap_col = rg_table->GetColumnByName("is_snapshot");
            auto side_col = rg_table->GetColumnByName("side");
            auto price_col = rg_table->GetColumnByName("price");
            auto amt_col = rg_table->GetColumnByName("amount");
            if (!ts_col || !snap_col || !side_col || !price_col ||
                !amt_col)
                continue;
            for (int chunk_i = 0; chunk_i < ts_col->num_chunks();
                 ++chunk_i) {
                auto ts_arr = std::static_pointer_cast<arrow::Int64Array>(
                    ts_col->chunk(chunk_i));
                auto snap_arr =
                    std::static_pointer_cast<arrow::BooleanArray>(
                        snap_col->chunk(chunk_i));
                auto side_chunk = side_col->chunk(chunk_i);
                auto price_arr =
                    std::static_pointer_cast<arrow::DoubleArray>(
                        price_col->chunk(chunk_i));
                auto amt_arr =
                    std::static_pointer_cast<arrow::DoubleArray>(
                        amt_col->chunk(chunk_i));
                const int64_t n = ts_arr->length();
                if (n > 0 && ts_arr->Value(0) >= end_us) {
                    past_end = true;
                    break;
                }
                for (int64_t i = 0; i < n; ++i) {
                    const long long ts_us = ts_arr->Value(i);
                    if (ts_us < start_us) continue;
                    if (ts_us >= end_us) {
                        past_end = true;
                        break;
                    }
                    out.push_back(L2RawTick{
                        ts_us, snap_arr->Value(i),
                        is_ask_at(side_chunk, i),
                        price_arr->Value(i), amt_arr->Value(i),
                    });
                }
            }
        }
    }
    return out;
}

// Load Tardis book_ticker parquet files in the requested time range.
// Layout: base_path/year=YYYY/month=MM/day=DD/data.parquet.
// Filters at file-day granularity (any file overlapping
// [start_ts_ms, end_ts_ms) is loaded), then row-filters in tick_to_event.
std::vector<BookTickerTick> load_book_ticker_(
    const std::string& base_path, long long start_ts_ms,
    long long end_ts_ms) {
    namespace fs = std::filesystem;
    if (!fs::exists(base_path)) return {};


    std::vector<fs::path> files;
    for (const auto& entry : fs::recursive_directory_iterator(base_path)) {
        if (!entry.is_regular_file() ||
            entry.path().extension() != ".parquet") continue;
        if (fs::file_size(entry.path()) == 0) continue;
        std::string path_str = entry.path().string();
        std::smatch m;
                long long file_day_ms = 0;
        long long file_span_ms = 0;
        if (!ParquetUtils::parse_partition_window(
                path_str, file_day_ms, file_span_ms))
            continue;
        if (file_day_ms + file_span_ms - 1 < start_ts_ms) continue;
        if (file_day_ms > end_ts_ms) continue;
        files.push_back(entry.path());
    }
    if (files.empty()) return {};

    std::vector<BookTickerTick> out;
    arrow::MemoryPool* pool = arrow::default_memory_pool();
    const long long start_us = start_ts_ms * 1000LL;
    const long long end_us = end_ts_ms * 1000LL;

    for (const auto& path : files) {
        auto infile = arrow::io::ReadableFile::Open(path.string());
        if (!infile.ok()) continue;
        auto reader_result =
            parquet::arrow::OpenFile(infile.ValueOrDie(), pool);
        if (!reader_result.ok()) continue;
        auto reader = std::move(reader_result.ValueOrDie());

        // Stream row-groups — Tardis book_ticker day-files hold ~36M
        // rows / multi-GB; ReadTable would OOM or stall. Same pattern
        // as the L2 loader below.
        const int num_row_groups = reader->num_row_groups();
        bool past_end = false;
        for (int rg = 0; rg < num_row_groups && !past_end; ++rg) {
            std::shared_ptr<arrow::Table> rg_table;
            if (!reader->ReadRowGroup(rg, &rg_table).ok()) continue;
            auto ts_col = rg_table->GetColumnByName("timestamp");
            auto ap_col = rg_table->GetColumnByName("ask_price");
            auto aa_col = rg_table->GetColumnByName("ask_amount");
            auto bp_col = rg_table->GetColumnByName("bid_price");
            auto ba_col = rg_table->GetColumnByName("bid_amount");
            if (!ts_col || !ap_col || !aa_col || !bp_col || !ba_col)
                continue;
            for (int chunk_i = 0; chunk_i < ts_col->num_chunks();
                 ++chunk_i) {
                auto ts_arr = std::static_pointer_cast<arrow::Int64Array>(
                    ts_col->chunk(chunk_i));
                auto ap_arr = std::static_pointer_cast<arrow::DoubleArray>(
                    ap_col->chunk(chunk_i));
                auto aa_arr = std::static_pointer_cast<arrow::DoubleArray>(
                    aa_col->chunk(chunk_i));
                auto bp_arr = std::static_pointer_cast<arrow::DoubleArray>(
                    bp_col->chunk(chunk_i));
                auto ba_arr = std::static_pointer_cast<arrow::DoubleArray>(
                    ba_col->chunk(chunk_i));
                const int64_t n = ts_arr->length();
                if (n > 0 && ts_arr->Value(0) >= end_us) {
                    past_end = true;
                    break;
                }
                for (int64_t i = 0; i < n; ++i) {
                    const long long ts_us = ts_arr->Value(i);
                    if (ts_us < start_us) continue;
                    if (ts_us >= end_us) {
                        past_end = true;
                        break;
                    }
                    out.push_back(BookTickerTick{
                        ts_us, bp_arr->Value(i), ba_arr->Value(i),
                        ap_arr->Value(i), aa_arr->Value(i),
                    });
                }
            }
        }
    }
    return out;
}

// AggrTradeDatasetTick → MarketData::MarketEvent (TRADE variant).
// Wire-shape note: TradeEvent::is_buyer_maker matches the Aster/Binance
// convention; AggrTradeDatasetTick stores it directly (no flip needed).
MarketEvent tick_to_event(const AggrTradeDatasetTick& tick,
                          const HistoricalDataRequest& req) {
    TradeEvent te;
    te.symbol = req.symbol;
    te.exchange_name = req.exchange;
    te.market_type = req.market_type;
    te.price = tick.price;
    te.qty = tick.quantity;
    te.is_buyer_maker = tick.is_buyer_maker;
    te.timestamp_ms = tick.timestamp;

    MarketEvent ev;
    ev.type = MarketEvent::TRADE;
    ev.trade = te;
    return ev;
}

MarketEvent polymarket_trade_to_event(
    const PolymarketTradeTick& tick) {
    PolymarketLastTradeEvent pe;
    pe.condition_id = tick.condition_id;
    pe.asset_id = tick.clob_token_id;
    pe.price = tick.price;
    pe.size = tick.size;
    pe.is_buy = (tick.side == "BUY");
    pe.timestamp_ms = tick.timestamp;

    MarketEvent ev;
    ev.type = MarketEvent::POLYMARKET_LAST_TRADE;
    ev.polymarket_last_trade = pe;
    return ev;
}

// Group consecutive Tardis L2 rows sharing a timestamp into one
// MarketEvent::L2_UPDATE. is_snapshot rows produce a FULL_REFRESH;
// non-snapshot rows produce an INCREMENTAL update.
std::vector<MarketEvent> group_l2_to_events_(
    const std::vector<L2RawTick>& ticks,
    const HistoricalDataRequest& req) {
    std::vector<MarketEvent> out;
    if (ticks.empty()) return out;

    auto flush = [&](size_t start, size_t end) {
        // All ticks in [start, end) share timestamp_us + is_snapshot.
        const auto& first = ticks[start];
        L2UpdateEvent le;
        le.exchange_name = req.exchange;
        le.market_type = req.market_type;
        le.symbol = req.symbol;
        le.sub_type = first.is_snapshot
                          ? L2UpdateEvent::FULL_REFRESH
                          : L2UpdateEvent::INCREMENTAL;
        Core::OrderBookUpdate update;
        update.symbol_full = req.symbol + ":" + req.exchange + ":" +
                              req.market_type;
        update.timestamp = first.timestamp_us / 1000LL;
        update.sequence_start = first.timestamp_us;
        update.sequence_end = first.timestamp_us;
        for (size_t i = start; i < end; ++i) {
            const auto& t = ticks[i];
            if (t.is_ask) {
                update.asks.emplace_back(t.price, t.amount);
            } else {
                update.bids.emplace_back(t.price, t.amount);
            }
        }
        le.update = std::move(update);
        // FULL_REFRESH stays in `update` only — full_refresh_orderbook
        // is the engine-side build-up, not the wire input.

        MarketEvent ev;
        ev.type = MarketEvent::L2_UPDATE;
        ev.l2_update = std::move(le);
        out.push_back(std::move(ev));
    };

    size_t group_start = 0;
    for (size_t i = 1; i < ticks.size(); ++i) {
        if (ticks[i].timestamp_us != ticks[group_start].timestamp_us ||
            ticks[i].is_snapshot != ticks[group_start].is_snapshot) {
            flush(group_start, i);
            group_start = i;
        }
    }
    flush(group_start, ticks.size());
    return out;
}

MarketEvent book_ticker_to_event(const BookTickerTick& tick,
                                  const HistoricalDataRequest& req) {
    BBAEvent be;
    be.symbol = req.symbol;
    be.exchange_name = req.exchange;
    be.market_type = req.market_type;
    be.bid_price = tick.bid_price;
    be.bid_qty = tick.bid_amount;
    be.ask_price = tick.ask_price;
    be.ask_qty = tick.ask_amount;
    be.timestamp_ms = tick.timestamp_us / 1000LL;  // Tardis is μs

    MarketEvent ev;
    ev.type = MarketEvent::BBA;
    ev.bba = be;
    return ev;
}

// Funding parquet row — schema written by Python's
// sync_binance_funding_rates job: (timestamp i64 ms, funding_rate f64).
struct FundingTick {
    long long timestamp_ms = 0;
    double funding_rate = 0.0;
};

// Load funding-rate parquet files in the requested time range. On-disk
// layout under raw/funding/{exchange}/{market_type}/{symbol}/parquets/
// year=YYYY/month=MM/day=DD/*.parquet. Funding cadence is 8h (3
// entries/day) so volume is tiny — a single ReadTable per day-file is
// fine, no row-group streaming needed.
std::vector<FundingTick> load_funding_parquet_(
    const std::string& base_path, long long start_ts_ms,
    long long end_ts_ms) {
    namespace fs = std::filesystem;
    if (!fs::exists(base_path)) return {};


    // Filter to the simulated-observation window: funding events whose
    // apply_time falls in [start - latency, end - latency) will be
    // emitted in [start, end). We pull a touch wider for safety.
    const long long src_start_ms = start_ts_ms - FUNDING_POLLING_LATENCY_MS;
    const long long src_end_ms = end_ts_ms - FUNDING_POLLING_LATENCY_MS;

    std::vector<fs::path> files;
    for (const auto& entry : fs::recursive_directory_iterator(base_path)) {
        if (!entry.is_regular_file() ||
            entry.path().extension() != ".parquet") continue;
        if (fs::file_size(entry.path()) == 0) continue;
        std::string path_str = entry.path().string();
        std::smatch m;
                long long file_day_ms = 0;
        long long file_span_ms = 0;
        if (!ParquetUtils::parse_partition_window(
                path_str, file_day_ms, file_span_ms))
            continue;
        if (file_day_ms + file_span_ms - 1 < src_start_ms) continue;
        if (file_day_ms > src_end_ms) continue;
        files.push_back(entry.path());
    }
    if (files.empty()) return {};

    std::vector<FundingTick> out;
    arrow::MemoryPool* pool = arrow::default_memory_pool();

    for (const auto& path : files) {
        auto infile = arrow::io::ReadableFile::Open(path.string());
        if (!infile.ok()) continue;
        auto reader_result =
            parquet::arrow::OpenFile(infile.ValueOrDie(), pool);
        if (!reader_result.ok()) continue;
        auto reader = std::move(reader_result.ValueOrDie());

        std::shared_ptr<arrow::Table> table;
        if (!reader->ReadTable(&table).ok() || !table) continue;
        auto ts_col = table->GetColumnByName("timestamp");
        auto rate_col = table->GetColumnByName("funding_rate");
        if (!ts_col || !rate_col) continue;

        for (int chunk_i = 0; chunk_i < ts_col->num_chunks(); ++chunk_i) {
            auto ts_arr = std::static_pointer_cast<arrow::Int64Array>(
                ts_col->chunk(chunk_i));
            auto rate_arr = std::static_pointer_cast<arrow::DoubleArray>(
                rate_col->chunk(chunk_i));
            const int64_t n = ts_arr->length();
            for (int64_t i = 0; i < n; ++i) {
                const long long ts_ms = ts_arr->Value(i);
                if (ts_ms < src_start_ms || ts_ms >= src_end_ms) continue;
                out.push_back(FundingTick{ts_ms, rate_arr->Value(i)});
            }
        }
    }
    return out;
}

// FundingTick → MarketEvent::FUNDING. Applies the simulated
// polling-latency offset to the event timestamp so SMs see funding
// events at their *observable* time, matching live polling.
MarketEvent funding_tick_to_event(const FundingTick& tick,
                                   const HistoricalDataRequest& req) {
    FundingEvent fe;
    fe.symbol = req.symbol;
    fe.exchange_name = req.exchange;
    fe.market_type = req.market_type;
    fe.funding_rate = tick.funding_rate;
    fe.timestamp_ms = tick.timestamp_ms + FUNDING_POLLING_LATENCY_MS;

    MarketEvent ev;
    ev.type = MarketEvent::FUNDING;
    ev.funding = fe;
    return ev;
}

MarketEvent ohlcv_to_event(const OhlcvDatasetTick& tick,
                            const HistoricalDataRequest& req) {
    OhlcvEvent oe;
    oe.symbol = req.symbol;
    oe.exchange_name = req.exchange;
    oe.market_type = req.market_type;
    oe.kline_open_time_ms = tick.kline_open_time;
    oe.kline_close_time_ms = tick.kline_close_time;
    oe.open_price = tick.open_price;
    oe.high_price = tick.high_price;
    oe.low_price = tick.low_price;
    oe.close_price = tick.close_price;
    oe.volume = tick.volume;
    oe.quote_asset_volume = tick.quote_asset_volume;
    oe.number_of_trades = tick.number_of_trades;
    oe.taker_buy_base_volume = tick.taker_buy_base_volume;
    oe.taker_buy_quote_volume = tick.taker_buy_quote_volume;

    MarketEvent ev;
    ev.type = MarketEvent::OHLCV;
    ev.ohlcv = oe;
    return ev;
}

long long event_ts(const MarketEvent& ev) {
    switch (ev.type) {
        case MarketEvent::TRADE: return ev.trade->timestamp_ms;
        case MarketEvent::BBA: return ev.bba->timestamp_ms;
        case MarketEvent::L2_UPDATE: return ev.l2_update->update.timestamp;
        case MarketEvent::OHLCV: return ev.ohlcv->kline_open_time_ms;
        case MarketEvent::FUNDING: return ev.funding->timestamp_ms;
        case MarketEvent::POLYMARKET_LAST_TRADE:
            return ev.polymarket_last_trade->timestamp_ms;
        default: return 0;
    }
}

}  // namespace

std::string dataset_path(const HistoricalDataRequest& req) {
    if (!req.dataset_path_override.empty()) {
        return req.dataset_path_override;
    }
    if (req.type == HistoricalStreamType::Ohlcv) {
        // OHLCV layout: derived/bars/{exchange}/{market_type}/exchange/{interval}_ms/{symbol}
        // For Polygon stocks the on-disk dir uses "exchange" bar type with
        // 60_000 (1m) or 900_000 (15m) interval. The interval is encoded
        // in HistoricalDataRequest::ohlcv_interval_ms (added below).
        return require_data_dir_root() + "/" + stream_subdir(req.type) +
               "/" + req.exchange + "/" + req.market_type +
               "/exchange/" +
               std::to_string(req.ohlcv_interval_ms) + "_ms/" +
               req.symbol;
    }
    if (req.type == HistoricalStreamType::PolymarketTrades) {
        // Polymarket layout: prediction_markets/polymarket/series/raw_data/{slug}
        // The "symbol" field carries the series slug (e.g.
        // "btc-weeklies", "doge-up-or-down-5m"). exchange + market_type
        // ignored — the on-disk path is venue-fixed.
        return require_data_dir_root() + "/" + stream_subdir(req.type) +
               "/" + req.symbol;
    }
    if (req.type == HistoricalStreamType::BookTicker ||
        req.type == HistoricalStreamType::L2Updates) {
        // Tardis layout: flat_files/tardis/{stream}/{exchange}/{market_type}/{symbol}
        // Both book_ticker + incremental_book_L2 share the same shape.
        return require_data_dir_root() + "/" + stream_subdir(req.type) +
               "/" + tardis_exchange_segment(req.exchange,
                                              req.market_type) +
               "/" + req.market_type + "/" + req.symbol;
    }
    return require_data_dir_root() + "/" + stream_subdir(req.type) +
           "/" + req.exchange + "/" + req.market_type + "/" + req.symbol;
}

std::vector<MarketEvent> read_historical(const HistoricalDataRequest& req) {
    const std::string base = dataset_path(req);
    if (!std::filesystem::exists(base)) {
        throw std::runtime_error(
            "historical dataset not found: " + base);
    }

    // Format auto-detection — same convention as backtest_state.cpp:875.
    // Parquet preferred (faster); SQLite fallback for older datasets.
    const bool is_parquet =
        std::filesystem::exists(base + "/parquets");

    std::vector<MarketEvent> out;

    switch (req.type) {
        case HistoricalStreamType::AggrTrades: {
            std::vector<AggrTradeDatasetTick> ticks;
            if (is_parquet) {
                ticks = ParquetUtils::load_aggr_trades_parquet(
                    base, req.start_ts_ms, req.end_ts_ms);
            } else {
                ticks = SqliteDatasets::load_trades_dataset_dir(
                    base, req.start_ts_ms, req.end_ts_ms);
            }
            out.reserve(ticks.size());
            for (const auto& t : ticks) out.push_back(tick_to_event(t, req));
            break;
        }
        case HistoricalStreamType::Ohlcv: {
            using namespace std::chrono;
            const std::string parquet_dir = base + "/parquets";
            if (!std::filesystem::exists(parquet_dir)) {
                throw std::runtime_error(
                    "OHLCV dataset present but not in parquet form: " +
                    base + " (sqlite OHLCV path not yet implemented)");
            }
            const auto start = time_point<system_clock, milliseconds>(
                milliseconds(req.start_ts_ms));
            const auto end = time_point<system_clock, milliseconds>(
                milliseconds(req.end_ts_ms));
            auto table = ParquetUtils::scan_partitions_fast(
                parquet_dir, "kline_open_time_ms", start, end);
            if (!table) break;
            auto ohlcv_ticks = OhlcvDatasetTick::from_arrow_table(table);
            out.reserve(ohlcv_ticks.size());
            for (const auto& t : ohlcv_ticks) {
                if (t.kline_open_time >= req.start_ts_ms &&
                    t.kline_open_time < req.end_ts_ms) {
                    out.push_back(ohlcv_to_event(t, req));
                }
            }
            break;
        }
        case HistoricalStreamType::PolymarketTrades: {
            // Polymarket layout has no `parquets/` subdir — files live
            // directly under {base}/year=YYYY/month=MM/day=DD/*.parquet.
            // load_polymarket_trades walks this layout itself.
            auto ticks = ParquetUtils::load_polymarket_trades(
                base, req.start_ts_ms, req.end_ts_ms);
            out.reserve(ticks.size());
            for (const auto& t : ticks) {
                out.push_back(polymarket_trade_to_event(t));
            }
            break;
        }
        case HistoricalStreamType::BookTicker: {
            // Tardis layout — same year=/month=/day= partition style as
            // polymarket trades. Loader is inline (no parquet_utils
            // export — keeps the Tardis-specific schema contained).
            auto ticks = load_book_ticker_(
                base, req.start_ts_ms, req.end_ts_ms);
            out.reserve(ticks.size());
            for (const auto& t : ticks) {
                out.push_back(book_ticker_to_event(t, req));
            }
            break;
        }
        case HistoricalStreamType::L2Updates: {
            auto raw = load_incremental_l2_(
                base, req.start_ts_ms, req.end_ts_ms);
            out = group_l2_to_events_(raw, req);
            break;
        }
        case HistoricalStreamType::Funding: {
            // Funding parquets are tiny (8h cadence × few-MB/day-file).
            // Loader handles the apply_time → observable_time offset.
            auto ticks = load_funding_parquet_(
                base, req.start_ts_ms, req.end_ts_ms);
            out.reserve(ticks.size());
            for (const auto& t : ticks) {
                out.push_back(funding_tick_to_event(t, req));
            }
            break;
        }
    }

    // Loaders typically return sorted, but enforce — callers rely on
    // ordering for replay timing and for merging multiple streams.
    std::stable_sort(out.begin(), out.end(),
                     [](const MarketEvent& a, const MarketEvent& b) {
                         return event_ts(a) < event_ts(b);
                     });
    return out;
}

}  // namespace MarketData
