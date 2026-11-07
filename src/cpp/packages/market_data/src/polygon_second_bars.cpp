#include "market_data/polygon_second_bars.h"

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/reader.h>
#include <parquet/arrow/writer.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <mutex>

namespace MarketData {

namespace {

constexpr long long DAY_MS = 86'400'000LL;
// About three minutes of the busiest part of a session.
constexpr std::size_t LIVE_LOG_BARS = 2'000'000;
constexpr long long SESSION_SHIFT_MS = 6 * 3'600'000LL;

long long session_start_ms(long long day) {
    return day * DAY_MS + SESSION_SHIFT_MS;
}

bool by_time(const SecondBar& bar, long long time_ms) {
    return bar.open_time_ms < time_ms;
}

const std::int64_t* int64_values(const arrow::Array& array) {
    return array.data()->GetValues<std::int64_t>(1);
}

const double* double_values(const arrow::Array& array) {
    return array.data()->GetValues<double>(1);
}

std::string string_value(const arrow::Array& array, std::int64_t row) {
    if (array.type_id() == arrow::Type::LARGE_STRING)
        return static_cast<const arrow::LargeStringArray&>(array).GetString(row);
    return static_cast<const arrow::StringArray&>(array).GetString(row);
}

}  // namespace

PolygonSecondBarStore& PolygonSecondBarStore::get_instance() {
    static PolygonSecondBarStore instance;
    return instance;
}

long long PolygonSecondBarStore::session_day(long long time_ms) {
    const long long shifted = time_ms - SESSION_SHIFT_MS;
    return shifted >= 0 ? shifted / DAY_MS : (shifted - DAY_MS + 1) / DAY_MS;
}

std::string PolygonSecondBarStore::hour_file_path(
    const std::string& data_dir_root, long long hour_start_ms) {
    const std::time_t seconds = hour_start_ms / 1000;
    std::tm utc{};
    gmtime_r(&seconds, &utc);
    std::array<char, 48> partition{};
    std::snprintf(partition.data(), partition.size(),
                  "date=%04d-%02d-%02d/hour=%02d.parquet", utc.tm_year + 1900,
                  utc.tm_mon + 1, utc.tm_mday, utc.tm_hour);
    return data_dir_root +
           "/derived/bars/polygon/stock/exchange/1000_ms_hourly/" + partition.data();
}

void PolygonSecondBarStore::insert_sorted(std::vector<SecondBar>& bars,
                                          const SecondBar& bar) {
    if (bars.empty() || bars.back().open_time_ms < bar.open_time_ms) {
        bars.push_back(bar);
        return;
    }
    auto it = std::lower_bound(bars.begin(), bars.end(), bar.open_time_ms,
                               by_time);
    if (it != bars.end() && it->open_time_ms == bar.open_time_ms) {
        *it = bar;
        return;
    }
    bars.insert(it, bar);
}

void PolygonSecondBarStore::count_day(long long time_ms) {
    ++bars_per_session_[session_day(time_ms)];
}

void PolygonSecondBarStore::add(
    std::vector<std::pair<std::string, SecondBar>>&& bars) {
    std::unique_lock lock(mutex_);
    for (auto& [symbol, bar] : bars) {
        auto& held = bars_[symbol];
        const std::size_t before = held.size();
        insert_sorted(held, bar);
        if (held.size() != before) count_day(bar.open_time_ms);
    }
    live_bars_ += bars.size();
    live_.push_back({++latest_sequence_, std::move(bars)});
    while (live_bars_ > LIVE_LOG_BARS) {
        live_bars_ -= live_.front().bars.size();
        live_.pop_front();
    }
}

SecondBarsSince PolygonSecondBarStore::since(std::uint64_t sequence) const {
    std::shared_lock lock(mutex_);
    SecondBarsSince out;
    out.latest_sequence = latest_sequence_;
    if (sequence == latest_sequence_) return out;
    // Ahead of the store means the recorder restarted and numbering began again.
    if (sequence > latest_sequence_ || live_.empty() ||
        live_.front().sequence > sequence + 1) {
        out.complete = false;
        return out;
    }
    for (auto it = live_.begin() + static_cast<std::ptrdiff_t>(
                                       sequence + 1 - live_.front().sequence);
         it != live_.end(); ++it)
        out.bars.insert(out.bars.end(), it->bars.begin(), it->bars.end());
    return out;
}

void PolygonSecondBarStore::merge(SecondBarsBySymbol&& loaded) {
    std::unique_lock lock(mutex_);
    for (auto& [symbol, incoming] : loaded) {
        auto& held = bars_[symbol];
        std::vector<SecondBar> merged;
        merged.reserve(held.size() + incoming.size());
        auto a = held.begin();
        auto b = incoming.begin();
        while (a != held.end() || b != incoming.end()) {
            if (b == incoming.end() ||
                (a != held.end() && a->open_time_ms <= b->open_time_ms)) {
                if (b != incoming.end() && a->open_time_ms == b->open_time_ms)
                    ++b;
                merged.push_back(*a++);
            } else {
                merged.push_back(*b);
                count_day(b->open_time_ms);
                ++b;
            }
        }
        held = std::move(merged);
    }
}

std::vector<SymbolSecondBars> PolygonSecondBarStore::snapshot(
    const std::vector<std::string>& symbols, long long from_ms) const {
    std::shared_lock lock(mutex_);
    std::vector<SymbolSecondBars> out;
    for (const auto& symbol : symbols) {
        SymbolSecondBars entry{symbol, {}};
        if (auto it = bars_.find(symbol); it != bars_.end()) {
            auto first = std::lower_bound(it->second.begin(), it->second.end(),
                                          from_ms, by_time);
            entry.bars.assign(first, it->second.end());
        }
        out.push_back(std::move(entry));
    }
    return out;
}

std::vector<SymbolSecondBars> PolygonSecondBarStore::hour(
    long long hour_start_ms) const {
    const long long hour_end_ms = hour_start_ms + 3'600'000LL;
    std::vector<SymbolSecondBars> out;
    {
        std::shared_lock lock(mutex_);
        for (const auto& [symbol, bars] : bars_) {
            auto first = std::lower_bound(bars.begin(), bars.end(),
                                          hour_start_ms, by_time);
            auto last = std::lower_bound(first, bars.end(), hour_end_ms, by_time);
            if (first != last)
                out.push_back({symbol, std::vector<SecondBar>(first, last)});
        }
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return a.symbol < b.symbol;
    });
    return out;
}

void PolygonSecondBarStore::keep_latest_sessions(int sessions) {
    std::unique_lock lock(mutex_);
    if (static_cast<int>(bars_per_session_.size()) <= sessions) return;
    auto oldest_kept = bars_per_session_.end();
    std::advance(oldest_kept, -sessions);
    const long long cutoff_ms = session_start_ms(oldest_kept->first);
    bars_per_session_.erase(bars_per_session_.begin(), oldest_kept);
    for (auto it = bars_.begin(); it != bars_.end();) {
        auto& bars = it->second;
        bars.erase(bars.begin(), std::lower_bound(bars.begin(), bars.end(),
                                                  cutoff_ms, by_time));
        if (bars.empty()) {
            it = bars_.erase(it);
            continue;
        }
        bars.shrink_to_fit();
        ++it;
    }
}

PolygonSecondBarStore::Stats PolygonSecondBarStore::stats() const {
    std::shared_lock lock(mutex_);
    Stats stats;
    stats.symbols = bars_.size();
    for (const auto& [day, count] : bars_per_session_) stats.bars += count;
    stats.sessions = bars_per_session_.size();
    return stats;
}

void PolygonSecondBarStore::clear() {
    std::unique_lock lock(mutex_);
    bars_.clear();
    bars_per_session_.clear();
    live_.clear();
    live_bars_ = 0;
}

namespace {

template <typename T>
void put(std::string& out, T value) {
    out.append(reinterpret_cast<const char*>(&value), sizeof(value));
}

void put_symbol(std::string& out, const std::string& symbol) {
    put(out, static_cast<std::uint16_t>(symbol.size()));
    out += symbol;
}

}  // namespace

// [u32 symbols] then per symbol [u16 length][name][u32 bars][64-byte bars].
std::string PolygonSecondBarStore::encode_snapshot(
    const std::vector<SymbolSecondBars>& bars) {
    std::string out;
    put(out, static_cast<std::uint32_t>(bars.size()));
    for (const auto& entry : bars) {
        put_symbol(out, entry.symbol);
        put(out, static_cast<std::uint32_t>(entry.bars.size()));
        out.append(reinterpret_cast<const char*>(entry.bars.data()),
                   entry.bars.size() * sizeof(SecondBar));
    }
    return out;
}

// [u64 latest sequence][u8 complete][u32 bars] then per bar
// [u16 length][name][64-byte bar].
std::string PolygonSecondBarStore::encode_since(const SecondBarsSince& since) {
    std::string out;
    put(out, since.latest_sequence);
    put(out, static_cast<std::uint8_t>(since.complete));
    put(out, static_cast<std::uint32_t>(since.bars.size()));
    for (const auto& [symbol, bar] : since.bars) {
        put_symbol(out, symbol);
        put(out, bar);
    }
    return out;
}

bool PolygonSecondBarStore::write_hour_file(
    const std::string& path, const std::vector<SymbolSecondBars>& bars,
    std::string_view source) {
    arrow::StringBuilder symbol_b;
    arrow::TimestampBuilder time_b(arrow::timestamp(arrow::TimeUnit::MILLI),
                                   arrow::default_memory_pool());
    arrow::DoubleBuilder open_b, high_b, low_b, close_b, volume_b, vwap_b;
    arrow::Int64Builder trades_b;
    for (const auto& entry : bars) {
        for (const auto& bar : entry.bars) {
            if (!symbol_b.Append(entry.symbol).ok() ||
                !time_b.Append(bar.open_time_ms).ok() ||
                !open_b.Append(bar.open).ok() || !high_b.Append(bar.high).ok() ||
                !low_b.Append(bar.low).ok() || !close_b.Append(bar.close).ok() ||
                !volume_b.Append(bar.volume).ok() ||
                !vwap_b.Append(bar.vwap).ok() ||
                !trades_b.Append(bar.trades).ok())
                return false;
        }
    }
    std::vector<std::shared_ptr<arrow::Array>> columns(9);
    if (!symbol_b.Finish(&columns[0]).ok() || !time_b.Finish(&columns[1]).ok() ||
        !open_b.Finish(&columns[2]).ok() || !high_b.Finish(&columns[3]).ok() ||
        !low_b.Finish(&columns[4]).ok() || !close_b.Finish(&columns[5]).ok() ||
        !volume_b.Finish(&columns[6]).ok() || !vwap_b.Finish(&columns[7]).ok() ||
        !trades_b.Finish(&columns[8]).ok())
        return false;

    auto schema = arrow::schema(
        {arrow::field("symbol", arrow::utf8()),
         arrow::field("kline_open_time_ms",
                      arrow::timestamp(arrow::TimeUnit::MILLI)),
         arrow::field("open_price", arrow::float64()),
         arrow::field("high_price", arrow::float64()),
         arrow::field("low_price", arrow::float64()),
         arrow::field("close_price", arrow::float64()),
         arrow::field("volume", arrow::float64()),
         arrow::field("vwap", arrow::float64()),
         arrow::field("number_of_trades", arrow::int64())},
        arrow::key_value_metadata({"source"}, {std::string(source)}));
    auto table = arrow::Table::Make(schema, columns);

    namespace fs = std::filesystem;
    const fs::path final_path(path);
    const fs::path tmp_path = final_path.string() + ".tmp";
    std::error_code ec;
    fs::create_directories(final_path.parent_path(), ec);
    auto out = arrow::io::FileOutputStream::Open(tmp_path.string());
    if (!out.ok()) return false;
    auto props = parquet::WriterProperties::Builder()
                     .compression(parquet::Compression::ZSTD)
                     ->compression_level(4)
                     ->build();
    auto arrow_props =
        parquet::ArrowWriterProperties::Builder().store_schema()->build();
    // Row groups of 64k rows let a one-symbol read skip most of the file.
    if (!parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), *out,
                                    65'536, props, arrow_props)
             .ok() ||
        !(*out)->Close().ok()) {
        fs::remove(tmp_path, ec);
        return false;
    }
    fs::rename(tmp_path, final_path, ec);
    if (ec) {
        fs::remove(tmp_path, ec);
        return false;
    }
    return true;
}

bool PolygonSecondBarStore::read_hour_file(const std::string& path,
                                           SecondBarsBySymbol& out) {
    auto input = arrow::io::ReadableFile::Open(path);
    if (!input.ok()) return false;
    auto reader = parquet::arrow::OpenFile(*input, arrow::default_memory_pool());
    if (!reader.ok()) return false;
    std::shared_ptr<arrow::Table> table;
    if (!(*reader)->ReadTable(&table).ok()) return false;
    auto combined = table->CombineChunks();
    if (!combined.ok()) return false;
    table = *combined;

    auto column = [&](const char* name) -> std::shared_ptr<arrow::Array> {
        auto chunked = table->GetColumnByName(name);
        if (!chunked || chunked->num_chunks() != 1) return nullptr;
        auto array = chunked->chunk(0);
        return array->null_count() == 0 ? array : nullptr;
    };
    auto symbol = column("symbol");
    auto time = column("kline_open_time_ms");
    auto open = column("open_price");
    auto high = column("high_price");
    auto low = column("low_price");
    auto close = column("close_price");
    auto volume = column("volume");
    auto vwap = column("vwap");
    auto trades = column("number_of_trades");
    if (table->num_rows() == 0) return true;
    if (!symbol || !time || !open || !high || !low || !close || !volume ||
        !vwap || !trades)
        return false;
    if (symbol->type_id() != arrow::Type::STRING &&
        symbol->type_id() != arrow::Type::LARGE_STRING)
        return false;
    if ((time->type_id() != arrow::Type::TIMESTAMP &&
         time->type_id() != arrow::Type::INT64) ||
        trades->type_id() != arrow::Type::INT64)
        return false;
    for (const auto& prices : {open, high, low, close, volume, vwap})
        if (prices->type_id() != arrow::Type::DOUBLE) return false;

    const auto* t = int64_values(*time);
    const auto* o = double_values(*open);
    const auto* h = double_values(*high);
    const auto* l = double_values(*low);
    const auto* c = double_values(*close);
    const auto* v = double_values(*volume);
    const auto* w = double_values(*vwap);
    const auto* n = int64_values(*trades);
    std::string current;
    std::vector<SecondBar>* bars = nullptr;
    for (std::int64_t row = 0; row < table->num_rows(); ++row) {
        std::string name = string_value(*symbol, row);
        if (bars == nullptr || name != current) {
            current = std::move(name);
            bars = &out[current];
        }
        insert_sorted(*bars, {t[row], o[row], h[row], l[row], c[row], v[row],
                              w[row], n[row]});
    }
    return true;
}

}  // namespace MarketData
