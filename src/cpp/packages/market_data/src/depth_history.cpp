#include "market_data/depth_history.h"

#include <algorithm>
#include <iterator>

namespace MarketData {

DepthHistory &DepthHistory::instance() {
    static DepthHistory history;
    return history;
}

DepthHistory::Series &DepthHistory::series(const std::string &key) {
    {
        std::shared_lock lock(map_mutex_);
        auto it = series_.find(key);
        if (it != series_.end()) return *it->second;
    }
    std::unique_lock lock(map_mutex_);
    auto &slot = series_[key];
    if (!slot) slot = std::make_unique<Series>();
    return *slot;
}

void DepthHistory::publish(const std::string &key, DepthSnapshot snapshot) {
    auto &s = series(key);
    const long long cutoff = snapshot.received_ms - RETENTION_MS;
    std::lock_guard lock(s.mutex);
    // Out-of-order receipt (clock step) would break the binary search.
    if (!s.snapshots.empty() &&
        snapshot.received_ms < s.snapshots.back().received_ms) {
        snapshot.received_ms = s.snapshots.back().received_ms;
    }
    s.snapshots.push_back(std::move(snapshot));
    while (s.snapshots.size() > 1 && s.snapshots.front().received_ms < cutoff)
        s.snapshots.pop_front();
}

void DepthHistory::mark_invalid(const std::string &key, long long received_ms) {
    DepthSnapshot marker;
    marker.received_ms = received_ms;
    marker.is_valid = false;
    publish(key, std::move(marker));
}

std::optional<DepthSnapshot> DepthHistory::at(const std::string &key,
                                              long long time_ms) const {
    const Series *s = nullptr;
    {
        std::shared_lock lock(map_mutex_);
        auto it = series_.find(key);
        if (it == series_.end()) return std::nullopt;
        s = it->second.get();
    }
    std::lock_guard lock(s->mutex);
    const auto &snapshots = s->snapshots;
    auto it = std::upper_bound(
        snapshots.begin(), snapshots.end(), time_ms,
        [](long long t, const DepthSnapshot &snap) { return t < snap.received_ms; });
    if (it == snapshots.begin()) return std::nullopt;
    return *std::prev(it);
}

void DepthHistory::clear() {
    std::unique_lock lock(map_mutex_);
    series_.clear();
}

}  // namespace MarketData
