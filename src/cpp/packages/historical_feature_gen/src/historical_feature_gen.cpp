#include "historical_feature_gen/historical_feature_gen.h"
#include "historical_feature_gen/merge_iterator.h"
#include "market_data/historical_reader.h"  // FUNDING_POLLING_LATENCY_MS

#include "market_data/state_machine_registry.h"

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/reader.h>
#include <parquet/arrow/writer.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <execution>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include <random>
#include <regex>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace HistoricalFeatureGen {

// ---------------------------------------------------------------------------
// parse_config
// ---------------------------------------------------------------------------

Config parse_config(const std::string& json_path) {
    std::ifstream f(json_path);
    if (!f.is_open()) {
        throw std::runtime_error("Cannot open config file: " + json_path);
    }
    nlohmann::json j;
    f >> j;

    Config cfg;
    cfg.start_ts_ms = j.value("start_ts_ms", 0LL);
    cfg.end_ts_ms = j.value("end_ts_ms", 0LL);
    cfg.emit_interval_ms = j.value("emit_interval_ms", 0LL);
    cfg.emit_interval_max_ms = j.value("emit_interval_max_ms", 0LL);
    cfg.chunk_size_ms = j.value("chunk_size_ms", DEFAULT_CHUNK_SIZE_MS);
    cfg.emit_strategy = j.value("emit_strategy", std::string("wallclock"));
    cfg.emit_tick_type = j.value("emit_tick_type", std::string(""));
    cfg.output_path = j.value("output_path", std::string(""));
    cfg.state_machine = j.at("state_machine").get<std::string>();
    cfg.sm_params = j.value("sm_params", nlohmann::json::object());
    cfg.incremental_buffer_ms = j.value("incremental_buffer_ms", 3600000LL);
    cfg.use_streaming = j.value("use_streaming", true);
    cfg.emit_start_ts_ms = j.value("emit_start_ts_ms", 0LL);
    cfg.output_part_rows = j.value("output_part_rows", 0LL);
    cfg.include_reference_state = j.value("include_reference_state", false);

    for (const auto& ds : j.at("datasets")) {
        DatasetConfig d;
        d.type = ds.at("type").get<std::string>();
        d.path = ds.at("path").get<std::string>();
        d.symbol = ds.at("symbol").get<std::string>();
        d.exchange = ds.at("exchange").get<std::string>();
        d.market_type = ds.at("market_type").get<std::string>();
        d.use_local_timestamp = ds.value("use_local_timestamp", false);
        cfg.datasets.push_back(std::move(d));
    }

    if (j.contains("emit_symbols")) {
        for (const auto& es : j["emit_symbols"]) {
            EmitSymbolConfig esc;
            esc.symbol_idx = es.at("symbol_idx").get<int>();
            esc.symbol_name = es.at("symbol_name").get<std::string>();
            esc.output_path = es.at("output_path").get<std::string>();
            cfg.emit_symbols.push_back(std::move(esc));
        }
    }

    return cfg;
}

// ---------------------------------------------------------------------------
// create_state_machine
// ---------------------------------------------------------------------------

std::unique_ptr<MarketData::StrategyStateMachine> create_state_machine(
    const std::string& name) {
    return MarketData::StateMachineRegistry::create(name);
}


// ---------------------------------------------------------------------------
// load_recording_events
// ---------------------------------------------------------------------------

struct ParquetFileEntry {
    fs::path path;
};

static std::vector<ParquetFileEntry> find_parquet_files(
    const std::string& dir, long long start_ts_ms, long long end_ts_ms) {
    std::vector<ParquetFileEntry> files;
    if (!fs::exists(dir) || !fs::is_directory(dir)) return files;

    std::regex date_regex(R"(year=(\d{4})/month=(\d{2})/day=(\d{2}))");
    constexpr long long ms_per_day = 86400000LL;

    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".parquet")
            continue;
        if (fs::file_size(entry.path()) == 0) continue;

        std::string path_str = entry.path().string();
        std::smatch m;
        if (!std::regex_search(path_str, m, date_regex)) continue;

        std::tm tm{};
        tm.tm_year = std::stoi(m[1]) - 1900;
        tm.tm_mon = std::stoi(m[2]) - 1;
        tm.tm_mday = std::stoi(m[3]);
        long long file_day_ms = static_cast<long long>(timegm(&tm)) * 1000LL;

        if (start_ts_ms > 0 && file_day_ms + ms_per_day - 1 < start_ts_ms)
            continue;
        if (end_ts_ms > 0 && file_day_ms > end_ts_ms) continue;

        files.push_back({entry.path()});
    }
    return files;
}

// Load trade recordings and convert to MarketEvent
static std::vector<MarketData::MarketEvent> load_trade_events(
    const std::string& dir, const std::string& symbol,
    const std::string& exchange, const std::string& market_type,
    long long start_ts_ms, long long end_ts_ms) {
    auto files = find_parquet_files(dir, start_ts_ms, end_ts_ms);
    if (files.empty()) return {};

    arrow::MemoryPool* pool = arrow::default_memory_pool();
    std::vector<std::vector<MarketData::MarketEvent>> per_file(files.size());
    std::vector<std::future<void>> futures;

    for (size_t i = 0; i < files.size(); ++i) {
        futures.emplace_back(std::async(std::launch::async, [&, i]() {
            try {
                auto infile =
                    arrow::io::ReadableFile::Open(files[i].path.string());
                if (!infile.ok()) return;

                auto reader_result =
                    parquet::arrow::OpenFile(infile.ValueOrDie(), pool);
                if (!reader_result.ok()) return;
                auto reader = std::move(reader_result.ValueOrDie());

                std::shared_ptr<arrow::Schema> schema;
                if (!reader->GetSchema(&schema).ok()) return;

                // Tardis trades use microsecond timestamps, amount and aggressor side.
                bool is_recordings = schema->GetFieldIndex("T") >= 0;
                bool is_tardis = schema->GetFieldIndex("amount") >= 0 &&
                                 schema->GetFieldIndex("side") >= 0;
                std::string ts_name = is_recordings ? "T" : "timestamp";
                std::string price_name = is_recordings ? "p" : "price";
                std::string qty_name = is_recordings ? "q" : (is_tardis ? "amount" : "qty");
                std::string buyer_name = is_recordings ? "m" : "is_buyer_maker";

                std::vector<int> col_indices;
                for (const auto& col : {ts_name, price_name, qty_name, buyer_name}) {
                    int idx = schema->GetFieldIndex(col);
                    if (idx >= 0) col_indices.push_back(idx);
                }
                if (is_tardis) col_indices.push_back(schema->GetFieldIndex("side"));

                std::shared_ptr<arrow::Table> tbl;
                if (!reader->ReadTable(col_indices, &tbl).ok() || !tbl ||
                    tbl->num_rows() == 0)
                    return;

                auto combined = tbl->CombineChunks();
                if (!combined.ok()) return;
                tbl = combined.ValueOrDie();

                auto ts_col = std::static_pointer_cast<arrow::Int64Array>(
                    tbl->GetColumnByName(ts_name)->chunk(0));
                auto price_col = std::static_pointer_cast<arrow::DoubleArray>(
                    tbl->GetColumnByName(price_name)->chunk(0));
                auto qty_col = std::static_pointer_cast<arrow::DoubleArray>(
                    tbl->GetColumnByName(qty_name)->chunk(0));
                std::shared_ptr<arrow::BooleanArray> buyer_col;
                std::shared_ptr<arrow::StringArray> side_col;
                if (is_tardis) {
                    side_col = std::static_pointer_cast<arrow::StringArray>(
                        tbl->GetColumnByName("side")->chunk(0));
                } else {
                    buyer_col = std::static_pointer_cast<arrow::BooleanArray>(
                        tbl->GetColumnByName(buyer_name)->chunk(0));
                }

                std::vector<MarketData::MarketEvent> events;
                events.reserve(tbl->num_rows());

                for (int64_t r = 0; r < tbl->num_rows(); ++r) {
                    long long ts = ts_col->Value(r) / (is_tardis ? 1000 : 1);
                    if (start_ts_ms > 0 && ts < start_ts_ms) continue;
                    if (end_ts_ms > 0 && ts > end_ts_ms) continue;

                    MarketData::MarketEvent ev;
                    ev.type = MarketData::MarketEvent::TRADE;
                    ev.trade = MarketData::TradeEvent{
                        symbol, exchange, market_type, price_col->Value(r),
                        qty_col->Value(r),
                        is_tardis ? side_col->GetView(r) == "sell" : buyer_col->Value(r), ts};
                    events.push_back(std::move(ev));
                }
                per_file[i] = std::move(events);

                reader.reset();
                (void)infile.ValueOrDie()->Close();
            } catch (const std::exception& e) {
                std::cerr << "Exception loading trade parquet "
                          << files[i].path << ": " << e.what() << '\n';
            }
        }));
    }
    for (auto& f : futures) f.get();

    size_t total = 0;
    for (const auto& v : per_file) total += v.size();

    std::vector<MarketData::MarketEvent> result;
    result.reserve(total);
    for (auto& v : per_file) {
        result.insert(result.end(), std::make_move_iterator(v.begin()),
                      std::make_move_iterator(v.end()));
    }
    return result;
}

// Load book_ticker recordings and convert to MarketEvent
static std::vector<MarketData::MarketEvent> load_bba_events(
    const std::string& dir, const std::string& symbol,
    const std::string& exchange, const std::string& market_type,
    long long start_ts_ms, long long end_ts_ms) {
    auto files = find_parquet_files(dir, start_ts_ms, end_ts_ms);
    if (files.empty()) return {};

    arrow::MemoryPool* pool = arrow::default_memory_pool();
    std::vector<std::vector<MarketData::MarketEvent>> per_file(files.size());
    std::vector<std::future<void>> futures;

    for (size_t i = 0; i < files.size(); ++i) {
        futures.emplace_back(std::async(std::launch::async, [&, i]() {
            try {
                auto infile =
                    arrow::io::ReadableFile::Open(files[i].path.string());
                if (!infile.ok()) return;

                auto reader_result =
                    parquet::arrow::OpenFile(infile.ValueOrDie(), pool);
                if (!reader_result.ok()) return;
                auto reader = std::move(reader_result.ValueOrDie());

                std::shared_ptr<arrow::Schema> schema;
                if (!reader->GetSchema(&schema).ok()) return;

                // Detect schema: native (lt,b,B,a,A) vs tardis (timestamp,bid_price,bid_amount,ask_price,ask_amount)
                bool is_tardis = schema->GetFieldIndex("bid_price") >= 0;
                std::string ts_name = is_tardis ? "timestamp" : "lt";
                std::string bp_name = is_tardis ? "bid_price" : "b";
                std::string bq_name = is_tardis ? "bid_amount" : "B";
                std::string ap_name = is_tardis ? "ask_price" : "a";
                std::string aq_name = is_tardis ? "ask_amount" : "A";

                std::vector<int> col_indices;
                for (const auto& col : {ts_name, bp_name, bq_name, ap_name, aq_name}) {
                    int idx = schema->GetFieldIndex(col);
                    if (idx >= 0) col_indices.push_back(idx);
                }

                std::shared_ptr<arrow::Table> tbl;
                if (!reader->ReadTable(col_indices, &tbl).ok() || !tbl ||
                    tbl->num_rows() == 0)
                    return;

                auto combined = tbl->CombineChunks();
                if (!combined.ok()) return;
                tbl = combined.ValueOrDie();

                auto ts_col = std::static_pointer_cast<arrow::Int64Array>(
                    tbl->GetColumnByName(ts_name)->chunk(0));
                auto bid_price_col =
                    std::static_pointer_cast<arrow::DoubleArray>(
                        tbl->GetColumnByName(bp_name)->chunk(0));
                auto bid_qty_col =
                    std::static_pointer_cast<arrow::DoubleArray>(
                        tbl->GetColumnByName(bq_name)->chunk(0));
                auto ask_price_col =
                    std::static_pointer_cast<arrow::DoubleArray>(
                        tbl->GetColumnByName(ap_name)->chunk(0));
                auto ask_qty_col =
                    std::static_pointer_cast<arrow::DoubleArray>(
                        tbl->GetColumnByName(aq_name)->chunk(0));

                // Tardis timestamps are in microseconds, convert to ms
                long long ts_divisor = is_tardis ? 1000 : 1;

                std::vector<MarketData::MarketEvent> events;
                events.reserve(tbl->num_rows());

                for (int64_t r = 0; r < tbl->num_rows(); ++r) {
                    long long ts = ts_col->Value(r) / ts_divisor;
                    if (start_ts_ms > 0 && ts < start_ts_ms) continue;
                    if (end_ts_ms > 0 && ts > end_ts_ms) continue;

                    MarketData::MarketEvent ev;
                    ev.type = MarketData::MarketEvent::BBA;
                    ev.bba = MarketData::BBAEvent{
                        symbol, exchange, market_type,
                        bid_price_col->Value(r), bid_qty_col->Value(r),
                        ask_price_col->Value(r), ask_qty_col->Value(r), ts};
                    events.push_back(std::move(ev));
                }
                per_file[i] = std::move(events);

                reader.reset();
                (void)infile.ValueOrDie()->Close();
            } catch (const std::exception& e) {
                std::cerr << "Exception loading book_ticker parquet "
                          << files[i].path << ": " << e.what() << '\n';
            }
        }));
    }
    for (auto& f : futures) f.get();

    size_t total = 0;
    for (const auto& v : per_file) total += v.size();

    std::vector<MarketData::MarketEvent> result;
    result.reserve(total);
    for (auto& v : per_file) {
        result.insert(result.end(), std::make_move_iterator(v.begin()),
                      std::make_move_iterator(v.end()));
    }
    return result;
}

// Load L2 depth update recordings and convert to MarketEvent
// Each parquet row is one price level update (side, price, qty).
// Rows sharing the same (lt, U, u) belong to one depth update message.
static std::vector<MarketData::MarketEvent> load_l2_events(
    const std::string& dir, const std::string& symbol,
    const std::string& exchange, const std::string& market_type,
    long long start_ts_ms, long long end_ts_ms) {
    auto files = find_parquet_files(dir, start_ts_ms, end_ts_ms);
    if (files.empty()) return {};

    arrow::MemoryPool* pool = arrow::default_memory_pool();
    std::vector<std::vector<MarketData::MarketEvent>> per_file(files.size());
    std::vector<std::future<void>> futures;

    for (size_t i = 0; i < files.size(); ++i) {
        futures.emplace_back(std::async(std::launch::async, [&, i]() {
            try {
                auto infile =
                    arrow::io::ReadableFile::Open(files[i].path.string());
                if (!infile.ok()) return;

                auto reader_result =
                    parquet::arrow::OpenFile(infile.ValueOrDie(), pool);
                if (!reader_result.ok()) return;
                auto reader = std::move(reader_result.ValueOrDie());

                std::shared_ptr<arrow::Schema> schema;
                if (!reader->GetSchema(&schema).ok()) return;

                // Detect schema: native (lt,u,U,side,price,qty) vs tardis (timestamp,side,price,amount,is_snapshot)
                bool is_tardis = schema->GetFieldIndex("amount") >= 0 &&
                                 schema->GetFieldIndex("qty") < 0;
                std::string ts_name = is_tardis ? "timestamp" : "lt";
                std::string qty_name = is_tardis ? "amount" : "qty";

                std::vector<std::string> needed_cols = {ts_name, "side", "price", qty_name};
                if (!is_tardis) {
                    needed_cols.push_back("u");
                    needed_cols.push_back("U");
                } else {
                    needed_cols.push_back("is_snapshot");
                }

                std::vector<int> col_indices;
                for (const auto& col : needed_cols) {
                    int idx = schema->GetFieldIndex(col);
                    if (idx >= 0) col_indices.push_back(idx);
                }

                std::shared_ptr<arrow::Table> tbl;
                if (!reader->ReadTable(col_indices, &tbl).ok() || !tbl ||
                    tbl->num_rows() == 0)
                    return;

                auto combined = tbl->CombineChunks();
                if (!combined.ok()) return;
                tbl = combined.ValueOrDie();

                auto ts_col = std::static_pointer_cast<arrow::Int64Array>(
                    tbl->GetColumnByName(ts_name)->chunk(0));
                auto side_chunk = tbl->GetColumnByName("side")->chunk(0);
                bool side_is_large = side_chunk->type_id() == arrow::Type::LARGE_STRING;
                auto side_str_col = side_is_large
                    ? std::shared_ptr<arrow::StringArray>()
                    : std::static_pointer_cast<arrow::StringArray>(side_chunk);
                auto side_lstr_col = side_is_large
                    ? std::static_pointer_cast<arrow::LargeStringArray>(side_chunk)
                    : std::shared_ptr<arrow::LargeStringArray>();
                auto get_side = [&](int64_t row) -> std::string_view {
                    return side_is_large ? side_lstr_col->GetView(row)
                                         : side_str_col->GetView(row);
                };
                auto price_col =
                    std::static_pointer_cast<arrow::DoubleArray>(
                        tbl->GetColumnByName("price")->chunk(0));
                auto qty_col =
                    std::static_pointer_cast<arrow::DoubleArray>(
                        tbl->GetColumnByName(qty_name)->chunk(0));

                // Tardis timestamps are in microseconds
                long long ts_divisor = is_tardis ? 1000 : 1;

                // Native format: group by (lt, U, u)
                // Tardis format: group by timestamp (same microsecond = same update)
                std::shared_ptr<arrow::Int64Array> u_col, U_col;
                std::shared_ptr<arrow::BooleanArray> snapshot_col;
                if (!is_tardis) {
                    u_col = std::static_pointer_cast<arrow::Int64Array>(
                        tbl->GetColumnByName("u")->chunk(0));
                    U_col = std::static_pointer_cast<arrow::Int64Array>(
                        tbl->GetColumnByName("U")->chunk(0));
                } else {
                    snapshot_col = std::static_pointer_cast<arrow::BooleanArray>(
                        tbl->GetColumnByName("is_snapshot")->chunk(0));
                }

                std::vector<MarketData::MarketEvent> events;
                int64_t num_rows = tbl->num_rows();
                int64_t r = 0;

                while (r < num_rows) {
                    long long raw_ts = ts_col->Value(r);
                    long long ts = raw_ts / ts_divisor;
                    bool is_snapshot_row = is_tardis && snapshot_col->Value(r);
                    if (start_ts_ms > 0 && ts < start_ts_ms &&
                        (!is_snapshot_row || ts < start_ts_ms - 60000)) { ++r; continue; }
                    if (end_ts_ms > 0 && ts > end_ts_ms) { ++r; continue; }

                    std::vector<std::pair<double, double>> bids, asks;
                    long long cur_u = 0, cur_U = 0;
                    if (!is_tardis) {
                        cur_u = u_col->Value(r);
                        cur_U = U_col->Value(r);
                    }

                    // Collect all rows belonging to the same update
                    auto same_group = [&](int64_t row) {
                        if (ts_col->Value(row) != raw_ts) return false;
                        if (is_tardis) {
                            // Snapshot rows share the same timestamp;
                            // don't mix snapshot and incremental rows
                            return snapshot_col->Value(row) == is_snapshot_row;
                        }
                        return u_col->Value(row) == cur_u &&
                               U_col->Value(row) == cur_U;
                    };

                    while (r < num_rows && same_group(r)) {
                        double price = price_col->Value(r);
                        double qty = qty_col->Value(r);
                        auto side = get_side(r);
                        if (side == "bid") {
                            bids.emplace_back(price, qty);
                        } else {
                            asks.emplace_back(price, qty);
                        }
                        ++r;
                    }

                    std::string symbol_full =
                        symbol + ":" + exchange + ":" + market_type;

                    MarketData::MarketEvent ev;
                    ev.type = MarketData::MarketEvent::L2_UPDATE;
                    MarketData::L2UpdateEvent l2;
                    l2.sub_type = is_snapshot_row
                        ? MarketData::L2UpdateEvent::FULL_REFRESH
                        : MarketData::L2UpdateEvent::INCREMENTAL;
                    l2.update.bids = std::move(bids);
                    l2.update.asks = std::move(asks);
                    l2.update.symbol_full = symbol_full;
                    l2.update.timestamp = ts;
                    l2.update.sequence_start = cur_U;
                    l2.update.sequence_end = cur_u;
                    l2.exchange_name = exchange;
                    l2.market_type = market_type;
                    l2.symbol = symbol;
                    ev.l2_update = std::move(l2);
                    events.push_back(std::move(ev));
                }
                per_file[i] = std::move(events);

                reader.reset();
                (void)infile.ValueOrDie()->Close();
            } catch (const std::exception& e) {
                std::cerr << "Exception loading lob_l2_update parquet "
                          << files[i].path << ": " << e.what() << '\n';
            }
        }));
    }
    for (auto& f : futures) f.get();

    size_t total = 0;
    for (const auto& v : per_file) total += v.size();

    std::vector<MarketData::MarketEvent> result;
    result.reserve(total);
    for (auto& v : per_file) {
        result.insert(result.end(), std::make_move_iterator(v.begin()),
                      std::make_move_iterator(v.end()));
    }
    return result;
}

// Load funding-rate parquets and convert to MarketEvent::FUNDING.
// Schema: (timestamp i64 ms, funding_rate f64). Applies the simulated
// polling-latency offset (FUNDING_POLLING_LATENCY_MS, defined in
// market_data/historical_reader.h) to each event's emit timestamp so
// SMs see funding events at their observable time, matching what
// will happen in live mode via the fapi poller.
static std::vector<MarketData::MarketEvent> load_funding_events(
    const std::string& dir, const std::string& symbol,
    const std::string& exchange, const std::string& market_type,
    long long start_ts_ms, long long end_ts_ms) {
    using MarketData::FUNDING_POLLING_LATENCY_MS;

    // Pull a slightly wider source window so events whose apply_time
    // is just outside [start, end) but whose emit_time lands inside
    // it (after the +latency shift) are still included.
    const long long src_start_ms = (start_ts_ms > 0)
                                       ? start_ts_ms - FUNDING_POLLING_LATENCY_MS
                                       : 0;
    const long long src_end_ms = (end_ts_ms > 0)
                                     ? end_ts_ms - FUNDING_POLLING_LATENCY_MS
                                     : 0;

    auto files = find_parquet_files(dir, src_start_ms, src_end_ms);
    if (files.empty()) return {};

    arrow::MemoryPool* pool = arrow::default_memory_pool();
    std::vector<std::vector<MarketData::MarketEvent>> per_file(files.size());
    std::vector<std::future<void>> futures;

    for (size_t i = 0; i < files.size(); ++i) {
        futures.emplace_back(std::async(std::launch::async, [&, i]() {
            try {
                auto infile =
                    arrow::io::ReadableFile::Open(files[i].path.string());
                if (!infile.ok()) return;

                auto reader_result =
                    parquet::arrow::OpenFile(infile.ValueOrDie(), pool);
                if (!reader_result.ok()) return;
                auto reader = std::move(reader_result.ValueOrDie());

                std::shared_ptr<arrow::Table> tbl;
                if (!reader->ReadTable(&tbl).ok() || !tbl ||
                    tbl->num_rows() == 0)
                    return;
                auto combined = tbl->CombineChunks();
                if (!combined.ok()) return;
                tbl = combined.ValueOrDie();

                auto ts_col_arr = tbl->GetColumnByName("timestamp");
                auto rate_col_arr = tbl->GetColumnByName("funding_rate");
                if (!ts_col_arr || !rate_col_arr) return;
                auto ts_col = std::static_pointer_cast<arrow::Int64Array>(
                    ts_col_arr->chunk(0));
                auto rate_col = std::static_pointer_cast<arrow::DoubleArray>(
                    rate_col_arr->chunk(0));

                std::vector<MarketData::MarketEvent> events;
                events.reserve(tbl->num_rows());
                for (int64_t r = 0; r < tbl->num_rows(); ++r) {
                    long long apply_ts = ts_col->Value(r);
                    if (src_start_ms > 0 && apply_ts < src_start_ms) continue;
                    if (src_end_ms > 0 && apply_ts > src_end_ms) continue;

                    MarketData::MarketEvent ev;
                    ev.type = MarketData::MarketEvent::FUNDING;
                    ev.funding = MarketData::FundingEvent{
                        symbol, exchange, market_type, rate_col->Value(r),
                        apply_ts + FUNDING_POLLING_LATENCY_MS};
                    events.push_back(std::move(ev));
                }
                per_file[i] = std::move(events);

                reader.reset();
                (void)infile.ValueOrDie()->Close();
            } catch (const std::exception& e) {
                std::cerr << "Exception loading funding parquet "
                          << files[i].path << ": " << e.what() << '\n';
            }
        }));
    }
    for (auto& f : futures) f.get();

    size_t total = 0;
    for (const auto& v : per_file) total += v.size();
    std::vector<MarketData::MarketEvent> result;
    result.reserve(total);
    for (auto& v : per_file) {
        result.insert(result.end(), std::make_move_iterator(v.begin()),
                      std::make_move_iterator(v.end()));
    }
    return result;
}

static long long event_timestamp(const MarketData::MarketEvent& ev) {
    switch (ev.type) {
        case MarketData::MarketEvent::TRADE:
            return ev.trade->timestamp_ms;
        case MarketData::MarketEvent::BBA:
            return ev.bba->timestamp_ms;
        case MarketData::MarketEvent::L2_UPDATE:
            return ev.l2_update->update.timestamp;
        case MarketData::MarketEvent::FUNDING:
            return ev.funding->timestamp_ms;
        default:
            return 0;
    }
}

std::vector<MarketData::MarketEvent> load_events(
    const std::vector<DatasetConfig>& datasets,
    long long start_ts_ms, long long end_ts_ms) {

    // Parallel load: each dataset in its own async task
    std::vector<std::future<std::vector<MarketData::MarketEvent>>> futures;
    futures.reserve(datasets.size());

    for (const auto& dataset : datasets) {
        futures.push_back(std::async(
            std::launch::async,
            [dataset, start_ts_ms, end_ts_ms]()
                -> std::vector<MarketData::MarketEvent> {
                std::vector<MarketData::MarketEvent> events;
                if (dataset.type == "trade") {
                    events = load_trade_events(
                        dataset.path, dataset.symbol, dataset.exchange,
                        dataset.market_type, start_ts_ms, end_ts_ms);
                } else if (dataset.type == "book_ticker") {
                    events = load_bba_events(
                        dataset.path, dataset.symbol, dataset.exchange,
                        dataset.market_type, start_ts_ms, end_ts_ms);
                } else if (dataset.type == "lob_l2_update") {
                    events = load_l2_events(
                        dataset.path, dataset.symbol, dataset.exchange,
                        dataset.market_type, start_ts_ms, end_ts_ms);
                } else if (dataset.type == "funding") {
                    events = load_funding_events(
                        dataset.path, dataset.symbol, dataset.exchange,
                        dataset.market_type, start_ts_ms, end_ts_ms);
                }
                return events;
            }));
    }

    // Collect results
    std::vector<MarketData::MarketEvent> merged;
    for (size_t i = 0; i < futures.size(); ++i) {
        auto events = futures[i].get();
        std::cout << "Loaded " << events.size() << " events from "
                  << datasets[i].type << " (" << datasets[i].symbol << " @ "
                  << datasets[i].path << ")" << std::endl;
        merged.insert(merged.end(), std::make_move_iterator(events.begin()),
                      std::make_move_iterator(events.end()));
    }

    // Parallel sort by timestamp
    std::sort(std::execution::par, merged.begin(), merged.end(),
              [](const MarketData::MarketEvent& a,
                 const MarketData::MarketEvent& b) {
                  return event_timestamp(a) < event_timestamp(b);
              });

    return merged;
}

// ---------------------------------------------------------------------------
// write_feature_parquets
// ---------------------------------------------------------------------------

struct FeatureRow {
    long long timestamp_ms;
    std::vector<float> features;
    double reference_mid = 0.0;
    long long reference_quote_timestamp_ms = 0;
    long long event_sequence = 0;
};

static int write_feature_parquets(
    const std::string& output_path,
    const std::vector<std::string>& feature_names,
    const std::vector<FeatureRow>& rows,
    bool include_reference_state = false) {
    if (rows.empty()) return 0;

    // Bucket by day
    std::unordered_map<std::string, std::vector<const FeatureRow*>> bucket;
    char buf[128];

    for (const auto& row : rows) {
        std::time_t seconds = static_cast<time_t>(row.timestamp_ms / 1000);
        std::tm tm{};
        gmtime_r(&seconds, &tm);
        std::strftime(buf, sizeof(buf),
                      "year=%Y/month=%m/day=%d/%Y-%m-%dT00-00.parquet", &tm);
        bucket[buf].push_back(&row);
    }

    int files_written = 0;
    for (auto& [key, vec] : bucket) {
        // Sort by timestamp within day
        std::stable_sort(vec.begin(), vec.end(),
                  [](const FeatureRow* a, const FeatureRow* b) {
                      return a->timestamp_ms < b->timestamp_ms;
                  });

        // Build Arrow arrays
        arrow::Int64Builder ts_builder;
        arrow::DoubleBuilder mid_builder;
        arrow::Int64Builder quote_ts_builder, sequence_builder;
        std::vector<arrow::DoubleBuilder> feature_builders(feature_names.size());

        for (const auto* row : vec) {
            if (!ts_builder.Append(row->timestamp_ms).ok()) return -1;
            if (include_reference_state &&
                (!mid_builder.Append(row->reference_mid).ok() ||
                 !quote_ts_builder.Append(row->reference_quote_timestamp_ms).ok() ||
                 !sequence_builder.Append(row->event_sequence).ok())) return -1;
            for (size_t f = 0; f < feature_names.size(); ++f) {
                float val = f < row->features.size() ? row->features[f] : 0.0f;
                if (!feature_builders[f].Append(static_cast<double>(val)).ok())
                    return -1;
            }
        }

        // Finish arrays
        std::shared_ptr<arrow::Array> ts_array;
        if (!ts_builder.Finish(&ts_array).ok()) return -1;

        std::vector<std::shared_ptr<arrow::Array>> feature_arrays(
            feature_names.size());
        for (size_t f = 0; f < feature_names.size(); ++f) {
            if (!feature_builders[f].Finish(&feature_arrays[f]).ok()) return -1;
        }

        // Build schema: timestamp_ms (Int64) + feature columns (Float64)
        std::vector<std::shared_ptr<arrow::Field>> fields;
        fields.push_back(arrow::field("timestamp_ms", arrow::int64()));
        for (const auto& name : feature_names) {
            fields.push_back(arrow::field(name, arrow::float64()));
        }
        auto schema = arrow::schema(fields);

        // Build column vector
        std::vector<std::shared_ptr<arrow::Array>> columns;
        columns.push_back(ts_array);
        for (auto& arr : feature_arrays) columns.push_back(arr);

        if (include_reference_state) {
            std::shared_ptr<arrow::Array> mid, quote_ts, sequence;
            if (!mid_builder.Finish(&mid).ok() || !quote_ts_builder.Finish(&quote_ts).ok() ||
                !sequence_builder.Finish(&sequence).ok()) return -1;
            fields.push_back(arrow::field("reference_mid", arrow::float64()));
            fields.push_back(arrow::field("reference_quote_timestamp_ms", arrow::int64()));
            fields.push_back(arrow::field("event_sequence", arrow::int64()));
            columns.insert(columns.end(), {mid, quote_ts, sequence});
            schema = arrow::schema(fields);
        }
        auto table = arrow::Table::Make(schema, columns);

        // Write parquet file
        fs::path parquet_path = fs::path(output_path) / key;
        fs::path tmp_path = parquet_path.string() + ".tmp";
        fs::create_directories(parquet_path.parent_path());

        auto out_result =
            arrow::io::FileOutputStream::Open(tmp_path.string());
        if (!out_result.ok()) {
            std::cerr << "Failed to open output file: " << tmp_path << '\n';
            return -1;
        }
        auto out = out_result.ValueOrDie();

        auto writer_props = parquet::WriterProperties::Builder()
                                .compression(parquet::Compression::SNAPPY)
                                ->build();

        auto write_status = parquet::arrow::WriteTable(
            *table, arrow::default_memory_pool(), out, 65536, writer_props);
        if (!write_status.ok()) {
            std::cerr << "Failed to write parquet: " << write_status.ToString()
                      << '\n';
            return -1;
        }

        auto close_status = out->Close();
        if (!close_status.ok()) {
            std::cerr << "Failed to close output file: "
                      << close_status.ToString() << '\n';
            return -1;
        }

        std::error_code ec;
        fs::rename(tmp_path, parquet_path, ec);
        if (ec) {
            std::cerr << "Failed to rename tmp to final: " << ec.message()
                      << '\n';
            std::error_code dummy;
            fs::remove(tmp_path, dummy);
            return -1;
        }

        files_written++;
    }

    return files_written;
}

// ---------------------------------------------------------------------------
// discover_data_time_range — scan parquet partition paths for min/max day
// ---------------------------------------------------------------------------

static std::pair<long long, long long> discover_data_time_range(
    const std::vector<DatasetConfig>& datasets) {
    long long min_ms = std::numeric_limits<long long>::max();
    long long max_ms = std::numeric_limits<long long>::min();
    constexpr long long ms_per_day = 86400000LL;

    std::regex date_regex(R"(year=(\d{4})/month=(\d{2})/day=(\d{2}))");

    for (const auto& dataset : datasets) {
        if (!fs::exists(dataset.path) || !fs::is_directory(dataset.path))
            continue;

        for (const auto& entry :
             fs::recursive_directory_iterator(dataset.path)) {
            if (!entry.is_regular_file() ||
                entry.path().extension() != ".parquet")
                continue;

            std::string path_str = entry.path().string();
            std::smatch m;
            if (!std::regex_search(path_str, m, date_regex)) continue;

            std::tm tm{};
            tm.tm_year = std::stoi(m[1]) - 1900;
            tm.tm_mon = std::stoi(m[2]) - 1;
            tm.tm_mday = std::stoi(m[3]);
            long long day_ms =
                static_cast<long long>(timegm(&tm)) * 1000LL;

            if (day_ms < min_ms) min_ms = day_ms;
            if (day_ms + ms_per_day > max_ms) max_ms = day_ms + ms_per_day;
        }
    }

    if (min_ms > max_ms) return {0, 0};
    return {min_ms, max_ms};
}

// ---------------------------------------------------------------------------
// run
// ---------------------------------------------------------------------------

static void replay_events(
    const std::vector<MarketData::MarketEvent>& events,
    const Config& config,
    MarketData::StrategyStateMachine& sm,
    std::vector<FeatureRow>& output_rows,
    std::unordered_map<int, std::vector<FeatureRow>>& per_symbol_rows,
    long long& last_emit_ts,
    long long& next_emit_threshold,
    std::mt19937& rng) {

    bool use_random_interval = config.emit_interval_max_ms > config.emit_interval_ms;
    auto roll_threshold = [&]() -> long long {
        if (!use_random_interval) return config.emit_interval_ms;
        std::uniform_int_distribution<long long> dist(
            config.emit_interval_ms, config.emit_interval_max_ms);
        return dist(rng);
    };

    if (next_emit_threshold <= 0)
        next_emit_threshold = roll_threshold();

    bool per_symbol = !config.emit_symbols.empty();

    for (const auto& ev : events) {
        sm.react(ev, true);

        long long ts = event_timestamp(ev);

        bool should_emit = false;
        if (config.emit_strategy == "tick") {
            bool type_matches = false;
            if (config.emit_tick_type == "trade" &&
                ev.type == MarketData::MarketEvent::TRADE) {
                type_matches = true;
            } else if (config.emit_tick_type == "bba" &&
                       ev.type == MarketData::MarketEvent::BBA) {
                type_matches = true;
            } else if (config.emit_tick_type == "l2" &&
                       ev.type == MarketData::MarketEvent::L2_UPDATE) {
                type_matches = true;
            }

            if (type_matches) {
                if (config.emit_interval_ms <= 0) {
                    should_emit = true;
                } else {
                    should_emit =
                        (ts - last_emit_ts >= next_emit_threshold) ||
                        (last_emit_ts == 0);
                }
            }
        } else {
            if (config.emit_interval_ms <= 0) {
                should_emit = true;
            } else {
                long long boundary =
                    (ts / next_emit_threshold) * next_emit_threshold;
                long long last_boundary =
                    (last_emit_ts / next_emit_threshold) *
                    next_emit_threshold;
                should_emit =
                    (boundary > last_boundary) || (last_emit_ts == 0);
            }
        }

        if (should_emit && ts >= config.emit_start_ts_ms) {
            if (per_symbol) {
                for (const auto& es : config.emit_symbols) {
                    auto f = sm.emit_state_for_symbol(es.symbol_idx);
                    if (f.has_value()) {
                        FeatureRow row;
                        row.timestamp_ms = ts;
                        row.features = std::move(*f);
                        per_symbol_rows[es.symbol_idx].push_back(
                            std::move(row));
                    }
                }
            } else {
                auto features_opt = sm.emit_state();
                if (!features_opt.has_value()) {
                    last_emit_ts = ts;
                    continue;
                }
                FeatureRow row;
                row.timestamp_ms = ts;
                row.features = std::move(*features_opt);
                output_rows.push_back(std::move(row));
            }
            last_emit_ts = ts;
            next_emit_threshold = roll_threshold();
        }
    }
}

static void replay_events_streaming(
    MergeIterator& iter,
    const Config& config,
    MarketData::StrategyStateMachine& sm,
    std::vector<FeatureRow>& output_rows,
    std::unordered_map<int, std::vector<FeatureRow>>& per_symbol_rows,
    long long& last_emit_ts,
    long long& next_emit_threshold,
    std::mt19937& rng,
    const std::function<void()>& flush_output = {}) {

    bool use_random_interval = config.emit_interval_max_ms > config.emit_interval_ms;
    auto roll_threshold = [&]() -> long long {
        if (!use_random_interval) return config.emit_interval_ms;
        std::uniform_int_distribution<long long> dist(
            config.emit_interval_ms, config.emit_interval_max_ms);
        return dist(rng);
    };

    if (next_emit_threshold <= 0)
        next_emit_threshold = roll_threshold();

    bool per_symbol = !config.emit_symbols.empty();
    if (config.include_reference_state && !sm.reference_state())
        throw std::invalid_argument(
            "include_reference_state: " + sm.get_name() +
            " exposes no reference_state()");

    // Throughput reporting
    auto throughput_start = std::chrono::steady_clock::now();
    long long throughput_events = 0;
    constexpr long long THROUGHPUT_REPORT_INTERVAL = 1000000;

    while (iter.has_next()) {
        auto ev = iter.next();
        sm.react(ev, true);

        throughput_events++;
        if (throughput_events >= THROUGHPUT_REPORT_INTERVAL) {
            auto now = std::chrono::steady_clock::now();
            double secs = std::chrono::duration<double>(
                              now - throughput_start).count();
            double rate = throughput_events / secs / 1e6;
            std::cerr << "[throughput] " << throughput_events
                      << " events in " << std::fixed << std::setprecision(2)
                      << secs << "s = " << rate << "M ev/s" << std::endl;
            throughput_start = now;
            throughput_events = 0;
        }

        long long ts = event_timestamp(ev);

        bool should_emit = false;
        if (config.emit_strategy == "tick") {
            bool type_matches = false;
            if (config.emit_tick_type == "trade" &&
                ev.type == MarketData::MarketEvent::TRADE) {
                type_matches = true;
            } else if (config.emit_tick_type == "bba" &&
                       ev.type == MarketData::MarketEvent::BBA) {
                type_matches = true;
            } else if (config.emit_tick_type == "l2" &&
                       ev.type == MarketData::MarketEvent::L2_UPDATE) {
                type_matches = true;
            }

            if (type_matches) {
                if (config.emit_interval_ms <= 0) {
                    should_emit = true;
                } else {
                    should_emit =
                        (ts - last_emit_ts >= next_emit_threshold) ||
                        (last_emit_ts == 0);
                }
            }
        } else {
            if (config.emit_interval_ms <= 0) {
                should_emit = true;
            } else {
                long long boundary =
                    (ts / next_emit_threshold) * next_emit_threshold;
                long long last_boundary =
                    (last_emit_ts / next_emit_threshold) *
                    next_emit_threshold;
                should_emit =
                    (boundary > last_boundary) || (last_emit_ts == 0);
            }
        }

        if (should_emit && ts >= config.emit_start_ts_ms) {
            if (per_symbol) {
                for (const auto& es : config.emit_symbols) {
                    auto f = sm.emit_state_for_symbol(es.symbol_idx);
                    if (f.has_value()) {
                        FeatureRow row;
                        row.timestamp_ms = ts;
                        row.features = std::move(*f);
                        per_symbol_rows[es.symbol_idx].push_back(
                            std::move(row));
                    }
                }
            } else {
                auto features_opt = sm.emit_state();
                if (!features_opt.has_value()) {
                    last_emit_ts = ts;
                    continue;
                }
                FeatureRow row;
                row.timestamp_ms = ts;
                row.features = std::move(*features_opt);
                if (config.include_reference_state) {
                    const auto state = *sm.reference_state();
                    row.reference_mid = state.mid;
                    row.reference_quote_timestamp_ms = state.timestamp_ms;
                    row.event_sequence = iter.total_events_consumed();
                }
                output_rows.push_back(std::move(row));
                if (flush_output && config.output_part_rows > 0 &&
                    output_rows.size() >= static_cast<size_t>(config.output_part_rows))
                    flush_output();
            }
            last_emit_ts = ts;
            next_emit_threshold = roll_threshold();
        }
    }
}

// Write per-symbol parquet outputs. Returns 0 on success, 1 on error.
static int write_per_symbol_parquets(
    const Config& config,
    MarketData::StrategyStateMachine& sm,
    std::unordered_map<int, std::vector<FeatureRow>>& per_symbol_rows) {
    for (const auto& es : config.emit_symbols) {
        auto& rows = per_symbol_rows[es.symbol_idx];
        if (rows.empty()) continue;
        auto names = sm.feature_names_for_symbol(es.symbol_idx);
        int result = write_feature_parquets(es.output_path, names, rows);
        if (result < 0) {
            std::cerr << "Failed to write parquets for "
                      << es.symbol_name << std::endl;
            return 1;
        }
        std::cout << "Wrote " << result << " parquet files for "
                  << es.symbol_name << " to " << es.output_path
                  << " (" << rows.size() << " rows)" << std::endl;
    }
    return 0;
}

// The SM decides when a row exists (emit_strategy "sm_rows"): rows go to
// output_path/<symbol>/year=/month=/day= files, each day written once it is over.
static int run_sm_rows(const Config& config,
                       MarketData::StrategyStateMachine& sm,
                       long long range_start, long long range_end) {
    const auto names = sm.feature_names();
    std::unordered_map<std::string, std::vector<FeatureRow>> pending;
    long long written = 0;
    auto write_through = [&](long long day_end_ms) {
        for (auto& [symbol, rows] : pending) {
            auto split = std::stable_partition(rows.begin(), rows.end(), [&](const FeatureRow& r) {
                return r.timestamp_ms < day_end_ms;
            });
            if (split == rows.begin()) continue;
            std::vector<FeatureRow> done(std::make_move_iterator(rows.begin()),
                                         std::make_move_iterator(split));
            rows.erase(rows.begin(), split);
            if (write_feature_parquets(config.output_path + "/" + symbol, names, done) < 0)
                throw std::runtime_error("Failed to write rows for " + symbol);
            written += static_cast<long long>(done.size());
        }
    };
    auto drain = [&]() {
        for (auto& e : sm.drain_rows()) {
            pending[e.symbol].push_back(
                FeatureRow{e.timestamp_ms, std::move(e.features)});
        }
    };
    constexpr long long kDayMs = 86'400'000;
    MergeIterator iter(config.datasets, range_start, range_end);
    long long open_day = -1;
    while (iter.has_next()) {
        auto ev = iter.next();
        sm.react(ev, true);
        drain();
        const long long day = event_timestamp(ev) / kDayMs;
        // A day's last rows are emitted up to a flush grace after midnight.
        if (open_day >= 0 && day > open_day && event_timestamp(ev) % kDayMs >= 60'000) {
            write_through(day * kDayMs);
            open_day = day;
        } else if (open_day < 0) {
            open_day = day;
        }
    }
    drain();
    write_through(LLONG_MAX);
    std::cout << "Wrote " << written << " rows to " << config.output_path
              << " (late events dropped: " << sm.late_events() << ")" << std::endl;
    return 0;
}

int run(const Config& config, MarketData::StrategyStateMachine& sm) {
    if (config.output_part_rows < 0 ||
        ((config.output_part_rows > 0 || config.include_reference_state) &&
         (!config.use_streaming || config.chunk_size_ms != 0 || !config.emit_symbols.empty())))
        throw std::invalid_argument("Bounded/audited output requires unchunked single-output streaming");
    if (!config.use_streaming) {
        for (const auto& dataset : config.datasets) {
            if (dataset.use_local_timestamp)
                throw std::runtime_error("local_timestamp requires streaming replay");
        }
    }
    long long range_start = config.start_ts_ms;
    long long range_end = config.end_ts_ms;

    if (range_start == 0 || range_end == 0) {
        auto [discovered_start, discovered_end] =
            discover_data_time_range(config.datasets);
        if (discovered_start == 0 && discovered_end == 0) {
            std::cerr << "No parquet data found" << std::endl;
            return 1;
        }
        if (range_start == 0) range_start = discovered_start;
        if (range_end == 0) range_end = discovered_end;
    }

    // Resume from checkpoint if state was loaded — start at next chunk boundary
    if (sm.has_loaded_state()) {
        long long resume_ts = sm.get_min_synced_ts();
        if (resume_ts > 0 && config.chunk_size_ms > 0) {
            // Next chunk boundary after the watermark — avoids overwriting
            // the last fully-written day with a partial file
            long long resume_start =
                ((resume_ts / config.chunk_size_ms) + 1) * config.chunk_size_ms;
            if (resume_start > range_start) {
                std::cout << "Incremental resume from ts=" << resume_start
                          << " (watermark=" << resume_ts << ")" << std::endl;
                range_start = resume_start;
            }
        }
    }

    if (config.emit_strategy == "sm_rows") {
        return run_sm_rows(config, sm, range_start, range_end);
    }

    // --- Streaming path (merge iterator) ---
    if (config.use_streaming) {
        bool chunked = config.chunk_size_ms > 0 &&
                       (range_end - range_start) > config.chunk_size_ms;

        auto ms_to_date = [](long long ms) -> std::string {
            std::time_t sec = static_cast<std::time_t>(ms / 1000);
            std::tm tm{};
            gmtime_r(&sec, &tm);
            char buf[16];
            std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
            return buf;
        };

        auto format_duration = [](double seconds) -> std::string {
            if (seconds < 60) return std::to_string(static_cast<int>(seconds)) + "s";
            int mins = static_cast<int>(seconds) / 60;
            int secs = static_cast<int>(seconds) % 60;
            if (mins < 60) return std::to_string(mins) + "m " + std::to_string(secs) + "s";
            int hours = mins / 60;
            mins %= 60;
            return std::to_string(hours) + "h " + std::to_string(mins) + "m";
        };

        if (!chunked) {
            std::cout << "Streaming merge replay [" << ms_to_date(range_start)
                      << " to " << ms_to_date(range_end) << "]..." << std::endl;

            auto t0 = std::chrono::steady_clock::now();
            MergeIterator iter(config.datasets, range_start, range_end);

            std::vector<FeatureRow> output_rows;
            std::unordered_map<int, std::vector<FeatureRow>> per_symbol_rows;
            long long last_emit_ts = 0;
            long long next_emit_threshold = 0;
            std::mt19937 rng(42);

            int files_written = 0;
            long long part_number = 0, rows_written = 0;
            auto flush_output = [&]() {
                if (output_rows.empty()) return;
                std::string part_path = config.output_path;
                if (config.output_part_rows > 0)
                    part_path += "/part=" + std::to_string(part_number++);
                int result = write_feature_parquets(part_path, sm.feature_names(), output_rows,
                                                   config.include_reference_state);
                if (result < 0) throw std::runtime_error("Failed to write feature part");
                files_written += result;
                rows_written += output_rows.size();
                output_rows.clear();
                if (config.output_part_rows > 0)
                    std::cout << "[Part " << part_number << "] " << rows_written
                              << " rows written" << std::endl;
            };

            replay_events_streaming(iter, config, sm, output_rows,
                                    per_symbol_rows,
                                    last_emit_ts, next_emit_threshold, rng, flush_output);

            auto t1 = std::chrono::steady_clock::now();
            double secs = std::chrono::duration<double>(t1 - t0).count();
            std::cout << "Replay done: "
                      << iter.total_events_consumed() << " events ("
                      << format_duration(secs) << ", "
                      << std::fixed << std::setprecision(1)
                      << (iter.total_events_consumed() / secs / 1000000.0) << "M ev/s)"
                      << std::endl;

            if (!config.emit_symbols.empty()) {
                return write_per_symbol_parquets(config, sm, per_symbol_rows);
            }

            flush_output();
            std::cout << "Wrote " << files_written << " parquet files (" << rows_written
                      << " rows) to " << config.output_path << std::endl;
            return 0;
        }

        // Chunked streaming
        long long total_chunks =
            (range_end - range_start + config.chunk_size_ms - 1) / config.chunk_size_ms;
        std::cout << "Streaming merge, " << total_chunks << " chunks across ["
                  << range_start << ", " << range_end << "]" << std::endl;

        auto run_start = std::chrono::steady_clock::now();
        long long chunk_start = range_start;
        long long last_emit_ts = 0;
        long long next_emit_threshold = 0;
        std::mt19937 rng(42);
        int total_files_written = 0;
        long long total_rows_emitted = 0;
        long long total_events_processed = 0;
        int chunk_num = 0;

        while (chunk_start < range_end) {
            long long chunk_end = std::min(chunk_start + config.chunk_size_ms, range_end);
            chunk_num++;

            auto chunk_clock_start = std::chrono::steady_clock::now();
            MergeIterator iter(config.datasets, chunk_start, chunk_end);

            std::vector<FeatureRow> output_rows;
            std::unordered_map<int, std::vector<FeatureRow>> per_symbol_rows;
            replay_events_streaming(iter, config, sm, output_rows,
                                    per_symbol_rows,
                                    last_emit_ts, next_emit_threshold, rng);

            long long chunk_events = static_cast<long long>(iter.total_events_consumed());
            long long chunk_rows = 0;

            if (!config.emit_symbols.empty()) {
                for (auto& [idx, rows] : per_symbol_rows) {
                    chunk_rows += static_cast<long long>(rows.size());
                }
                if (chunk_rows > 0) {
                    int r = write_per_symbol_parquets(config, sm, per_symbol_rows);
                    if (r != 0) return 1;
                    total_rows_emitted += chunk_rows;
                }
            } else {
                chunk_rows = static_cast<long long>(output_rows.size());
                if (!output_rows.empty()) {
                    auto names = sm.feature_names();
                    int result = write_feature_parquets(config.output_path, names, output_rows);
                    if (result < 0) {
                        std::cerr << "Failed to write output parquets" << std::endl;
                        return 1;
                    }
                    total_files_written += result;
                    total_rows_emitted += chunk_rows;
                }
            }

            total_events_processed += chunk_events;

            auto chunk_clock_end = std::chrono::steady_clock::now();
            double chunk_secs = std::chrono::duration<double>(chunk_clock_end - chunk_clock_start).count();
            double elapsed_secs = std::chrono::duration<double>(chunk_clock_end - run_start).count();
            double avg_secs_per_chunk = elapsed_secs / chunk_num;
            double eta_secs = avg_secs_per_chunk * (total_chunks - chunk_num);
            double events_per_sec = chunk_secs > 0 ? chunk_events / chunk_secs : 0;

            std::cout << "[Chunk " << chunk_num << "/" << total_chunks << "] "
                      << ms_to_date(chunk_start) << " | "
                      << std::fixed << std::setprecision(1)
                      << (chunk_events / 1000000.0) << "M events, "
                      << (chunk_rows / 1000.0) << "K rows | "
                      << chunk_secs << "s ("
                      << (events_per_sec / 1000000.0) << "M ev/s) | "
                      << "Total: " << (total_events_processed / 1000000.0) << "M events, "
                      << (total_rows_emitted / 1000.0) << "K rows | "
                      << "Elapsed: " << format_duration(elapsed_secs) << " | "
                      << "ETA: " << format_duration(eta_secs)
                      << std::endl;

            sm.init_dependencies();
            sm.serialize_state();
            chunk_start = chunk_end;
        }

        double total_secs = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - run_start).count();
        std::cout << "Done: " << total_rows_emitted << " rows, "
                  << total_files_written << " files, "
                  << (total_events_processed / 1000000.0) << "M events in "
                  << format_duration(total_secs) << std::endl;
        return 0;
    }

    // --- Legacy path (load-all-sort) ---
    bool chunked = config.chunk_size_ms > 0 &&
                   (range_end - range_start) > config.chunk_size_ms;

    if (!chunked) {
        auto t0 = std::chrono::steady_clock::now();
        std::cout << "Loading all events at once..." << std::endl;
        auto events =
            load_events(config.datasets, range_start, range_end);

        if (events.empty()) {
            std::cerr << "No events found" << std::endl;
            return 1;
        }

        auto t1 = std::chrono::steady_clock::now();
        double load_secs = std::chrono::duration<double>(t1 - t0).count();
        std::cout << "Loaded " << events.size() << " events in "
                  << load_secs << "s" << std::endl;

        std::cout << "Replaying " << events.size() << " events..."
                  << std::endl;

        std::vector<FeatureRow> output_rows;
        std::unordered_map<int, std::vector<FeatureRow>> per_symbol_rows;
        output_rows.reserve(events.size());
        long long last_emit_ts = 0;
        long long next_emit_threshold = 0;
        std::mt19937 rng(42);

        replay_events(events, config, sm, output_rows, per_symbol_rows,
                      last_emit_ts, next_emit_threshold, rng);

        auto t2 = std::chrono::steady_clock::now();
        double replay_secs = std::chrono::duration<double>(t2 - t1).count();
        std::cout << "Replay done: " << events.size() << " events in "
                  << replay_secs << "s" << std::endl;

        if (!config.emit_symbols.empty()) {
            return write_per_symbol_parquets(config, sm, per_symbol_rows);
        }

        auto names = sm.feature_names();
        int result =
            write_feature_parquets(config.output_path, names, output_rows);
        if (result < 0) {
            std::cerr << "Failed to write output parquets" << std::endl;
            return 1;
        }
        std::cout << "Wrote " << result << " parquet files to "
                  << config.output_path << std::endl;
        return 0;
    }

    // Legacy chunked processing
    long long total_chunks =
        (range_end - range_start + config.chunk_size_ms - 1) /
        config.chunk_size_ms;
    std::cout << "Processing " << total_chunks << " chunks across range ["
              << range_start << ", " << range_end << "]" << std::endl;

    auto run_start = std::chrono::steady_clock::now();
    long long chunk_start = range_start;
    long long last_emit_ts = 0;
    long long next_emit_threshold = 0;
    std::mt19937 rng(42);
    int total_files_written = 0;
    long long total_rows_emitted = 0;
    long long total_events_processed = 0;
    int chunk_num = 0;

    auto ms_to_date = [](long long ms) -> std::string {
        std::time_t sec = static_cast<std::time_t>(ms / 1000);
        std::tm tm{};
        gmtime_r(&sec, &tm);
        char buf[16];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
        return buf;
    };

    auto format_duration = [](double seconds) -> std::string {
        if (seconds < 60) return std::to_string(static_cast<int>(seconds)) + "s";
        int mins = static_cast<int>(seconds) / 60;
        int secs = static_cast<int>(seconds) % 60;
        if (mins < 60) return std::to_string(mins) + "m " + std::to_string(secs) + "s";
        int hours = mins / 60;
        mins %= 60;
        return std::to_string(hours) + "h " + std::to_string(mins) + "m";
    };

    while (chunk_start < range_end) {
        long long chunk_end =
            std::min(chunk_start + config.chunk_size_ms, range_end);
        chunk_num++;

        auto chunk_clock_start = std::chrono::steady_clock::now();

        auto events =
            load_events(config.datasets, chunk_start, chunk_end);

        long long chunk_rows = 0;
        if (!events.empty()) {
            std::vector<FeatureRow> output_rows;
            std::unordered_map<int, std::vector<FeatureRow>> per_symbol_rows;
            output_rows.reserve(events.size());

            replay_events(events, config, sm, output_rows, per_symbol_rows,
                          last_emit_ts, next_emit_threshold, rng);

            if (!config.emit_symbols.empty()) {
                for (auto& [idx, rows] : per_symbol_rows) {
                    chunk_rows += static_cast<long long>(rows.size());
                }
                if (chunk_rows > 0) {
                    int r = write_per_symbol_parquets(config, sm,
                                                      per_symbol_rows);
                    if (r != 0) return 1;
                    total_rows_emitted += chunk_rows;
                }
            } else {
                chunk_rows = static_cast<long long>(output_rows.size());
                if (!output_rows.empty()) {
                    auto names = sm.feature_names();
                    int result = write_feature_parquets(
                        config.output_path, names, output_rows);
                    if (result < 0) {
                        std::cerr << "Failed to write output parquets"
                                  << std::endl;
                        return 1;
                    }
                    total_files_written += result;
                    total_rows_emitted += chunk_rows;
                }
            }
        }

        total_events_processed +=
            static_cast<long long>(events.size());

        auto chunk_clock_end = std::chrono::steady_clock::now();
        double chunk_secs =
            std::chrono::duration<double>(chunk_clock_end - chunk_clock_start)
                .count();
        double elapsed_secs =
            std::chrono::duration<double>(chunk_clock_end - run_start).count();
        double avg_secs_per_chunk = elapsed_secs / chunk_num;
        double eta_secs = avg_secs_per_chunk * (total_chunks - chunk_num);
        double events_per_sec =
            chunk_secs > 0 ? events.size() / chunk_secs : 0;

        std::cout << "[Chunk " << chunk_num << "/" << total_chunks << "] "
                  << ms_to_date(chunk_start) << " | "
                  << std::fixed << std::setprecision(1)
                  << (events.size() / 1000000.0) << "M events, "
                  << (chunk_rows / 1000.0) << "K rows | "
                  << chunk_secs << "s ("
                  << (events_per_sec / 1000000.0) << "M ev/s) | "
                  << "Total: " << (total_events_processed / 1000000.0) << "M events, "
                  << (total_rows_emitted / 1000.0) << "K rows | "
                  << "Elapsed: " << format_duration(elapsed_secs) << " | "
                  << "ETA: " << format_duration(eta_secs)
                  << std::endl;

        sm.init_dependencies();
        sm.serialize_state();

        chunk_start = chunk_end;
    }

    double total_secs =
        std::chrono::duration<double>(
            std::chrono::steady_clock::now() - run_start).count();
    std::cout << "Done: " << total_rows_emitted << " rows, "
              << total_files_written << " files, "
              << (total_events_processed / 1000000.0) << "M events in "
              << format_duration(total_secs) << std::endl;
    return 0;
}

}  // namespace HistoricalFeatureGen
