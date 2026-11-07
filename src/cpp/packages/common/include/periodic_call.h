#pragma once

#include <chrono>
#include <utility>

namespace CommonUtils {

// Rate-limits a periodic action. Call tick(fn) repeatedly from a loop; fn
// runs when at least `interval` has elapsed since the last run, otherwise
// tick() is a noop. Drift-mode cooldown: next fire is measured from the
// previous fire's completion, not from a fixed schedule — a slow fn pushes
// the next fire back rather than causing catch-up bursts.
//
// First tick() call always fires (construction seeds last_fire_ one
// interval in the past).
class PeriodicCall {
   public:
    explicit PeriodicCall(std::chrono::milliseconds interval)
        : interval_(interval),
          last_fire_(std::chrono::steady_clock::now() - interval) {}

    template <typename F>
    bool tick(F&& fn) {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_fire_ < interval_) return false;
        std::forward<F>(fn)();
        last_fire_ = std::chrono::steady_clock::now();
        return true;
    }

   private:
    std::chrono::milliseconds interval_;
    std::chrono::steady_clock::time_point last_fire_;
};

}  // namespace CommonUtils
