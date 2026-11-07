#pragma once

#include <atomic>

#include "threadpool.h"

class ExclusivePeriodicJob {
    std::atomic<bool> running_{false};

   public:
    template <typename F>
    bool try_dispatch(ThreadPool& pool, F&& func) {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true,
                                              std::memory_order_acq_rel)) {
            return false;
        }
        pool.enqueue([this, f = std::forward<F>(func)]() {
            try {
                f();
            } catch (...) {
            }
            running_.store(false, std::memory_order_release);
        });
        return true;
    }
};
