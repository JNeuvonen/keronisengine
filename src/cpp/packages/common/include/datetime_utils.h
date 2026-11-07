#pragma once

#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <tuple>

struct Date {
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
    int millisecond;
    std::string day_of_week;
    bool is_leap_year;
};

namespace DatetimeUtils {
Date ts_to_date(long long timestamp_ms);
inline long long get_current_time_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               ::std::chrono::system_clock::now().time_since_epoch())
        .count();
}

long long get_fully_completed_bar_close_time_ms(long long interval_ms);
long long get_fully_completed_bar_close_time_from_input(long long input_time_ms,
                                                        long long interval_ms);

inline long long timestamptz_string_to_ms(const std::string &timestamp) {
    std::tm tm = {};
    std::istringstream ss(timestamp);

    ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");

    if (ss.fail()) {
        throw std::runtime_error("Failed to parse timestamp: " + timestamp);
    }

    auto time_since_epoch = timegm(&tm);

    int milliseconds = 0;
    int tz_hours = 0;

    char dot, plus;
    ss >> dot >> milliseconds >> plus >> tz_hours;

    long long total_ms =
        (time_since_epoch - (tz_hours * 3600)) * 1000LL + milliseconds;

    return total_ms;
}

}  // namespace DatetimeUtils
//
//
namespace TimeUtils {
class ExecutionTimeMsInScope {
   public:
    explicit ExecutionTimeMsInScope(const std::string &prefix)
        : prefix(prefix),
          start_time(::std::chrono::high_resolution_clock::now()) {}

    ~ExecutionTimeMsInScope() {
        auto end_time = ::std::chrono::high_resolution_clock::now();
        auto duration =
            ::std::chrono::duration_cast<::std::chrono::milliseconds>(
                end_time - start_time);
        std::cout << prefix << ": " << duration.count() << " milliseconds"
                  << std::endl;
    }

   private:
    std::string prefix;
    ::std::chrono::high_resolution_clock::time_point start_time;
};

class ExecutionTimeMicrosInScope {
   public:
    explicit ExecutionTimeMicrosInScope(const std::string &prefix)
        : prefix(prefix),
          start_time(::std::chrono::high_resolution_clock::now()) {}

    ~ExecutionTimeMicrosInScope() {
        auto end_time = ::std::chrono::high_resolution_clock::now();
        auto duration =
            ::std::chrono::duration_cast<::std::chrono::microseconds>(
                end_time - start_time);
        std::cout << prefix << ": " << duration.count() << " µs" << std::endl;
    }

   private:
    std::string prefix;
    ::std::chrono::high_resolution_clock::time_point start_time;
};

class ExecutionTimeNanosInScope {
   public:
    explicit ExecutionTimeNanosInScope(const std::string &prefix)
        : prefix(prefix),
          start_time(::std::chrono::high_resolution_clock::now()) {}

    ~ExecutionTimeNanosInScope() {
        auto end_time = ::std::chrono::high_resolution_clock::now();
        auto duration =
            ::std::chrono::duration_cast<::std::chrono::nanoseconds>(
                end_time - start_time);
        std::cout << prefix << ": " << duration.count() << " ns" << std::endl;
    }

   private:
    std::string prefix;
    ::std::chrono::high_resolution_clock::time_point start_time;
};

template <typename Func, typename... Args>
auto measure_execution_time(const std::string &prefix, Func func,
                            Args &&...args) {
    using namespace ::std::chrono;

    auto start = high_resolution_clock::now();

    if constexpr (std::is_void<decltype(func(
                      std::forward<Args>(args)...))>::value) {
        // Handle void return type
        func(std::forward<Args>(args)...);
        auto end = high_resolution_clock::now();
        auto duration = duration_cast<seconds>(end - start);

        std::cout << prefix << ": " << duration.count() << " seconds"
                  << std::endl;
    } else {
        // Handle non-void return type
        auto result = func(std::forward<Args>(args)...);
        auto end = high_resolution_clock::now();
        auto duration = duration_cast<seconds>(end - start);

        std::cout << prefix << ": " << duration.count() << " seconds"
                  << std::endl;
        return result;
    }
}

std::string ms_to_timestamptz(long long ms_since_epoch);
}  // namespace TimeUtils
