#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

// Request rate governor keyed by "<exchange>:<market_type>".
//
//  - Reactive cooldown (always on): after a 429/418, record_limit() parks the
//    key; is_limited() reports it so callers bail.
//  - Proactive pacing (opt-in): should_throttle() is a non-blocking test-and-
//    take over a per-key token bucket. It keeps the COMBINED request rate of
//    every caller sharing a key under the IP budget, and when multiple request
//    classes contend it alternates grants (work-conserving fair round-robin);
//    a lone active class gets the whole budget. The limiter never blocks — the
//    caller owns any wait/backoff — so no condition_variables live here.
class RateLimiter {
   public:
    using OnLimitCallback = std::function<void(const std::string&, int)>;

    // Identifies the call site so the paced scheduler can fairly interleave
    // sources sharing one key's budget.
    enum class RequestClass { SM_SYNC = 0, BAR_BACKFILL = 1, OTHER = 2 };
    static constexpr int kNumRequestClasses = 3;

    static RateLimiter& instance() {
        static RateLimiter inst;
        return inst;
    }

    // Reactive cooldown check — pure.
    bool is_limited(const std::string& key) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = limits_.find(key);
        if (it == limits_.end()) return false;
        return std::chrono::steady_clock::now() < it->second;
    }

    // Proactive paced gate (non-blocking test-and-take). Returns true if the
    // caller should NOT send right now — no token available, or another active
    // request class holds the round-robin turn. Returns false AND consumes one
    // token when a slot is granted. Does not consult the reactive cooldown;
    // call is_limited() separately to decide bail-vs-wait.
    bool should_throttle(const std::string& key, RequestClass cls) {
        std::lock_guard<std::mutex> lock(mutex_);
        Bucket& b = bucket_for(key);
        int c = static_cast<int>(cls);
        auto now = std::chrono::steady_clock::now();
        b.last_seen[c] = now;
        refill(b, now);
        if (b.tokens >= 1.0 && current_turn(b, now) == c) {
            b.tokens -= 1.0;
            b.rr_cursor = (c + 1) % kNumRequestClasses;
            return false;  // granted
        }
        return true;  // throttled — caller should back off and retry
    }

    void record_limit(const std::string& key, int retry_after_s) {
        OnLimitCallback callback;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto now = std::chrono::steady_clock::now();
            limits_[key] = now + std::chrono::seconds(retry_after_s);
            // Drain the pacer so we don't burst straight back into the limit
            // when the reactive cooldown lifts.
            auto it = buckets_.find(key);
            if (it != buckets_.end()) {
                it->second.tokens = 0.0;
                it->second.last_refill = now;
            }
            callback = on_limit_callback_;
        }
        if (callback) callback(key, retry_after_s);
    }

    void set_on_limit_callback(OnLimitCallback callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        on_limit_callback_ = std::move(callback);
    }

    int remaining_s(const std::string& key) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = limits_.find(key);
        if (it == limits_.end()) return 0;
        auto diff = it->second - std::chrono::steady_clock::now();
        int secs = std::chrono::duration_cast<std::chrono::seconds>(diff).count();
        return secs > 0 ? secs : 0;
    }

    // Config/test hook: sustained rate (req/s) and burst (max tokens) for a key.
    void set_pacing(const std::string& key, double rate_per_sec, double burst) {
        std::lock_guard<std::mutex> lock(mutex_);
        Bucket& b = bucket_for(key);
        b.rate_per_sec = rate_per_sec;
        b.burst = burst;
        b.tokens = std::min(b.tokens, burst);
    }

   private:
    RateLimiter() = default;

    struct Bucket {
        double tokens = 0.0;
        double rate_per_sec = 0.0;
        double burst = 0.0;
        std::chrono::steady_clock::time_point last_refill{};
        std::array<std::chrono::steady_clock::time_point, kNumRequestClasses>
            last_seen{};
        int rr_cursor = 0;
    };

    // binance fapi/api budget is 2400/min; 30/s leaves headroom.
    static constexpr double kBinanceRate = 30.0;
    // Polygon's paid plans do not meter request count, and an equity backfill
    // spans ~12.5k symbols, so the default pacing is the binding constraint
    // there rather than a protection.
    static constexpr double kPolygonRate = 100.0;
    static constexpr double kDefaultRate = 20.0;
    static constexpr double kBurst = 5.0;
    // A class counts as "contending" only if it polled within this window, so
    // an idle class yields its round-robin turn to an active one after it.
    static constexpr auto kActiveWindow = std::chrono::milliseconds(250);

    Bucket& bucket_for(const std::string& key) {  // caller holds mutex_
        auto it = buckets_.find(key);
        if (it != buckets_.end()) return it->second;
        Bucket& b = buckets_[key];
        b.rate_per_sec = (key.rfind("binance:", 0) == 0)   ? kBinanceRate
                         : (key.rfind("polygon:", 0) == 0) ? kPolygonRate
                                                           : kDefaultRate;
        b.burst = kBurst;
        b.tokens = b.burst;
        b.last_refill = std::chrono::steady_clock::now();
        return b;
    }

    static void refill(Bucket& b, std::chrono::steady_clock::time_point now) {
        double elapsed =
            std::chrono::duration<double>(now - b.last_refill).count();
        if (elapsed <= 0.0) return;
        b.tokens = std::min(b.burst, b.tokens + elapsed * b.rate_per_sec);
        b.last_refill = now;
    }

    int current_turn(const Bucket& b,
                     std::chrono::steady_clock::time_point now) const {
        for (int i = 0; i < kNumRequestClasses; ++i) {
            int idx = (b.rr_cursor + i) % kNumRequestClasses;
            if (b.last_seen[idx].time_since_epoch().count() != 0 &&
                now - b.last_seen[idx] <= kActiveWindow) {
                return idx;
            }
        }
        return -1;
    }

    mutable std::mutex mutex_;
    OnLimitCallback on_limit_callback_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point>
        limits_;
    std::unordered_map<std::string, Bucket> buckets_;
};
