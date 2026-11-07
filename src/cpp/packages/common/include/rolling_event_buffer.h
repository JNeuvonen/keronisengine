#pragma once

#include <algorithm>
#include <vector>

// Generic time-windowed event buffer. Stores events with timestamps and
// prunes entries older than window_ms on read. Suitable for tracking
// recent fills, trades, signals, or any timestamped event.
//
// Not thread-safe — caller must synchronize if accessed from multiple threads.
template <typename T>
class RollingEventBuffer {
   public:
    explicit RollingEventBuffer(long long window_ms) : window_ms_(window_ms) {}

    void push(const T& event, long long timestamp_ms) {
        entries_.push_back({event, timestamp_ms});
    }

    // Returns events within window, prunes old ones.
    std::vector<T> get_recent(long long current_time_ms) {
        prune(current_time_ms);
        std::vector<T> result;
        result.reserve(entries_.size());
        for (auto& e : entries_) result.push_back(e.event);
        return result;
    }

    size_t size(long long current_time_ms) {
        prune(current_time_ms);
        return entries_.size();
    }

    // Count events within the trailing window_ms (must be <= the buffer's
    // retention window). O(log n): entries are timestamp-sorted, so binary-
    // search the cutoff and count the tail — no copy, no scan. Lets one buffer
    // serve multiple sub-window queries.
    size_t count_within(long long current_time_ms, long long window_ms) {
        prune(current_time_ms);
        long long cutoff = current_time_ms - window_ms;
        auto it = std::lower_bound(
            entries_.begin(), entries_.end(), cutoff,
            [](const Entry& e, long long t) { return e.timestamp_ms < t; });
        return static_cast<size_t>(entries_.end() - it);
    }

    bool empty(long long current_time_ms) {
        prune(current_time_ms);
        return entries_.empty();
    }

   private:
    struct Entry {
        T event;
        long long timestamp_ms;
    };

    void prune(long long current_time_ms) {
        long long cutoff = current_time_ms - window_ms_;
        auto it = std::lower_bound(
            entries_.begin(), entries_.end(), cutoff,
            [](const Entry& e, long long t) { return e.timestamp_ms < t; });
        if (it != entries_.begin()) entries_.erase(entries_.begin(), it);
    }

    std::vector<Entry> entries_;
    long long window_ms_;
};
