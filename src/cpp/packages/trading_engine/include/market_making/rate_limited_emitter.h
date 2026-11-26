#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace Core {

// Per-key cooldown emitter for high-frequency log events. See
// Rate-limits high-frequency log events that can fire
// hundreds of times per second (min-notional rejections, orderbook sync
// warnings, late fills, etc.) must be rate-limited at the emit site so the
// DB log table and operator dashboard stay signal-rich.
//
// Usage:
//   static Core::RateLimitedEmitter emitter;
//   emitter.emit_if_allowed(
//       "ORDER_REJECTED_MIN_NOTIONAL|" + symbol + "|" + std::to_string(sid),
//       LOG_COOLDOWN_ORDER_REJECTED_MIN_NOTIONAL_MS,
//       [&](uint64_t suppressed_count) {
//           nlohmann::json md;
//           md["events_suppressed_since_last_emit"] = suppressed_count;
//           ... populate required schema keys ...
//           engine.create_system_log(symbol, LOG_LEVEL_WARNING, msg,
//               TRADING_LOG_EVENT_TYPE_ORDER_REJECTED_MIN_NOTIONAL, md, ...);
//       });
//
// Cooldown semantics: the first call for a given key fires its emitter
// immediately. Subsequent calls within cooldown_ms are counted (suppressed)
// but not emitted. The next call after cooldown fires the emitter with
// suppressed_count = #calls suppressed since the last successful emit.
//
// Thread-safety: readers take a shared lock to check the last-emit ts; on the
// rare write path (first emit for a key, or first emit after cooldown) an
// upgrade to an exclusive lock happens inside. Copy-under-lock, release,
// invoke — never invoke the emitter callback with the lock held, since the
// callback enqueues to the trading_logs queue which itself is lock-free but
// can grow.
class RateLimitedEmitter {
   public:
    RateLimitedEmitter() = default;
    RateLimitedEmitter(const RateLimitedEmitter&) = delete;
    RateLimitedEmitter& operator=(const RateLimitedEmitter&) = delete;

    void emit_if_allowed(
        const std::string& key, long long cooldown_ms,
        const std::function<void(uint64_t suppressed_count)>& emit_fn) {
        const int64_t now_ms = now_epoch_ms();

        uint64_t suppressed = 0;
        bool should_emit = false;

        {
            std::unique_lock<std::shared_mutex> lock(mutex_);
            auto it = state_.find(key);
            if (it == state_.end()) {
                state_.emplace(key, State{now_ms, 0});
                should_emit = true;
            } else {
                const int64_t since_last = now_ms - it->second.last_emit_ms;
                if (since_last >= cooldown_ms) {
                    suppressed = it->second.suppressed_since_last_emit;
                    it->second.last_emit_ms = now_ms;
                    it->second.suppressed_since_last_emit = 0;
                    should_emit = true;
                } else {
                    it->second.suppressed_since_last_emit += 1;
                }
            }
        }

        if (should_emit) {
            emit_fn(suppressed);
        }
    }

    // Testing / diagnostics: peek at how many suppressions are pending for a
    // key without emitting. Not used in production code paths.
    uint64_t peek_suppressed(const std::string& key) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = state_.find(key);
        return it == state_.end() ? 0 : it->second.suppressed_since_last_emit;
    }

    // Testing: clear all state. Not used in production.
    void reset() {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        state_.clear();
    }

   private:
    struct State {
        int64_t last_emit_ms;
        uint64_t suppressed_since_last_emit;
    };

    static int64_t now_epoch_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, State> state_;
};

}  // namespace Core
