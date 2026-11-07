#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>

namespace Core {

// Lock-free log-bucket latency approximation. Each bucket counts samples
// whose nanoseconds fall into [2^i, 2^(i+1)). With 63 buckets we cover 1 ns
// up to ~2^63-1 ns (far beyond any realistic latency); in practice the
// populated range is ~100 ns (1e2) to 10 s (1e10). p50/p95/p99 are derived
// from the bucket counts — accurate to within one power-of-two, enough to
// see regressions on an operator dashboard. Trade-off: one atomic increment
// per record() in the hot path, no reader contention, no allocation.
//
// Usage:
//   LatencyHistogram create_to_ack_hist;
//   auto start = std::chrono::steady_clock::now();
//   ... do the work ...
//   auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
//                      std::chrono::steady_clock::now() - start).count();
//   create_to_ack_hist.record(elapsed);
//
//   // In periodic metrics emit:
//   auto snap = create_to_ack_hist.snapshot_and_reset();
//   metadata["latency_order_sent_to_ack_ns"] = snap;
//
// Designed to be held as a value member on Engine; no ownership semantics.
class LatencyHistogram {
   public:
    // 63 buckets: index i covers [2^i, 2^(i+1)). Max index = 62, so the
    // largest bucket's upper edge is 2^63 - 1 = INT64_MAX, avoiding UB from
    // shifting by 64.
    static constexpr size_t kNumBuckets = 63;

    LatencyHistogram() { clear(); }

    LatencyHistogram(const LatencyHistogram&) = delete;
    LatencyHistogram& operator=(const LatencyHistogram&) = delete;

    void record(int64_t ns) {
        if (ns <= 0) {
            buckets_[0].fetch_add(1, std::memory_order_relaxed);
            count_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        size_t idx = 0;
        int64_t v = ns;
        while (v > 1 && idx < kNumBuckets - 1) {
            v >>= 1;
            ++idx;
        }
        buckets_[idx].fetch_add(1, std::memory_order_relaxed);
        count_.fetch_add(1, std::memory_order_relaxed);
        update_max(ns);
    }

    // Snapshot + reset for periodic emission. Not strictly atomic across
    // buckets — a record() racing with snapshot_and_reset() may land in
    // either the old or the new window. Acceptable for periodic metrics.
    nlohmann::json snapshot_and_reset() {
        std::array<uint64_t, kNumBuckets> counts{};
        uint64_t total = 0;
        for (size_t i = 0; i < kNumBuckets; ++i) {
            counts[i] = buckets_[i].exchange(0, std::memory_order_acq_rel);
            total += counts[i];
        }
        const int64_t max_ns =
            max_ns_.exchange(0, std::memory_order_acq_rel);
        count_.store(0, std::memory_order_release);

        nlohmann::json out;
        out["count"] = total;
        out["max"] = max_ns;
        if (total == 0) {
            out["p50"] = 0;
            out["p95"] = 0;
            out["p99"] = 0;
            return out;
        }
        out["p50"] = percentile_ns(counts, total, 0.50);
        out["p95"] = percentile_ns(counts, total, 0.95);
        out["p99"] = percentile_ns(counts, total, 0.99);
        return out;
    }

    // Non-destructive read (for tests / diagnostics). Not used in prod flush.
    [[nodiscard]] uint64_t total_count() const {
        return count_.load(std::memory_order_relaxed);
    }

    void clear() {
        for (auto& b : buckets_) {
            b.store(0, std::memory_order_relaxed);
        }
        count_.store(0, std::memory_order_relaxed);
        max_ns_.store(0, std::memory_order_relaxed);
    }

   private:
    static int64_t percentile_ns(
        const std::array<uint64_t, kNumBuckets>& counts, uint64_t total,
        double p) {
        const auto target = static_cast<uint64_t>(p * static_cast<double>(total));
        uint64_t cumulative = 0;
        for (size_t i = 0; i < kNumBuckets; ++i) {
            cumulative += counts[i];
            if (cumulative >= target + 1) {
                // Bucket i covers [2^i, 2^(i+1)). Report upper edge so we
                // don't underreport. For i=kNumBuckets-1=62 this is
                // 2^63 - 1 = INT64_MAX.
                return (static_cast<int64_t>(1) << (i + 1)) - 1;
            }
        }
        return std::numeric_limits<int64_t>::max();
    }

    void update_max(int64_t ns) {
        int64_t cur = max_ns_.load(std::memory_order_relaxed);
        while (ns > cur && !max_ns_.compare_exchange_weak(
                               cur, ns, std::memory_order_release,
                               std::memory_order_relaxed)) {
            // spin on CAS
        }
    }

    std::array<std::atomic<uint64_t>, kNumBuckets> buckets_{};
    std::atomic<uint64_t> count_{0};
    std::atomic<int64_t> max_ns_{0};
};

}  // namespace Core
