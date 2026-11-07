#include "parquet_utils.h"

#include <arrow/api.h>
#include <arrow/compute/api.h>
#include <arrow/compute/initialize.h>
#include <arrow/dataset/api.h>
#include <arrow/dataset/file_parquet.h>
#include <arrow/dataset/scanner.h>
#include <arrow/filesystem/api.h>
#include <arrow/io/api.h>
#include <common_types/log_event_types.h>
#include <disk_logging/log.h>
#include <parquet/arrow/reader.h>
#include <parquet/arrow/writer.h>
#include <parquet/file_reader.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <regex>
#include <unordered_set>
#include <vector>

#include "aggr_trade_dataset_tick.h"
#include "market_making/dtypes/market_event.h"
#include "ohlcv_dataset_tick.h"
#include "polymarket_trade_tick.h"

namespace fs = std::filesystem;

struct FileInfo {
    fs::path path;
    std::chrono::system_clock::time_point ts;
};

// Partition files on disk use three stem shapes:
//   YYYY-MM               monthly bars (2026-08.parquet)
//   YYYY-MM-DD            daily bars
//   YYYY-MM-DDTHH-MM      hourly raw trades
// Day and time groups are optional. Requiring the day makes every monthly
// file invisible; get_latest_row_timestamp then returns 0 and the polygon
// stream-ingest gate never opens.
const std::regex& partition_filename_regex() {
    static const std::regex re(
        R"((\d{4})-(\d{2})(?:-(\d{2}))?(?:T(\d{2})-(\d{2}))?)");
    return re;
}

bool parse_partition_filename(const std::string& stem, std::tm& tm) {
    std::smatch m;
    if (!std::regex_match(stem, m, partition_filename_regex())) return false;
    tm = {};
    tm.tm_year = std::stoi(m[1]) - 1900;
    tm.tm_mon = std::stoi(m[2]) - 1;
    tm.tm_mday = m[3].matched ? std::stoi(m[3]) : 1;
    tm.tm_hour = m[4].matched ? std::stoi(m[4]) : 0;
    tm.tm_min = m[5].matched ? std::stoi(m[5]) : 0;
    return true;
}

bool directory_exists(const std::filesystem::path& path) {
    return std::filesystem::exists(path) && std::filesystem::is_directory(path);
}

namespace ParquetUtils {

bool parse_partition_window(const std::string& path, long long& start_ms,
                            long long& span_ms) {
    // `month=` and `day=` are both optional. A compacted month file has no day
    // component; a year-partitioned tree (the SEC filing store) has neither.
    // Requiring month= made this return false for every SEC path, which turns a
    // caller's file-pruning loop into "skip everything" and reads as an empty
    // dataset rather than an error.
    static const std::regex partition_regex(
        R"(year=(\d{4})(?:/month=(\d{2})(?:/day=(\d{2}))?)?)");
    std::smatch m;
    if (!std::regex_search(path, m, partition_regex)) return false;

    const int year = std::stoi(m[1]);
    const bool has_month = m[2].matched;
    const bool has_day = m[3].matched;
    const int month = has_month ? std::stoi(m[2]) : 1;

    std::tm start_tm{};
    start_tm.tm_year = year - 1900;
    start_tm.tm_mon = month - 1;
    start_tm.tm_mday = has_day ? std::stoi(m[3]) : 1;
    start_ms = static_cast<long long>(timegm(&start_tm)) * 1000LL;

    if (has_day) {
        span_ms = 86400000LL;
        return true;
    }

    if (!has_month) {
        std::tm next_year_tm{};
        next_year_tm.tm_year = year + 1 - 1900;
        next_year_tm.tm_mon = 0;
        next_year_tm.tm_mday = 1;
        span_ms =
            static_cast<long long>(timegm(&next_year_tm)) * 1000LL - start_ms;
        return true;
    }

    // Month length from the calendar rather than a table, so February and
    // leap years need no special case.
    std::tm next_tm{};
    next_tm.tm_year = (month == 12 ? year + 1 : year) - 1900;
    next_tm.tm_mon = (month == 12 ? 0 : month);
    next_tm.tm_mday = 1;
    span_ms = static_cast<long long>(timegm(&next_tm)) * 1000LL - start_ms;
    return true;
}

std::shared_ptr<arrow::Table> scan_partitions(
    const std::string& base_path,
    const std::chrono::system_clock::time_point& start_time,
    const std::optional<std::chrono::system_clock::time_point>& end_time) {
    arrow::MemoryPool* pool = arrow::default_memory_pool();

    // Cache for empty files
    static std::unordered_set<std::string> empty_files;

    // ──────────────────── 1. Scan · collect (path, timestamp)
    // ────────────────────
    std::vector<FileInfo> files;

    if (!directory_exists(base_path)) {
        std::cerr << "Directory does not exist: " << base_path << std::endl;
        return nullptr;
    }

    struct Candidate {
        fs::path path;
        std::chrono::system_clock::time_point ts;
        long long span_ms;
        long long month_span_ms;
    };
    std::vector<Candidate> candidates;
    // Month-compacted trees keep each month in a day=01 partition, which reads
    // as a one-day span: a request starting after the 2nd would drop the whole
    // month. A tree where no file sits outside day=01 is such a tree.
    bool every_file_on_day_one = true;
    static const std::regex day_regex(R"(day=(\d{2}))");
    static const std::regex month_regex(R"(year=(\d{4})/month=(\d{2}))");
    for (const auto& e : fs::recursive_directory_iterator(base_path)) {
        if (!e.is_regular_file() || e.path().extension() != ".parquet")
            continue;

        // Check if file is empty (using cache)
        std::string file_path = e.path().string();
        if (empty_files.find(file_path) != empty_files.end()) {
            continue;
        }

        if (std::filesystem::file_size(e.path()) == 0) {
            empty_files.insert(file_path);
            continue;
        }

        std::tm tm{};
        if (!parse_partition_filename(e.path().stem().string(), tm)) continue;

        const auto ts =
            std::chrono::system_clock::from_time_t(std::mktime(&tm));
        long long span_start_ms = 0;
        long long span_ms = 86400000LL;
        if (!ParquetUtils::parse_partition_window(file_path, span_start_ms,
                                                  span_ms)) {
            span_ms = 86400000LL;
        }
        std::smatch day;
        if (!std::regex_search(file_path, day, day_regex) || day[1] != "01")
            every_file_on_day_one = false;
        long long month_span_ms = span_ms;
        std::smatch ym;
        if (std::regex_search(file_path, ym, month_regex)) {
            const int year = std::stoi(ym[1]);
            const int month = std::stoi(ym[2]);
            std::tm from{}, to{};
            from.tm_year = year - 1900;
            from.tm_mon = month - 1;
            from.tm_mday = 1;
            to.tm_year = (month == 12 ? year + 1 : year) - 1900;
            to.tm_mon = month == 12 ? 0 : month;
            to.tm_mday = 1;
            month_span_ms = (timegm(&to) - timegm(&from)) * 1000LL;
        }
        candidates.push_back({e.path(), ts, span_ms, month_span_ms});
    }

    for (const auto& c : candidates) {
        const long long span_ms =
            every_file_on_day_one ? c.month_span_ms : c.span_ms;
        // Compare the file's END against the window start: a file beginning
        // before the request can still hold the bars it asks for.
        const auto file_end = c.ts + std::chrono::milliseconds(span_ms);
        if (file_end < start_time) continue;
        if (end_time && c.ts > *end_time) continue;
        files.push_back({c.path, c.ts});
    }
    if (files.empty()) {
        return nullptr;
    }

    // ──────────────────── 2. Sort by timestamp ────────────────────
    std::sort(files.begin(), files.end(),
              [](const FileInfo& a, const FileInfo& b) { return a.ts < b.ts; });

    const size_t max_concurrent_files = 128;
    std::vector<std::shared_ptr<arrow::Table>> tables(files.size());
    std::vector<std::future<void>> futures;
    futures.reserve(max_concurrent_files);

    for (std::size_t i = 0; i < files.size(); ++i) {
        if (futures.size() >= max_concurrent_files) {
            futures[0].get();  // Wait for the oldest future
            futures.erase(futures.begin());
        }

        futures.emplace_back(std::async(std::launch::async, [&, i]() {
            try {
                auto infile =
                    arrow::io::ReadableFile::Open(files[i].path.string());
                if (!infile.ok()) {
                    std::string error_msg =
                        "Failed to open " + files[i].path.string() +
                        " with error: " + infile.status().ToString();
                    std::cerr << error_msg << '\n';
                    LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_PARQUET_WRITE_FAILED, error_msg, nlohmann::json{});
                    return;
                }

                std::unique_ptr<parquet::arrow::FileReader> reader;
                auto reader_result =
                    parquet::arrow::OpenFile(infile.ValueOrDie(), pool);
                if (!reader_result.ok()) {
                    std::string error_msg =
                        "Failed to create reader for " +
                        files[i].path.string() +
                        " with error: " + reader_result.status().ToString();
                    std::cerr << error_msg << '\n';
                    LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_PARQUET_WRITE_FAILED, error_msg, nlohmann::json{});
                    return;
                }
                reader = std::move(reader_result.ValueOrDie());

                std::shared_ptr<arrow::Table> tbl;
                auto read_status = reader->ReadTable(&tbl);
                if (!read_status.ok()) {
                    std::string error_msg =
                        "Failed to read table " + files[i].path.string() +
                        " with error: " + read_status.ToString();
                    std::cerr << error_msg << '\n';
                    LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_PARQUET_WRITE_FAILED, error_msg, nlohmann::json{});
                    return;
                }

                for (const auto* name : {"kline_open_time_ms", "kline_close_time_ms"}) {
                    const int column = tbl->schema()->GetFieldIndex(name);
                    if (column < 0 || tbl->field(column)->type()->id() != arrow::Type::INT64)
                        continue;
                    const auto type = arrow::timestamp(arrow::TimeUnit::MILLI);
                    const auto values = arrow::compute::Cast(tbl->column(column), type)
                                            .ValueOrDie().chunked_array();
                    tbl = tbl->SetColumn(column, tbl->field(column)->WithType(type), values)
                              .ValueOrDie();
                }
                tables[i] = std::move(tbl);

                // Explicitly close the reader and file
                reader.reset();
                auto close_status = infile.ValueOrDie()->Close();
                if (!close_status.ok()) {
                    std::string error_msg =
                        "Failed to close file " + files[i].path.string() +
                        " with error: " + close_status.ToString();
                    std::cerr << error_msg << '\n';
                    LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_PARQUET_WRITE_FAILED, error_msg, nlohmann::json{});
                }
            } catch (const std::exception& e) {
                std::string error_msg = "Exception while processing " +
                                        files[i].path.string() + ": " +
                                        e.what();
                std::cerr << error_msg << '\n';
                LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_PARQUET_WRITE_FAILED, error_msg, nlohmann::json{});
            }
        }));
    }

    // Wait for remaining futures
    for (auto& f : futures) {
        f.get();
    }

    arrow::ConcatenateTablesOptions opts =
        arrow::ConcatenateTablesOptions::Defaults();
    opts.unify_schemas = true;
    opts.field_merge_options.promote_decimal = true;
    opts.field_merge_options.promote_numeric_width = true;

    auto concat = arrow::ConcatenateTables(tables, opts);
    if (!concat.ok()) {
        std::cerr << "ConcatenateTables failed: " << concat.status() << '\n';
        return nullptr;
    }
    return concat.ValueOrDie()->CombineChunks().ValueOrDie();
}

long long get_latest_row_timestamp(const std::string& base_path,
                                   const std::string& time_column_name) {
    if (!directory_exists(base_path)) {
        return 0LL;
    }

    std::string time_column_name_copy = time_column_name;

    fs::path latest_path;
    std::chrono::system_clock::time_point latest_tp{};
    bool found_file = false;

    for (const auto& e : fs::recursive_directory_iterator(base_path)) {
        if (!e.is_regular_file() || e.path().extension() != ".parquet")
            continue;

        std::tm tm{};
        if (!parse_partition_filename(e.path().stem().string(), tm)) continue;

        std::chrono::system_clock::time_point tp =
            std::chrono::system_clock::from_time_t(std::mktime(&tm));
        if (!found_file || tp > latest_tp) {
            latest_tp = tp;
            latest_path = e.path();
            found_file = true;
        }
    }
    if (!found_file) return 0LL;

    arrow::MemoryPool* pool = arrow::default_memory_pool();
    auto infile_res = arrow::io::ReadableFile::Open(latest_path.string());
    if (!infile_res.ok()) {
        std::cerr << "Failed to open " << latest_path << '\n';
        return 0LL;
    }

    auto reader_result =
        parquet::arrow::OpenFile(infile_res.ValueOrDie(), pool);
    if (!reader_result.ok()) {
        std::cerr << "Failed to create parquet reader for " << latest_path
                  << " with error: " << reader_result.status().ToString()
                  << '\n';
        return 0LL;
    }
    std::unique_ptr<parquet::arrow::FileReader> reader =
        std::move(reader_result.ValueOrDie());

    std::shared_ptr<arrow::Table> raw_tbl;
    if (!reader->ReadTable(&raw_tbl).ok()) {
        std::cerr << "Failed to read parquet table from " << latest_path
                  << '\n';
        return 0LL;
    }

    auto tbl_res = raw_tbl->CombineChunks();
    if (!tbl_res.ok()) {
        std::cerr << "CombineChunks failed: " << tbl_res.status() << '\n';
        return 0LL;
    }
    std::shared_ptr<arrow::Table> tbl = tbl_res.ValueOrDie();
    if (tbl->num_rows() == 0) return 0LL;

    int col_idx = -1;
    if (!time_column_name_copy.empty()) {
        col_idx = tbl->schema()->GetFieldIndex(time_column_name_copy);
    }
    if (col_idx == -1) {
        for (int i = 0; i < tbl->num_columns(); ++i) {
            auto id = tbl->schema()->field(i)->type()->id();
            if (id == arrow::Type::TIMESTAMP || id == arrow::Type::INT64) {
                col_idx = i;
                time_column_name_copy = tbl->schema()->field(i)->name();
                break;
            }
        }
    }
    if (col_idx == -1) {
        std::cerr << "No suitable timestamp column found in " << latest_path
                  << '\n';
        return 0LL;
    }

    const auto& chunked = tbl->column(col_idx);
    if (chunked->num_chunks() == 0 || chunked->chunk(0)->length() == 0)
        return 0LL;

    // CombineChunks() guarantees exactly one chunk per column
    auto id = chunked->type()->id();
    if (id == arrow::Type::TIMESTAMP) {
        auto arr =
            std::static_pointer_cast<arrow::TimestampArray>(chunked->chunk(0));
        return static_cast<long long>(arr->Value(arr->length() - 1));
    } else {  // treat INT64 as epoch-like
        auto arr =
            std::static_pointer_cast<arrow::Int64Array>(chunked->chunk(0));
        return static_cast<long long>(arr->Value(arr->length() - 1));
    }
}

std::string ohlcv_partition_month_key(long long kline_open_time_ms) {
    std::time_t seconds = static_cast<time_t>(kline_open_time_ms / 1000);
    std::tm tm{};
    gmtime_r(&seconds, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "year=%Y/month=%m", &tm);
    return std::string(buf);
}

// Both writers bucket through the month key above so a dirty-month marker can
// never disagree with where the rows actually land.
static std::string ohlcv_partition_file_key(long long kline_open_time_ms) {
    std::time_t seconds = static_cast<time_t>(kline_open_time_ms / 1000);
    std::tm tm{};
    gmtime_r(&seconds, &tm);
    char name[24];
    std::strftime(name, sizeof(name), "%Y-%m.parquet", &tm);
    return ohlcv_partition_month_key(kline_open_time_ms) + "/" + name;
}

int write_ohlcv_ticks_partitioned(
    const std::string& base_path,
    const std::vector<MarketData::OhlcvDatasetTick>& ticks) {
    if (ticks.empty()) return 0;

    std::unordered_map<std::string,
                       std::vector<const MarketData::OhlcvDatasetTick*>>
        bucket;
    char buf[64];

    for (const auto& t : ticks) {
        // Monthly, not daily: one file per day per symbol per timeframe
        // exhausted the inode table on the market-data volume.
        bucket[ohlcv_partition_file_key(t.kline_open_time)].push_back(&t);
    }

    int new_files_created = 0;
    for (auto& [key, vec] : bucket) {
        const fs::path parquet_path = fs::path(base_path) / key;
        const fs::path tmp_path = parquet_path.string() + ".tmp";

        // Merge with any existing rows for this day so we get UNION-of-iters
        // semantics (matches sqlite's INSERT-OR-IGNORE). Holds a copy of any
        // existing rows so they survive subsequent partial-vec writes.
        std::vector<MarketData::OhlcvDatasetTick> existing_owned;
        if (fs::exists(parquet_path)) {
            try {
                auto file_result =
                    arrow::io::ReadableFile::Open(parquet_path.string());
                if (file_result.ok()) {
                    auto reader_result = parquet::arrow::OpenFile(
                        file_result.ValueOrDie(),
                        arrow::default_memory_pool());
                    if (reader_result.ok()) {
                        auto reader = std::move(reader_result.ValueOrDie());
                        std::shared_ptr<arrow::Table> tbl;
                        if (reader->ReadTable(&tbl).ok() && tbl) {
                            existing_owned =
                                MarketData::OhlcvDatasetTick::from_arrow_table(
                                    tbl);
                        }
                    }
                }
            } catch (...) {
            }
        }

        // Build merged vec: keep one row per kline_open_time. Prefer the
        // NEW value when both exist (the in-memory vec is more recent).
        std::unordered_map<long long, const MarketData::OhlcvDatasetTick*>
            by_ts;
        by_ts.reserve(vec.size() + existing_owned.size());
        for (const auto& t : existing_owned) by_ts[t.kline_open_time] = &t;
        for (const auto* p : vec) by_ts[p->kline_open_time] = p;

        std::vector<const MarketData::OhlcvDatasetTick*> merged;
        merged.reserve(by_ts.size());
        for (const auto& [_, p] : by_ts) merged.push_back(p);
        std::sort(merged.begin(), merged.end(),
                  [](const MarketData::OhlcvDatasetTick* a,
                     const MarketData::OhlcvDatasetTick* b) {
                      return a->kline_open_time < b->kline_open_time;
                  });
        // Repoint vec to the merged set so the writer below emits the union.
        vec = std::move(merged);

        if (vec.empty()) {
            continue;
        }

        arrow::TimestampBuilder open_ts_b(
            arrow::timestamp(arrow::TimeUnit::MILLI),
            arrow::default_memory_pool());
        arrow::TimestampBuilder close_ts_b(
            arrow::timestamp(arrow::TimeUnit::MILLI),
            arrow::default_memory_pool());
        arrow::DoubleBuilder close_b, open_b, high_b, low_b, vol_b, quote_vol_b,
            taker_bb_b, taker_bq_b;
        arrow::Int64Builder trades_b;

        for (auto* p : vec) {
            if (!open_ts_b.Append(p->kline_open_time).ok() ||
                !close_b.Append(p->close_price).ok() ||
                !open_b.Append(p->open_price).ok() ||
                !high_b.Append(p->high_price).ok() ||
                !low_b.Append(p->low_price).ok() ||
                !vol_b.Append(p->volume).ok() ||
                !close_ts_b.Append(p->kline_close_time).ok() ||
                !quote_vol_b.Append(p->quote_asset_volume).ok() ||
                !trades_b.Append(p->number_of_trades).ok() ||
                !taker_bb_b.Append(p->taker_buy_base_volume).ok() ||
                !taker_bq_b.Append(p->taker_buy_quote_volume).ok()) {
                return -1;
            }
        }

        std::shared_ptr<arrow::Array> open_ts, close_ts, close_, open_, high_,
            low_, vol_, quote_vol, trades, taker_bb, taker_bq;

        if (!open_ts_b.Finish(&open_ts).ok() || !close_b.Finish(&close_).ok() ||
            !open_b.Finish(&open_).ok() || !high_b.Finish(&high_).ok() ||
            !low_b.Finish(&low_).ok() || !vol_b.Finish(&vol_).ok() ||
            !close_ts_b.Finish(&close_ts).ok() ||
            !quote_vol_b.Finish(&quote_vol).ok() ||
            !trades_b.Finish(&trades).ok() ||
            !taker_bb_b.Finish(&taker_bb).ok() ||
            !taker_bq_b.Finish(&taker_bq).ok()) {
            return -1;
        }

        auto schema = arrow::schema(
            {arrow::field("kline_open_time_ms",
                          arrow::timestamp(arrow::TimeUnit::MILLI)),
             arrow::field("close_price", arrow::float64()),
             arrow::field("open_price", arrow::float64()),
             arrow::field("high_price", arrow::float64()),
             arrow::field("low_price", arrow::float64()),
             arrow::field("volume", arrow::float64()),
             arrow::field("kline_close_time_ms",
                          arrow::timestamp(arrow::TimeUnit::MILLI)),
             arrow::field("quote_asset_volume", arrow::float64()),
             arrow::field("number_of_trades", arrow::int64()),
             arrow::field("taker_buy_base_volume", arrow::float64()),
             arrow::field("taker_buy_quote_volume", arrow::float64())});

        auto table = arrow::Table::Make(
            schema, {open_ts, close_, open_, high_, low_, vol_, close_ts,
                     quote_vol, trades, taker_bb, taker_bq});
        fs::create_directories(parquet_path.parent_path());

        auto tmp_out_result =
            arrow::io::FileOutputStream::Open(tmp_path.string());
        if (!tmp_out_result.ok()) {
            return -1;
        }
        auto tmp_out = tmp_out_result.ValueOrDie();

        auto writer_props = parquet::WriterProperties::Builder()
                                .compression(parquet::Compression::ZSTD)
                                ->compression_level(4)
                                ->build();

        auto write_status = parquet::arrow::WriteTable(
            *table, arrow::default_memory_pool(), tmp_out, 1024, writer_props);
        if (!write_status.ok()) {
            std::cerr << "Failed to write parquet table with error: "
                      << write_status.ToString() << '\n';
            return -1;
        }

        auto close_status = tmp_out->Close();
        if (!close_status.ok()) {
            std::string error_msg = "Failed to close output file " +
                                    tmp_path.string() +
                                    " with error: " + close_status.ToString();
            std::cerr << error_msg << '\n';
            LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_PARQUET_WRITE_FAILED, error_msg, nlohmann::json{});
            return -1;
        }

        std::error_code ec;
        fs::rename(tmp_path, parquet_path, ec);
        if (ec) {
            std::string error_msg =
                "Failed to rename temp file to final file: " +
                parquet_path.string() + " with error: " + ec.message();
            std::cerr << error_msg << '\n';
            LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_PARQUET_WRITE_FAILED, error_msg, nlohmann::json{});
            std::error_code dummy;
            fs::remove(tmp_path, dummy);
            return -1;
        }

        new_files_created++;
    }

    return new_files_created;
}

int write_ohlcv_ticks_partitioned_minimal(
    const std::string& base_path,
    const std::vector<MarketData::OhlcvDatasetTick>& ticks) {
    if (ticks.empty()) return 0;

    std::unordered_map<std::string,
                       std::vector<const MarketData::OhlcvDatasetTick*>>
        bucket;
    char buf[64];

    // Find the latest timestamp across all ticks
    long long latest_timestamp = 0;
    for (const auto& t : ticks) {
        latest_timestamp = std::max(latest_timestamp, t.kline_close_time);
    }

    for (const auto& t : ticks) {
        // Was one file per bar-hour, which is 24x worse than daily and the
        // second largest contributor to inode exhaustion.
        bucket[ohlcv_partition_file_key(t.kline_open_time)].push_back(&t);
    }

    int new_files_created = 0;
    for (auto& [key, vec] : bucket) {
        const fs::path parquet_path = fs::path(base_path) / key;
        const fs::path tmp_path = parquet_path.string() + ".tmp";

        std::sort(vec.begin(), vec.end(),
                  [](const MarketData::OhlcvDatasetTick* a,
                     const MarketData::OhlcvDatasetTick* b) {
                      return a->kline_open_time < b->kline_open_time;
                  });

        const long long hour_start_ms =
            (vec.front()->kline_open_time / (60'000LL * 60)) * (60'000LL * 60);
        const long long hour_end_ms = hour_start_ms + (60 * 60 * 1'000) - 1;

        if (latest_timestamp <= hour_end_ms) {
            continue;
        }

        if (fs::exists(parquet_path)) {
            try {
                auto file_result =
                    arrow::io::ReadableFile::Open(parquet_path.string());
                if (file_result.ok()) {
                    auto reader_result = parquet::ParquetFileReader::Open(
                        file_result.ValueOrDie());
                    if (reader_result) {
                        auto metadata = reader_result->metadata();
                        if (metadata && metadata->num_rows() >=
                                            static_cast<int64_t>(vec.size())) {
                            continue;
                        }
                    }
                }
            } catch (...) {
            }
        }

        arrow::TimestampBuilder open_ts_b(
            arrow::timestamp(arrow::TimeUnit::MILLI),
            arrow::default_memory_pool());
        arrow::TimestampBuilder close_ts_b(
            arrow::timestamp(arrow::TimeUnit::MILLI),
            arrow::default_memory_pool());
        arrow::DoubleBuilder high_b, low_b, close_b;

        for (auto* p : vec) {
            if (!open_ts_b.Append(p->kline_open_time).ok() ||
                !high_b.Append(p->high_price).ok() ||
                !low_b.Append(p->low_price).ok() ||
                !close_b.Append(p->close_price).ok() ||
                !close_ts_b.Append(p->kline_close_time).ok()) {
                return -1;
            }
        }

        std::shared_ptr<arrow::Array> open_ts, close_ts, high_, low_, close_;

        if (!open_ts_b.Finish(&open_ts).ok() || !high_b.Finish(&high_).ok() ||
            !low_b.Finish(&low_).ok() || !close_b.Finish(&close_).ok() ||
            !close_ts_b.Finish(&close_ts).ok()) {
            return -1;
        }

        auto schema = arrow::schema(
            {arrow::field("kline_open_time_ms",
                          arrow::timestamp(arrow::TimeUnit::MILLI)),
             arrow::field("high_price", arrow::float64()),
             arrow::field("low_price", arrow::float64()),
             arrow::field("close_price", arrow::float64()),
             arrow::field("kline_close_time_ms",
                          arrow::timestamp(arrow::TimeUnit::MILLI))});

        auto table = arrow::Table::Make(
            schema, {open_ts, high_, low_, close_, close_ts});
        fs::create_directories(parquet_path.parent_path());

        auto tmp_out_result =
            arrow::io::FileOutputStream::Open(tmp_path.string());
        if (!tmp_out_result.ok()) {
            return -1;
        }
        auto tmp_out = tmp_out_result.ValueOrDie();

        auto writer_props = parquet::WriterProperties::Builder()
                                .compression(parquet::Compression::ZSTD)
                                ->compression_level(4)
                                ->build();

        auto write_status = parquet::arrow::WriteTable(
            *table, arrow::default_memory_pool(), tmp_out, 1024, writer_props);
        if (!write_status.ok()) {
            std::cerr << "Failed to write parquet table with error: "
                      << write_status.ToString() << '\n';
            return -1;
        }

        auto close_status = tmp_out->Close();
        if (!close_status.ok()) {
            std::string error_msg = "Failed to close output file " +
                                    tmp_path.string() +
                                    " with error: " + close_status.ToString();
            std::cerr << error_msg << '\n';
            LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_PARQUET_WRITE_FAILED, error_msg, nlohmann::json{});
            return -1;
        }

        std::error_code ec;
        fs::rename(tmp_path, parquet_path, ec);
        if (ec) {
            std::string error_msg =
                "Failed to rename temp file to final file: " +
                parquet_path.string() + " with error: " + ec.message();
            std::cerr << error_msg << '\n';
            LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_PARQUET_WRITE_FAILED, error_msg, nlohmann::json{});
            std::error_code dummy;
            fs::remove(tmp_path, dummy);
            return -1;
        }

        new_files_created++;
    }

    return new_files_created;
}

std::shared_ptr<arrow::Table> scan_partitions_fast(
    const std::string& base_path, const std::string& time_column,
    const std::optional<std::chrono::system_clock::time_point>& start_time,
    const std::optional<std::chrono::system_clock::time_point>& end_time) {
    try {
        auto start = std::chrono::high_resolution_clock::now();

        // 1. Filesystem & selector
        auto fs = std::make_shared<arrow::fs::LocalFileSystem>();
        arrow::fs::FileSelector selector;
        selector.base_dir = base_path;
        selector.recursive = true;

        // 2. Dataset factory
        arrow::dataset::FileSystemFactoryOptions opts;
        opts.partitioning = arrow::dataset::HivePartitioning::MakeFactory();

        auto factory = arrow::dataset::FileSystemDatasetFactory::Make(
            fs, selector, std::make_shared<arrow::dataset::ParquetFileFormat>(),
            opts);
        if (!factory.ok()) {
            std::cerr << "Failed to create dataset factory: "
                      << factory.status() << '\n';
            return nullptr;
        }

        auto dataset = factory.ValueOrDie()->Finish();
        if (!dataset.ok()) {
            std::cerr << "Failed to create dataset: " << dataset.status()
                      << '\n';
            return nullptr;
        }

        static const arrow::Status compute_status = arrow::compute::Initialize();
        if (!compute_status.ok()) {
            std::cerr << "Failed to initialize Arrow compute: "
                      << compute_status << '\n';
            return nullptr;
        }

        const std::shared_ptr<arrow::Field> time_field =
            dataset.ValueOrDie()->schema()->GetFieldByName(time_column);
        if (time_field == nullptr) {
            std::cerr << "Time column not found in parquet dataset: "
                      << time_column << '\n';
            return nullptr;
        }

        auto time_literal = [&](std::chrono::system_clock::time_point value)
            -> std::shared_ptr<arrow::Scalar> {
            const long long timestamp_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    value.time_since_epoch())
                    .count();
            if (time_field->type()->id() == arrow::Type::INT64) {
                return std::make_shared<arrow::Int64Scalar>(timestamp_ms);
            }
            if (time_field->type()->id() != arrow::Type::TIMESTAMP) {
                return nullptr;
            }
            const auto timestamp = std::make_shared<arrow::TimestampScalar>(
                timestamp_ms, arrow::TimeUnit::MILLI);
            const arrow::Result<std::shared_ptr<arrow::Scalar>> converted =
                timestamp->CastTo(time_field->type());
            return converted.ok() ? converted.ValueOrDie() : nullptr;
        };

        auto dataset_end = std::chrono::high_resolution_clock::now();
        auto dataset_duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(dataset_end -
                                                                  start);
        std::cout << "Dataset creation took: " << dataset_duration.count()
                  << "ms" << std::endl;

        // 3. Scanner with optimized settings
        arrow::dataset::ScannerBuilder builder(dataset.ValueOrDie());

        std::optional<arrow::compute::Expression> filter;
        auto add_filter = [&](arrow::compute::Expression expression) {
            filter = filter.has_value()
                         ? arrow::compute::and_(*filter, std::move(expression))
                         : std::move(expression);
        };
        if (start_time.has_value()) {
            const std::shared_ptr<arrow::Scalar> scalar =
                time_literal(*start_time);
            if (scalar == nullptr) {
                std::cerr << "Unsupported parquet time column type: "
                          << time_field->type()->ToString() << '\n';
                return nullptr;
            }
            add_filter(arrow::compute::greater_equal(
                arrow::compute::field_ref(time_column),
                arrow::compute::literal(scalar)));
        }
        if (end_time.has_value()) {
            const std::shared_ptr<arrow::Scalar> scalar =
                time_literal(*end_time);
            if (scalar == nullptr) {
                std::cerr << "Unsupported parquet time column type: "
                          << time_field->type()->ToString() << '\n';
                return nullptr;
            }
            add_filter(arrow::compute::less(
                arrow::compute::field_ref(time_column),
                arrow::compute::literal(scalar)));
        }
        if (filter.has_value()) {
            const arrow::Status filter_status = builder.Filter(*filter);
            if (!filter_status.ok()) {
                std::cerr << "Failed to apply parquet time filter: "
                          << filter_status << '\n';
                return nullptr;
            }
        }

        // Use even larger batch size (256MB) and enable threading
        if (!builder.BatchSize(256 * 1024 * 1024).ok()) {  // 256MB batches
            std::cerr << "Failed to set batch size" << '\n';
            return nullptr;
        }
        if (!builder.UseThreads(true).ok()) {
            std::cerr << "Failed to enable threading" << '\n';
            return nullptr;
        }

        auto scanner = builder.Finish();
        if (!scanner.ok()) {
            std::cerr << "Failed to create scanner: " << scanner.status()
                      << '\n';
            return nullptr;
        }

        // 4. Materialize table with optimized settings
        auto table = scanner.ValueOrDie()->ToTable();
        if (!table.ok()) {
            std::cerr << "Failed to scan to table: " << table.status() << '\n';
            return nullptr;
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto scan_duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(end -
                                                                  dataset_end);
        std::cout << "Table scanning took: " << scan_duration.count() << "ms"
                  << std::endl;

        return table.ValueOrDie();

    } catch (const std::exception& e) {
        std::cerr << "Exception in scan_partitions_fast: " << e.what() << '\n';
        return nullptr;
    }
}

std::vector<PolymarketTradeTick> load_polymarket_trades(
    const std::string& base_path, long long start_ts_ms, long long end_ts_ms) {
    if (!directory_exists(base_path)) {
        std::cerr << "Polymarket trades directory does not exist: " << base_path
                  << std::endl;
        return {};
    }

    arrow::MemoryPool* pool = arrow::default_memory_pool();

    struct FileEntry {
        fs::path path;
    };
    std::vector<FileEntry> files;

    for (const auto& entry : fs::recursive_directory_iterator(base_path)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".parquet")
            continue;
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

        files.push_back({entry.path()});
    }

    if (files.empty()) return {};

    static const std::vector<std::string> needed_columns = {
        "side",      "conditionId", "size",    "price",
        "timestamp", "slug",        "outcome", "outcomeIndex"};

    std::vector<std::vector<PolymarketTradeTick>> per_file_ticks(files.size());
    std::vector<std::future<void>> futures;

    for (size_t i = 0; i < files.size(); ++i) {
        futures.emplace_back(std::async(std::launch::async, [&, i]() {
            try {
                auto infile =
                    arrow::io::ReadableFile::Open(files[i].path.string());
                if (!infile.ok()) {
                    std::cerr << "Failed to open " << files[i].path << ": "
                              << infile.status() << '\n';
                    return;
                }

                auto reader_result =
                    parquet::arrow::OpenFile(infile.ValueOrDie(), pool);
                if (!reader_result.ok()) {
                    std::cerr << "Failed to create reader for " << files[i].path
                              << ": " << reader_result.status() << '\n';
                    return;
                }
                auto reader = std::move(reader_result.ValueOrDie());

                std::shared_ptr<arrow::Schema> schema;
                if (!reader->GetSchema(&schema).ok()) return;

                std::vector<int> col_indices;
                for (const auto& col_name : needed_columns) {
                    int idx = schema->GetFieldIndex(col_name);
                    if (idx >= 0) col_indices.push_back(idx);
                }

                std::shared_ptr<arrow::Table> tbl;
                if (!reader->ReadTable(col_indices, &tbl).ok() || !tbl ||
                    tbl->num_rows() == 0)
                    return;

                auto combined = tbl->CombineChunks();
                if (!combined.ok()) return;

                auto all_ticks = PolymarketTradeTick::from_arrow_table(
                    combined.ValueOrDie());

                std::vector<PolymarketTradeTick> filtered;
                filtered.reserve(all_ticks.size());
                for (auto& tick : all_ticks) {
                    if (tick.timestamp >= start_ts_ms &&
                        tick.timestamp <= end_ts_ms) {
                        filtered.push_back(std::move(tick));
                    }
                }
                per_file_ticks[i] = std::move(filtered);

                reader.reset();
                auto close_status = infile.ValueOrDie()->Close();
                if (!close_status.ok()) {
                    std::cerr << "Failed to close " << files[i].path << ": "
                              << close_status << '\n';
                }
            } catch (const std::exception& e) {
                std::cerr << "Exception processing " << files[i].path << ": "
                          << e.what() << '\n';
            }
        }));
    }

    for (auto& f : futures) f.get();

    size_t total = 0;
    for (const auto& v : per_file_ticks) total += v.size();

    std::vector<PolymarketTradeTick> result;
    result.reserve(total);
    for (auto& v : per_file_ticks) {
        result.insert(result.end(), std::make_move_iterator(v.begin()),
                      std::make_move_iterator(v.end()));
    }
    return result;
}

std::vector<Core::BacktestPredictionEvent> load_prediction_ticks(
    const std::string& path, const std::string& model_name,
    long long start_ts_ms, long long end_ts_ms) {
    if (!fs::exists(path)) {
        std::cerr << "Prediction file does not exist: " << path << std::endl;
        return {};
    }

    arrow::MemoryPool* pool = arrow::default_memory_pool();
    auto infile = arrow::io::ReadableFile::Open(path);
    if (!infile.ok()) {
        std::cerr << "Failed to open prediction file: " << path << ": "
                  << infile.status() << std::endl;
        return {};
    }

    auto reader_result = parquet::arrow::OpenFile(infile.ValueOrDie(), pool);
    if (!reader_result.ok()) {
        std::cerr << "Failed to create reader for prediction file: " << path
                  << ": " << reader_result.status() << std::endl;
        return {};
    }
    auto reader = std::move(reader_result.ValueOrDie());

    std::shared_ptr<arrow::Schema> schema;
    if (!reader->GetSchema(&schema).ok()) {
        std::cerr << "Failed to get schema for prediction file: " << path
                  << std::endl;
        return {};
    }

    int ts_idx = schema->GetFieldIndex("timestamp_ms");
    int pred_idx = schema->GetFieldIndex("prediction");
    if (ts_idx < 0 || pred_idx < 0) {
        std::cerr << "Prediction file missing required columns (timestamp_ms, "
                     "prediction): "
                  << path << std::endl;
        return {};
    }

    std::shared_ptr<arrow::Table> tbl;
    if (!reader->ReadTable({ts_idx, pred_idx}, &tbl).ok() || !tbl ||
        tbl->num_rows() == 0) {
        return {};
    }

    auto combined = tbl->CombineChunks();
    if (!combined.ok()) {
        std::cerr << "CombineChunks failed for prediction file: " << path
                  << std::endl;
        return {};
    }
    tbl = combined.ValueOrDie();

    auto ts_col = std::static_pointer_cast<arrow::Int64Array>(
        tbl->GetColumnByName("timestamp_ms")->chunk(0));
    auto pred_chunk = tbl->GetColumnByName("prediction")->chunk(0);
    bool is_f64 = pred_chunk->type_id() == arrow::Type::DOUBLE;

    std::vector<Core::BacktestPredictionEvent> result;
    result.reserve(tbl->num_rows() / 4);

    if (is_f64) {
        auto pred_col = std::static_pointer_cast<arrow::DoubleArray>(pred_chunk);
        for (int64_t i = 0; i < tbl->num_rows(); ++i) {
            long long ts = ts_col->Value(i);
            if (ts < start_ts_ms || ts > end_ts_ms) continue;
            Core::BacktestPredictionEvent event;
            event.model_name = model_name;
            event.prediction = static_cast<float>(pred_col->Value(i));
            event.timestamp_ms = ts;
            result.push_back(std::move(event));
        }
    } else {
        auto pred_col = std::static_pointer_cast<arrow::FloatArray>(pred_chunk);
        for (int64_t i = 0; i < tbl->num_rows(); ++i) {
            long long ts = ts_col->Value(i);
            if (ts < start_ts_ms || ts > end_ts_ms) continue;
            Core::BacktestPredictionEvent event;
            event.model_name = model_name;
            event.prediction = pred_col->Value(i);
            event.timestamp_ms = ts;
            result.push_back(std::move(event));
        }
    }

    reader.reset();
    auto close_status = infile.ValueOrDie()->Close();
    if (!close_status.ok()) {
        std::cerr << "Failed to close prediction file: " << path << std::endl;
    }

    return result;
}

std::vector<AggrTradeDatasetTick> load_aggr_trades_parquet(
    const std::string& base_path, long long start_ts_ms, long long end_ts_ms) {
    const std::string parquets_path = base_path + "/parquets";
    const std::string& scan_path =
        fs::exists(parquets_path) ? parquets_path : base_path;

    if (!fs::exists(scan_path)) {
        std::cerr << "Aggr trades directory does not exist: " << scan_path
                  << std::endl;
        return {};
    }

    arrow::MemoryPool* pool = arrow::default_memory_pool();

    struct FileEntry {
        fs::path path;
    };
    std::vector<FileEntry> files;

    for (const auto& entry : fs::recursive_directory_iterator(scan_path)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".parquet")
            continue;
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

        files.push_back({entry.path()});
    }

    if (files.empty()) return {};

    std::vector<std::vector<AggrTradeDatasetTick>> per_file_ticks(files.size());
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

                std::vector<int> col_indices;
                for (const auto& col : {"price", "qty", "timestamp",
                                        "is_buyer_maker", "id"}) {
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

                auto price_col = std::static_pointer_cast<arrow::DoubleArray>(
                    tbl->GetColumnByName("price")->chunk(0));
                auto qty_col = std::static_pointer_cast<arrow::DoubleArray>(
                    tbl->GetColumnByName("qty")->chunk(0));
                auto ts_col = std::static_pointer_cast<arrow::Int64Array>(
                    tbl->GetColumnByName("timestamp")->chunk(0));
                auto buyer_col = std::static_pointer_cast<arrow::BooleanArray>(
                    tbl->GetColumnByName("is_buyer_maker")->chunk(0));

                // id column may be string or int
                auto id_arr = tbl->GetColumnByName("id");
                bool has_id = id_arr != nullptr;

                std::vector<AggrTradeDatasetTick> filtered;
                filtered.reserve(tbl->num_rows());

                for (int64_t r = 0; r < tbl->num_rows(); ++r) {
                    long long ts = ts_col->Value(r);
                    if (ts < start_ts_ms || ts > end_ts_ms) continue;

                    AggrTradeDatasetTick tick;
                    tick.timestamp = ts;
                    tick.price = price_col->Value(r);
                    tick.quantity = qty_col->Value(r);
                    tick.is_buyer_maker = buyer_col->Value(r);
                    tick.trade_id = 0;
                    if (has_id) {
                        auto chunk = id_arr->chunk(0);
                        if (chunk->type_id() == arrow::Type::INT64) {
                            tick.trade_id =
                                std::static_pointer_cast<arrow::Int64Array>(
                                    chunk)
                                    ->Value(r);
                        } else if (chunk->type_id() == arrow::Type::STRING) {
                            try {
                                tick.trade_id = std::stoll(
                                    std::static_pointer_cast<
                                        arrow::StringArray>(chunk)
                                        ->GetString(r));
                            } catch (...) {
                            }
                        }
                    }
                    filtered.push_back(tick);
                }
                per_file_ticks[i] = std::move(filtered);

                reader.reset();
                (void)infile.ValueOrDie()->Close();
            } catch (const std::exception& e) {
                std::cerr << "Exception processing " << files[i].path << ": "
                          << e.what() << '\n';
            }
        }));
    }

    for (auto& f : futures) f.get();

    size_t total = 0;
    for (const auto& v : per_file_ticks) total += v.size();

    std::vector<AggrTradeDatasetTick> result;
    result.reserve(total);
    for (auto& v : per_file_ticks) {
        result.insert(result.end(), std::make_move_iterator(v.begin()),
                      std::make_move_iterator(v.end()));
    }
    return result;
}

std::vector<MarketData::TimestampedValue> load_timestamped_values(
    const std::string& base_path, const std::string& value_column,
    long long start_ts_ms, long long end_ts_ms) {
    const std::string parquets_path = base_path + "/parquets";
    const std::string& scan_path =
        fs::exists(parquets_path) ? parquets_path : base_path;
    if (!fs::exists(scan_path)) return {};


    std::vector<fs::path> files;
    for (const auto& entry : fs::recursive_directory_iterator(scan_path)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".parquet")
            continue;
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

    arrow::MemoryPool* pool = arrow::default_memory_pool();
    std::vector<MarketData::TimestampedValue> result;

    for (const auto& path : files) {
        try {
            auto infile = arrow::io::ReadableFile::Open(path.string());
            if (!infile.ok()) continue;

            auto reader_result =
                parquet::arrow::OpenFile(infile.ValueOrDie(), pool);
            if (!reader_result.ok()) continue;
            auto reader = std::move(reader_result.ValueOrDie());

            std::shared_ptr<arrow::Table> tbl;
            if (!reader->ReadTable(&tbl).ok() || !tbl || tbl->num_rows() == 0)
                continue;

            auto combined = tbl->CombineChunks();
            if (!combined.ok()) continue;
            tbl = combined.ValueOrDie();

            auto ts_col_field = tbl->GetColumnByName("timestamp");
            auto rate_col_field = tbl->GetColumnByName(value_column);
            if (!ts_col_field || !rate_col_field) continue;

            auto ts_col = std::static_pointer_cast<arrow::Int64Array>(
                ts_col_field->chunk(0));
            auto rate_col = std::static_pointer_cast<arrow::DoubleArray>(
                rate_col_field->chunk(0));

            for (int64_t r = 0; r < tbl->num_rows(); ++r) {
                const long long ts = ts_col->Value(r);
                if (ts < start_ts_ms || ts > end_ts_ms) continue;
                result.push_back({ts, rate_col->Value(r)});
            }

            reader.reset();
            (void)infile.ValueOrDie()->Close();
        } catch (const std::exception& e) {
            std::cerr << "Exception processing timestamped parquet " << path << ": "
                      << e.what() << '\n';
        }
    }

    // Files are visited in directory order; callers rely on ascending time.
    std::sort(result.begin(), result.end(),
              [](const MarketData::TimestampedValue& a,
                 const MarketData::TimestampedValue& b) {
                  return a.timestamp_ms < b.timestamp_ms;
              });
    return result;
}

std::vector<HistoricalFundingRateTick> load_funding_rate_ticks(
    const std::string& base_path, long long start_ts_ms, long long end_ts_ms) {
    std::vector<HistoricalFundingRateTick> ticks;
    for (const auto& row : load_timestamped_values(base_path, "funding_rate",
                                                   start_ts_ms, end_ts_ms)) {
        ticks.push_back({row.timestamp_ms, row.value});
    }
    return ticks;
}

}  // namespace ParquetUtils

namespace ParquetUtils {

// Polars writes utf8 columns as arrow large_string (64-bit offsets), not
// string (32-bit). static_pointer_cast to StringArray reads that 64-bit offset
// buffer as 32-bit: sometimes it throws basic_string::_M_create, sometimes it
// silently yields a garbage string. Dispatch on the actual type instead.
std::string arrow_str_at(const std::shared_ptr<arrow::Array>& arr, int64_t r) {
    if (!arr || arr->IsNull(r)) return "";
    switch (arr->type_id()) {
        case arrow::Type::STRING:
            return std::static_pointer_cast<arrow::StringArray>(arr)->GetString(r);
        case arrow::Type::LARGE_STRING:
            return std::static_pointer_cast<arrow::LargeStringArray>(arr)
                ->GetString(r);
        default:
            return "";
    }
}

std::vector<MarketData::SecFiling> load_sec_filings(const std::string& base_path,
                                                    long long start_ts_ms,
                                                    long long end_ts_ms) {
    namespace fs = std::filesystem;
    const std::string parquets_path = base_path + "/parquets";
    const std::string& scan_path =
        fs::exists(parquets_path) ? parquets_path : base_path;
    if (!fs::exists(scan_path)) return {};

    std::vector<fs::path> files;
    for (const auto& entry : fs::recursive_directory_iterator(scan_path)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".parquet")
            continue;
        if (fs::file_size(entry.path()) == 0) continue;
        long long file_start_ms = 0;
        long long file_span_ms = 0;
        // Year-only partitions: parse_partition_window handles them, which it
        // did not before this tree existed.
        if (!ParquetUtils::parse_partition_window(entry.path().string(),
                                                  file_start_ms, file_span_ms))
            continue;
        if (file_start_ms + file_span_ms - 1 < start_ts_ms) continue;
        if (file_start_ms > end_ts_ms) continue;
        files.push_back(entry.path());
    }
    if (files.empty()) return {};

    arrow::MemoryPool* pool = arrow::default_memory_pool();
    std::vector<MarketData::SecFiling> result;

    for (const auto& path : files) {
        try {
            auto infile = arrow::io::ReadableFile::Open(path.string());
            if (!infile.ok()) continue;
            auto reader_result =
                parquet::arrow::OpenFile(infile.ValueOrDie(), pool);
            if (!reader_result.ok()) continue;
            auto reader = std::move(reader_result.ValueOrDie());

            std::shared_ptr<arrow::Table> tbl;
            if (!reader->ReadTable(&tbl).ok() || !tbl || tbl->num_rows() == 0)
                continue;
            auto combined = tbl->CombineChunks();
            if (!combined.ok()) continue;
            tbl = combined.ValueOrDie();

            auto cik_f = tbl->GetColumnByName("cik");
            auto accn_f = tbl->GetColumnByName("accn");
            auto form_f = tbl->GetColumnByName("form");
            auto ts_f = tbl->GetColumnByName("filed_ts_ms");
            if (!cik_f || !accn_f || !form_f || !ts_f) continue;
            // Present only in the material_events tree.
            auto item_f = tbl->GetColumnByName("item_code");

            auto cik_c = cik_f->chunk(0);
            auto accn_c = accn_f->chunk(0);
            auto form_c = form_f->chunk(0);
            if (ts_f->chunk(0)->type_id() != arrow::Type::INT64) continue;
            auto ts_c =
                std::static_pointer_cast<arrow::Int64Array>(ts_f->chunk(0));
            std::shared_ptr<arrow::Array> item_c;
            if (item_f) item_c = item_f->chunk(0);

            for (int64_t r = 0; r < tbl->num_rows(); ++r) {
                if (ts_c->IsNull(r)) continue;
                const long long ts = ts_c->Value(r);
                if (ts < start_ts_ms || ts > end_ts_ms) continue;
                MarketData::SecFiling f;
                f.cik = arrow_str_at(cik_c, r);
                f.accn = arrow_str_at(accn_c, r);
                f.form = arrow_str_at(form_c, r);
                f.item_code = arrow_str_at(item_c, r);
                f.filed_ts_ms = ts;
                f.observable_ts_ms =
                    ts + MarketData::SecLatency::pad_for_form(f.form);
                result.push_back(std::move(f));
            }
            reader.reset();
            (void)infile.ValueOrDie()->Close();
        } catch (const std::exception& e) {
            std::cerr << "Exception processing SEC parquet " << path << ": "
                      << e.what() << '\n';
        }
    }
    return result;
}

}  // namespace ParquetUtils

namespace ParquetUtils {

std::vector<MarketData::SymbolCikRow> load_symbol_cik_map(
    const std::string& path) {
    namespace fs = std::filesystem;
    if (!fs::exists(path)) return {};
    std::vector<MarketData::SymbolCikRow> out;
    try {
        auto infile = arrow::io::ReadableFile::Open(path);
        if (!infile.ok()) return {};
        auto reader_result = parquet::arrow::OpenFile(
            infile.ValueOrDie(), arrow::default_memory_pool());
        if (!reader_result.ok()) return {};
        auto reader = std::move(reader_result.ValueOrDie());
        std::shared_ptr<arrow::Table> tbl;
        if (!reader->ReadTable(&tbl).ok() || !tbl || tbl->num_rows() == 0)
            return {};
        auto combined = tbl->CombineChunks();
        if (!combined.ok()) return {};
        tbl = combined.ValueOrDie();

        auto sym_f = tbl->GetColumnByName("symbol");
        auto cik_f = tbl->GetColumnByName("cik");
        auto from_f = tbl->GetColumnByName("valid_from_ms");
        auto to_f = tbl->GetColumnByName("valid_to_ms");
        if (!sym_f || !cik_f || !from_f || !to_f) return {};
        auto sym_c = sym_f->chunk(0);
        auto cik_c = cik_f->chunk(0);
        auto from_c =
            std::static_pointer_cast<arrow::Int64Array>(from_f->chunk(0));
        auto to_c = std::static_pointer_cast<arrow::Int64Array>(to_f->chunk(0));

        out.reserve(tbl->num_rows());
        for (int64_t r = 0; r < tbl->num_rows(); ++r) {
            MarketData::SymbolCikRow row;
            row.symbol = arrow_str_at(sym_c, r);
            row.cik = arrow_str_at(cik_c, r);
            row.valid_from_ms = from_c->IsNull(r) ? 0 : from_c->Value(r);
            row.valid_to_ms = to_c->IsNull(r) ? 0 : to_c->Value(r);
            if (!row.symbol.empty() && !row.cik.empty())
                out.push_back(std::move(row));
        }
        reader.reset();
        (void)infile.ValueOrDie()->Close();
    } catch (const std::exception& e) {
        std::cerr << "Exception reading symbol_cik_map " << path << ": "
                  << e.what() << '\n';
    }
    return out;
}

std::vector<MarketData::StockSplitEvent> load_us_equity_stock_splits(
    const std::string& path) {
    namespace fs = std::filesystem;
    if (!fs::exists(path)) return {};
    std::vector<MarketData::StockSplitEvent> out;
    try {
        auto infile = arrow::io::ReadableFile::Open(path);
        if (!infile.ok()) return {};
        auto reader_result = parquet::arrow::OpenFile(
            infile.ValueOrDie(), arrow::default_memory_pool());
        if (!reader_result.ok()) return {};
        auto reader = std::move(reader_result.ValueOrDie());
        std::shared_ptr<arrow::Table> tbl;
        if (!reader->ReadTable(&tbl).ok() || !tbl || tbl->num_rows() == 0)
            return {};
        auto combined = tbl->CombineChunks();
        if (!combined.ok()) return {};
        tbl = combined.ValueOrDie();

        auto date_f = tbl->GetColumnByName("execution_date");
        auto from_f = tbl->GetColumnByName("split_from");
        auto to_f = tbl->GetColumnByName("split_to");
        auto ratio_f = tbl->GetColumnByName("ratio");
        if (!date_f || !from_f || !to_f || !ratio_f) return {};

        // execution_date is DATE32 -- days since epoch, not milliseconds.
        auto date_c =
            std::static_pointer_cast<arrow::Date32Array>(date_f->chunk(0));
        auto from_c =
            std::static_pointer_cast<arrow::Int64Array>(from_f->chunk(0));
        auto to_c = std::static_pointer_cast<arrow::Int64Array>(to_f->chunk(0));
        auto ratio_c =
            std::static_pointer_cast<arrow::DoubleArray>(ratio_f->chunk(0));

        out.reserve(tbl->num_rows());
        for (int64_t r = 0; r < tbl->num_rows(); ++r) {
            if (date_c->IsNull(r) || ratio_c->IsNull(r)) continue;
            MarketData::StockSplitEvent e;
            e.execution_ts_ms =
                static_cast<long long>(date_c->Value(r)) * 86400000LL;
            e.split_from = from_c->IsNull(r) ? 0 : from_c->Value(r);
            e.split_to = to_c->IsNull(r) ? 0 : to_c->Value(r);
            e.ratio = ratio_c->Value(r);
            // A zero or negative ratio would silently zero a market cap.
            if (e.ratio > 0.0) out.push_back(e);
        }
        std::sort(out.begin(), out.end(),
                  [](const MarketData::StockSplitEvent& a,
                     const MarketData::StockSplitEvent& b) {
                      return a.execution_ts_ms < b.execution_ts_ms;
                  });
        reader.reset();
        (void)infile.ValueOrDie()->Close();
    } catch (const std::exception& e) {
        std::cerr << "Exception reading splits " << path << ": " << e.what()
                  << '\n';
    }
    return out;
}

std::vector<MarketData::XbrlFact> load_us_equity_sec_facts(
    const std::string& cik_dir,
    const std::unordered_set<std::string>& tags) {
    namespace fs = std::filesystem;
    const std::string parquets = cik_dir + "/parquets";
    const std::string& scan = fs::exists(parquets) ? parquets : cik_dir;
    if (!fs::exists(scan)) return {};

    std::vector<MarketData::XbrlFact> out;
    arrow::MemoryPool* pool = arrow::default_memory_pool();
    for (const auto& entry : fs::recursive_directory_iterator(scan)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".parquet")
            continue;
        if (fs::file_size(entry.path()) == 0) continue;
        try {
            auto infile = arrow::io::ReadableFile::Open(entry.path().string());
            if (!infile.ok()) continue;
            auto reader_result =
                parquet::arrow::OpenFile(infile.ValueOrDie(), pool);
            if (!reader_result.ok()) continue;
            auto reader = std::move(reader_result.ValueOrDie());
            std::shared_ptr<arrow::Table> tbl;
            if (!reader->ReadTable(&tbl).ok() || !tbl || tbl->num_rows() == 0)
                continue;
            auto combined = tbl->CombineChunks();
            if (!combined.ok()) continue;
            tbl = combined.ValueOrDie();

            auto tax_f = tbl->GetColumnByName("taxonomy");
            auto tag_f = tbl->GetColumnByName("tag");
            auto unit_f = tbl->GetColumnByName("unit");
            auto pstart_f = tbl->GetColumnByName("period_start");
            auto pend_f = tbl->GetColumnByName("period_end");
            auto filed_f = tbl->GetColumnByName("filed_ts_ms");
            auto value_f = tbl->GetColumnByName("value_num");
            if (!tag_f || !filed_f || !value_f || !pend_f) continue;

            auto tag_c = tag_f->chunk(0);
            auto tax_c = tax_f ? tax_f->chunk(0) : nullptr;
            auto unit_c = unit_f ? unit_f->chunk(0) : nullptr;
            auto filed_c =
                std::static_pointer_cast<arrow::Int64Array>(filed_f->chunk(0));
            auto value_c =
                std::static_pointer_cast<arrow::DoubleArray>(value_f->chunk(0));
            // period_start / period_end are DATE32 -- days, not milliseconds.
            auto pend_c =
                std::static_pointer_cast<arrow::Date32Array>(pend_f->chunk(0));
            auto pstart_c =
                pstart_f ? std::static_pointer_cast<arrow::Date32Array>(
                               pstart_f->chunk(0))
                         : nullptr;

            for (int64_t r = 0; r < tbl->num_rows(); ++r) {
                const std::string tag = arrow_str_at(tag_c, r);
                if (!tags.empty() && tags.count(tag) == 0) continue;
                if (filed_c->IsNull(r) || value_c->IsNull(r)) continue;
                MarketData::XbrlFact fact;
                fact.tag = tag;
                fact.taxonomy = tax_c ? arrow_str_at(tax_c, r) : std::string();
                fact.unit = unit_c ? arrow_str_at(unit_c, r) : std::string();
                fact.filed_ts_ms = filed_c->Value(r);
                fact.value = value_c->Value(r);
                fact.period_end_ms =
                    pend_c->IsNull(r)
                        ? 0
                        : static_cast<long long>(pend_c->Value(r)) * 86400000LL;
                fact.period_start_ms =
                    (pstart_c == nullptr || pstart_c->IsNull(r))
                        ? 0
                        : static_cast<long long>(pstart_c->Value(r)) *
                              86400000LL;
                out.push_back(std::move(fact));
            }
            reader.reset();
            (void)infile.ValueOrDie()->Close();
        } catch (const std::exception& e) {
            std::cerr << "Exception reading sec facts " << entry.path().string()
                      << ": " << e.what() << '\n';
        }
    }
    std::sort(out.begin(), out.end(),
              [](const MarketData::XbrlFact& a, const MarketData::XbrlFact& b) {
                  return a.filed_ts_ms < b.filed_ts_ms;
              });
    return out;
}

}  // namespace ParquetUtils
