#include "historical_feature_gen/merge_iterator.h"

#include <arrow/compute/api.h>
#include <numeric>

#include <algorithm>
#include <iostream>
#include <regex>
#include <stdexcept>

#include "market_data/historical_reader.h"  // FUNDING_POLLING_LATENCY_MS

namespace HistoricalFeatureGen {

// ---------------------------------------------------------------------------
// event_timestamp helper (same as in historical_feature_gen.cpp)
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// find_parquet_files_sorted
// ---------------------------------------------------------------------------

std::vector<fs::path> find_parquet_files_sorted(
    const std::string& dir, long long start_ts_ms, long long end_ts_ms) {
    std::vector<fs::path> files;
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

        files.push_back(entry.path());
    }

    // Sort by path string to ensure chronological order
    std::sort(files.begin(), files.end());
    return files;
}

// ---------------------------------------------------------------------------
// convert_table_to_events — dispatches by dataset type
// ---------------------------------------------------------------------------

static std::vector<MarketData::MarketEvent> convert_trade_table(
    const std::shared_ptr<arrow::Table>& tbl,
    const DatasetConfig& config,
    long long start_ts_ms, long long end_ts_ms) {

    auto schema = tbl->schema();
    bool is_recordings = schema->GetFieldIndex("T") >= 0;
    bool is_tardis = schema->GetFieldIndex("amount") >= 0 &&
                     schema->GetFieldIndex("side") >= 0;
    std::string ts_name = is_recordings ? "T" :
        (is_tardis && config.use_local_timestamp ? "local_timestamp" : "timestamp");
    std::string price_name = is_recordings ? "p" : "price";
    std::string qty_name = is_recordings ? "q" : (is_tardis ? "amount" : "qty");
    std::string buyer_name = is_recordings ? "m" : "is_buyer_maker";

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

    // Some archive files are sorted by timestamp only; within a millisecond the
    // exchange's (and the live stream's) order is the trade id's.
    std::vector<int64_t> ids;
    if (!is_recordings && !is_tardis && tbl->GetColumnByName("id")) {
        auto cast = arrow::compute::Cast(arrow::Datum(tbl->GetColumnByName("id")->chunk(0)), arrow::int64());
        if (!cast.ok()) throw std::runtime_error("trade ids: " + cast.status().ToString());
        auto id_col = std::static_pointer_cast<arrow::Int64Array>(cast.ValueOrDie().make_array());
        ids.assign(id_col->raw_values(), id_col->raw_values() + id_col->length());
    }
    std::vector<int64_t> rows(tbl->num_rows());
    std::iota(rows.begin(), rows.end(), 0);
    if (!ids.empty())
        std::stable_sort(rows.begin(), rows.end(), [&](int64_t a, int64_t b) {
            const long long ta = ts_col->Value(a), tb = ts_col->Value(b);
            return ta != tb ? ta < tb : ids[a] < ids[b];
        });

    std::vector<MarketData::MarketEvent> events;
    events.reserve(tbl->num_rows());

    for (int64_t r : rows) {
        long long ts = ts_col->Value(r) / (is_tardis ? 1000 : 1);
        if (start_ts_ms > 0 && ts < start_ts_ms) continue;
        if (end_ts_ms > 0 && ts > end_ts_ms) continue;

        MarketData::MarketEvent ev;
        ev.type = MarketData::MarketEvent::TRADE;
        ev.trade = MarketData::TradeEvent{
            config.symbol, config.exchange, config.market_type,
            price_col->Value(r), qty_col->Value(r),
            is_tardis ? side_col->GetView(r) == "sell" : buyer_col->Value(r), ts};
        events.push_back(std::move(ev));
    }
    if (config.use_local_timestamp) {
        auto before = [](const auto& a, const auto& b) {
            return a.trade->timestamp_ms < b.trade->timestamp_ms;
        };
        if (!std::is_sorted(events.begin(), events.end(), before))
            std::stable_sort(events.begin(), events.end(), before);
    }
    return events;
}

static std::vector<MarketData::MarketEvent> convert_bba_table(
    const std::shared_ptr<arrow::Table>& tbl,
    const DatasetConfig& config,
    long long start_ts_ms, long long end_ts_ms) {

    auto schema = tbl->schema();
    bool is_tardis = schema->GetFieldIndex("bid_price") >= 0;
    std::string ts_name = is_tardis ?
        (config.use_local_timestamp ? "local_timestamp" : "timestamp") : "lt";
    std::string bp_name = is_tardis ? "bid_price" : "b";
    std::string bq_name = is_tardis ? "bid_amount" : "B";
    std::string ap_name = is_tardis ? "ask_price" : "a";
    std::string aq_name = is_tardis ? "ask_amount" : "A";

    auto ts_col = std::static_pointer_cast<arrow::Int64Array>(
        tbl->GetColumnByName(ts_name)->chunk(0));
    auto bid_price_col = std::static_pointer_cast<arrow::DoubleArray>(
        tbl->GetColumnByName(bp_name)->chunk(0));
    auto bid_qty_col = std::static_pointer_cast<arrow::DoubleArray>(
        tbl->GetColumnByName(bq_name)->chunk(0));
    auto ask_price_col = std::static_pointer_cast<arrow::DoubleArray>(
        tbl->GetColumnByName(ap_name)->chunk(0));
    auto ask_qty_col = std::static_pointer_cast<arrow::DoubleArray>(
        tbl->GetColumnByName(aq_name)->chunk(0));

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
            config.symbol, config.exchange, config.market_type,
            bid_price_col->Value(r), bid_qty_col->Value(r),
            ask_price_col->Value(r), ask_qty_col->Value(r), ts};
        events.push_back(std::move(ev));
    }
    if (config.use_local_timestamp) {
        auto before = [](const auto& a, const auto& b) {
            return a.bba->timestamp_ms < b.bba->timestamp_ms;
        };
        if (!std::is_sorted(events.begin(), events.end(), before))
            std::stable_sort(events.begin(), events.end(), before);
    }
    return events;
}

static std::vector<MarketData::MarketEvent> convert_l2_table(
    const std::shared_ptr<arrow::Table>& tbl,
    const DatasetConfig& config,
    long long start_ts_ms, long long end_ts_ms) {

    auto schema = tbl->schema();
    bool is_tardis = schema->GetFieldIndex("amount") >= 0 &&
                     schema->GetFieldIndex("qty") < 0;
    std::string ts_name = is_tardis ?
        (config.use_local_timestamp ? "local_timestamp" : "timestamp") : "lt";
    std::string qty_name = is_tardis ? "amount" : "qty";

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

    auto price_col = std::static_pointer_cast<arrow::DoubleArray>(
        tbl->GetColumnByName("price")->chunk(0));
    auto qty_col = std::static_pointer_cast<arrow::DoubleArray>(
        tbl->GetColumnByName(qty_name)->chunk(0));

    long long ts_divisor = is_tardis ? 1000 : 1;

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

        auto same_group = [&](int64_t row) {
            if (ts_col->Value(row) != raw_ts) return false;
            if (is_tardis) {
                return snapshot_col->Value(row) == is_snapshot_row;
            }
            return u_col->Value(row) == cur_u && U_col->Value(row) == cur_U;
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
            config.symbol + ":" + config.exchange + ":" + config.market_type;

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
        l2.exchange_name = config.exchange;
        l2.market_type = config.market_type;
        l2.symbol = config.symbol;
        ev.l2_update = std::move(l2);
        events.push_back(std::move(ev));
    }
    if (is_tardis) {
        std::stable_sort(events.begin(), events.end(),
                         [](const auto& a, const auto& b) {
            auto ta = a.l2_update->update.timestamp;
            auto tb = b.l2_update->update.timestamp;
            if (ta != tb) return ta < tb;
            return a.l2_update->sub_type == MarketData::L2UpdateEvent::FULL_REFRESH &&
                   b.l2_update->sub_type != MarketData::L2UpdateEvent::FULL_REFRESH;
        });
    }
    return events;
}

// Convert a funding-rate parquet row group → FUNDING events with the
// simulated polling-latency offset applied. Schema:
// (timestamp i64 ms, funding_rate f64).
static std::vector<MarketData::MarketEvent> convert_funding_table(
    const std::shared_ptr<arrow::Table>& tbl,
    const DatasetConfig& config,
    long long start_ts_ms, long long end_ts_ms) {
    using MarketData::FUNDING_POLLING_LATENCY_MS;

    auto ts_arr = tbl->GetColumnByName("timestamp");
    auto rate_arr = tbl->GetColumnByName("funding_rate");
    if (!ts_arr || !rate_arr) return {};
    auto ts_col = std::static_pointer_cast<arrow::Int64Array>(ts_arr->chunk(0));
    auto rate_col =
        std::static_pointer_cast<arrow::DoubleArray>(rate_arr->chunk(0));

    std::vector<MarketData::MarketEvent> events;
    events.reserve(tbl->num_rows());
    for (int64_t r = 0; r < tbl->num_rows(); ++r) {
        long long apply_ts = ts_col->Value(r);
        long long emit_ts = apply_ts + FUNDING_POLLING_LATENCY_MS;
        if (start_ts_ms > 0 && emit_ts < start_ts_ms) continue;
        if (end_ts_ms > 0 && emit_ts > end_ts_ms) continue;
        MarketData::MarketEvent ev;
        ev.type = MarketData::MarketEvent::FUNDING;
        ev.funding = MarketData::FundingEvent{
            config.symbol, config.exchange, config.market_type,
            rate_col->Value(r), emit_ts};
        events.push_back(std::move(ev));
    }
    return events;
}

std::vector<MarketData::MarketEvent> convert_table_to_events(
    const std::shared_ptr<arrow::Table>& table,
    const DatasetConfig& config,
    long long start_ts_ms, long long end_ts_ms) {
    if (config.type == "trade") {
        return convert_trade_table(table, config, start_ts_ms, end_ts_ms);
    } else if (config.type == "book_ticker") {
        return convert_bba_table(table, config, start_ts_ms, end_ts_ms);
    } else if (config.type == "lob_l2_update") {
        return convert_l2_table(table, config, start_ts_ms, end_ts_ms);
    } else if (config.type == "funding") {
        return convert_funding_table(table, config, start_ts_ms, end_ts_ms);
    }
    return {};
}

// ---------------------------------------------------------------------------
// ParquetEventStream
// ---------------------------------------------------------------------------

ParquetEventStream::ParquetEventStream(
    const fs::path& path, const DatasetConfig& config,
    long long start_ts_ms, long long end_ts_ms)
    : config_(config), start_ts_ms_(start_ts_ms), end_ts_ms_(end_ts_ms) {

    auto infile = arrow::io::ReadableFile::Open(path.string());
    if (!infile.ok()) return;

    auto reader_result = parquet::arrow::OpenFile(
        infile.ValueOrDie(), arrow::default_memory_pool());
    if (!reader_result.ok()) return;
    reader_ = std::move(reader_result.ValueOrDie());

    if (!reader_->GetSchema(&schema_).ok()) {
        reader_.reset();
        return;
    }

    if (config_.use_local_timestamp) {
        std::vector<std::string> columns;
        if (config_.type == "trade")
            columns = {"local_timestamp", "price", "amount", "side"};
        else if (config_.type == "book_ticker")
            columns = {"local_timestamp", "bid_price", "bid_amount",
                       "ask_price", "ask_amount"};
        else if (config_.type == "lob_l2_update")
            columns = {"local_timestamp", "is_snapshot", "side", "price", "amount"};
        for (const auto& column : columns) {
            int index = schema_->GetFieldIndex(column);
            if (index < 0) throw std::runtime_error("missing Tardis column: " + column);
            col_indices_.push_back(index);
        }
    }

    total_row_groups_ = reader_->num_row_groups();
    advance_to_valid();
}

void ParquetEventStream::load_next_row_group() {
    buffer_.clear();
    cursor_ = 0;

    while (true) {
        if (table_ && table_offset_ < table_->num_rows()) {
            // A slice ends between milliseconds, so a millisecond's trades sort together.
            int64_t rows = kSliceRows;
            if (auto ts = table_->GetColumnByName("timestamp");
                ts && config_.type == "trade" && ts->type()->id() == arrow::Type::INT64) {
                auto v = std::static_pointer_cast<arrow::Int64Array>(ts->chunk(0));
                while (table_offset_ + rows < v->length() &&
                       v->Value(table_offset_ + rows) == v->Value(table_offset_ + rows - 1))
                    ++rows;
            }
            buffer_ = convert_table_to_events(table_->Slice(table_offset_, rows),
                                              config_, start_ts_ms_, end_ts_ms_);
            table_offset_ += rows;
            if (!buffer_.empty()) return;
            continue;
        }
        table_.reset();
        if (current_row_group_ >= total_row_groups_) return;
        std::shared_ptr<arrow::Table> tbl;
        auto read_status = col_indices_.empty()
            ? reader_->ReadRowGroup(current_row_group_, &tbl)
            : reader_->ReadRowGroup(current_row_group_, col_indices_, &tbl);
        if (!read_status.ok() || !tbl) {
            ++current_row_group_;
            continue;
        }
        ++current_row_group_;

        auto combined = tbl->CombineChunks();
        if (!combined.ok() || combined.ValueOrDie()->num_rows() == 0) continue;
        table_ = combined.ValueOrDie();
        table_offset_ = 0;
    }
}

void ParquetEventStream::advance_to_valid() {
    if (!reader_) return;
    load_next_row_group();
}

bool ParquetEventStream::has_next() const {
    return cursor_ < buffer_.size();
}

const MarketData::MarketEvent& ParquetEventStream::peek() const {
    return buffer_[cursor_];
}

MarketData::MarketEvent ParquetEventStream::pop() {
    auto ev = std::move(buffer_[cursor_]);
    ++cursor_;
    if (cursor_ >= buffer_.size()) {
        load_next_row_group();
    }
    return ev;
}

// ---------------------------------------------------------------------------
// DatasetStreamSequence
// ---------------------------------------------------------------------------

DatasetStreamSequence::DatasetStreamSequence(
    const DatasetConfig& config, int dataset_index,
    long long start_ts_ms, long long end_ts_ms)
    : config_(config), dataset_index_(dataset_index),
      start_ts_ms_(start_ts_ms), end_ts_ms_(end_ts_ms) {

    files_ = find_parquet_files_sorted(config.path, start_ts_ms, end_ts_ms);
    open_next_file();
}

void DatasetStreamSequence::open_next_file() {
    stream_.reset();
    while (current_file_index_ < files_.size()) {
        stream_ = std::make_unique<ParquetEventStream>(
            files_[current_file_index_], config_, start_ts_ms_, end_ts_ms_);
        ++current_file_index_;
        if (stream_->has_next()) return;
        stream_.reset();
    }
}

bool DatasetStreamSequence::has_next() const {
    return stream_ && stream_->has_next();
}

const MarketData::MarketEvent& DatasetStreamSequence::peek() const {
    return stream_->peek();
}

MarketData::MarketEvent DatasetStreamSequence::pop() {
    auto ev = stream_->pop();
    if (!stream_->has_next()) {
        open_next_file();
    }
    return ev;
}

// ---------------------------------------------------------------------------
// MergeIterator
// ---------------------------------------------------------------------------

MergeIterator::MergeIterator(
    const std::vector<DatasetConfig>& datasets,
    long long start_ts_ms, long long end_ts_ms) {

    for (size_t i = 0; i < datasets.size(); ++i) {
        auto seq = std::make_unique<DatasetStreamSequence>(
            datasets[i], static_cast<int>(i), start_ts_ms, end_ts_ms);
        if (seq->has_next()) {
            heap_.push({seq.get(), event_timestamp(seq->peek()),
                        seq->dataset_index()});
            streams_.push_back(std::move(seq));
        }
    }
}

bool MergeIterator::has_next() const {
    return !heap_.empty();
}

MarketData::MarketEvent MergeIterator::next() {
    auto top = heap_.top();
    heap_.pop();

    auto ev = top.stream->pop();
    ++total_consumed_;

    if (top.stream->has_next()) {
        heap_.push({top.stream, event_timestamp(top.stream->peek()),
                    top.stream->dataset_index()});
    }

    return ev;
}

}  // namespace HistoricalFeatureGen
