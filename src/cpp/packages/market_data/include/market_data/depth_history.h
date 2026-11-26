#pragma once

#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace MarketData {

struct DepthLevel {
    double price = 0;
    double quantity = 0;
};

// Top of a venue book as this process saw it at `received_ms` (our clock).
// An invalid snapshot marks the moment the book stopped being trustworthy
// (a sequence gap or reconnect) until the next valid one.
struct DepthSnapshot {
    long long received_ms = 0;
    long long exchange_ms = 0;
    long long sequence = 0;
    bool is_valid = true;
    std::vector<DepthLevel> bids;  // best first
    std::vector<DepthLevel> asks;  // best first
};

// Recent book snapshots per symbol key (`SYMBOL:exchange:market`), so a
// fill can be judged against the book as it stood at an earlier moment.
class DepthHistory {
   public:
    static constexpr size_t MAX_LEVELS = 50;
    static constexpr long long RETENTION_MS = 30000;

    static DepthHistory &instance();

    void publish(const std::string &key, DepthSnapshot snapshot);
    void mark_invalid(const std::string &key, long long received_ms);

    // Latest snapshot received at or before `time_ms`.
    [[nodiscard]] std::optional<DepthSnapshot> at(const std::string &key,
                                                  long long time_ms) const;

    void clear();

   private:
    struct Series {
        mutable std::mutex mutex;
        std::deque<DepthSnapshot> snapshots;
    };
    Series &series(const std::string &key);

    mutable std::shared_mutex map_mutex_;
    std::unordered_map<std::string, std::unique_ptr<Series>> series_;
};

}  // namespace MarketData
