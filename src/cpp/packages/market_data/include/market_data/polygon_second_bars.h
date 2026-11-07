#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace MarketData {

// One Polygon per-second aggregate ("A" event), prices as traded: split
// adjustment happens when the bars are read, never on disk.
struct SecondBar {
    long long open_time_ms = 0;
    double open = 0;
    double high = 0;
    double low = 0;
    double close = 0;
    double volume = 0;
    double vwap = 0;
    std::int64_t trades = 0;
};

static_assert(sizeof(SecondBar) == 64,
              "SecondBar is sent as raw 64-byte little-endian records");

struct SymbolSecondBars {
    std::string symbol;
    std::vector<SecondBar> bars;
};

using SecondBarsBySymbol = std::unordered_map<std::string, std::vector<SecondBar>>;

struct SecondBarsSince {
    // False when bars after the requested sequence were already trimmed: the
    // reader must take a fresh snapshot.
    bool complete = true;
    std::uint64_t latest_sequence = 0;
    std::vector<std::pair<std::string, SecondBar>> bars;
};

// Every symbol's 1s bars for the latest sessions. Written by the ohlcv
// consumer thread, read by the hourly writer and the serving endpoints.
class PolygonSecondBarStore {
   public:
    static PolygonSecondBarStore& get_instance();

    // US equities trade 04:00-20:00 ET, which is 08:00-01:00 UTC at the
    // latest; shifting by 6h puts every session inside one day number.
    static long long session_day(long long time_ms);
    static std::string hour_file_path(const std::string& data_dir_root,
                                      long long hour_start_ms);

    void add(std::vector<std::pair<std::string, SecondBar>>&& bars);
    // Bars already held win over merged ones; used to load files on startup
    // without holding the lock while reading them.
    void merge(SecondBarsBySymbol&& loaded);
    [[nodiscard]] std::vector<SymbolSecondBars> snapshot(
        const std::vector<std::string>& symbols, long long from_ms) const;
    // Every symbol with bars in [hour_start_ms, +1h), sorted by symbol.
    [[nodiscard]] std::vector<SymbolSecondBars> hour(long long hour_start_ms) const;
    // Bars added after `sequence`, in arrival order, for live subscribers.
    [[nodiscard]] SecondBarsSince since(std::uint64_t sequence) const;
    void keep_latest_sessions(int sessions);
    struct Stats {
        std::size_t symbols = 0;
        std::size_t bars = 0;
        std::size_t sessions = 0;
    };
    [[nodiscard]] Stats stats() const;
    void clear();

    static bool write_hour_file(const std::string& path,
                                const std::vector<SymbolSecondBars>& bars,
                                std::string_view source);
    static bool read_hour_file(const std::string& path, SecondBarsBySymbol& out);
    // Wire formats of the recorder's /seconds routes; bars are raw records.
    static std::string encode_snapshot(const std::vector<SymbolSecondBars>& bars);
    static std::string encode_since(const SecondBarsSince& since);

   private:
    static void insert_sorted(std::vector<SecondBar>& bars, const SecondBar& bar);
    void count_day(long long time_ms);

    mutable std::shared_mutex mutex_;
    SecondBarsBySymbol bars_;
    std::map<long long, std::size_t> bars_per_session_;
    struct LiveBatch {
        std::uint64_t sequence = 0;
        std::vector<std::pair<std::string, SecondBar>> bars;
    };
    std::deque<LiveBatch> live_;
    std::size_t live_bars_ = 0;
    std::uint64_t latest_sequence_ = 0;
};

}  // namespace MarketData
