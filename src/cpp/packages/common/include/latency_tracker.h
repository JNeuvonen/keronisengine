#pragma once

#include <cmath>
#include <iostream>
#include <string>

class LatencyTracker {
   public:
    LatencyTracker(std::string label, int warmup = 5)
        : label_(std::move(label)), warmup_(warmup) {}

    void track(long long latency_ms) {
        count_++;
        if (count_ > warmup_) {
            int n = count_ - warmup_;
            sum_ += latency_ms;
            sum_sq_ += latency_ms * latency_ms;
            if (latency_ms < min_) min_ = latency_ms;
            if (latency_ms > max_) max_ = latency_ms;
            double mean = static_cast<double>(sum_) / n;
            double variance =
                (static_cast<double>(sum_sq_) / n) - (mean * mean);
            double stddev = variance > 0 ? std::sqrt(variance) : 0;
            std::cout << label_ << ": " << latency_ms
                      << "ms | avg=" << static_cast<long long>(mean)
                      << " std=" << static_cast<long long>(stddev)
                      << " min=" << min_ << " max=" << max_ << " n=" << n
                      << std::endl;
        } else {
            std::cout << label_ << ": " << latency_ms << "ms (warmup "
                      << count_ << "/" << warmup_ << ")" << std::endl;
        }
    }

   private:
    std::string label_;
    int warmup_;
    long long sum_ = 0;
    long long sum_sq_ = 0;
    long long min_ = 999999;
    long long max_ = 0;
    int count_ = 0;
};
