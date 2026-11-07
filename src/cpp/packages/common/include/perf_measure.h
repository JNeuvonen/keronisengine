#pragma once

#include <chrono>
#include <iostream>
#include <string_view>

class ScopedTimer {
    std::string_view name_;
    std::string_view unit_;
    std::chrono::high_resolution_clock::time_point start_;

   public:
    inline ScopedTimer(std::string_view name, std::string_view unit = "ns")
        : name_(name),
          unit_(unit),
          start_(std::chrono::high_resolution_clock::now()) {}

    inline ~ScopedTimer() {
        auto end = std::chrono::high_resolution_clock::now();
        auto duration_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start_)
                .count();

        auto value = static_cast<double>(duration_ns);
        const char* suffix = "ns";

        if (unit_ == "us") {
            value /= 1e3;
            suffix = "µs";
        } else if (unit_ == "ms") {
            value /= 1e6;
            suffix = "ms";
        } else if (unit_ == "s") {
            value /= 1e9;
            suffix = "s";
        } else if (unit_ == "m") {
            value /= 6e10;
            suffix = "min";
        }

        std::cout << name_ << ": " << value << suffix << "\n";
    }
};

#define MEASURE_SCOPE(name, ...) \
    ScopedTimer timer##__LINE__(name, ##__VA_ARGS__)
