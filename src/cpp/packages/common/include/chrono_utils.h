#pragma once
#include <chrono>
#include <string>

namespace ChronoUtils {

std::chrono::system_clock::time_point make_timepoint(int year, int month,
                                                     int day, int hour,
                                                     int minute, int sec);

std::chrono::system_clock::time_point make_now_utc_timepoint();

std::string chrono_timepoint_to_string(
    std::chrono::system_clock::time_point tp);

std::tm gmtime_safe(std::time_t t);

std::chrono::system_clock::time_point make_utc_timepoint_from_epoch_ms(
    long long ms_since_epoch);

long long to_ms_timestamp(std::chrono::system_clock::time_point tp);

bool is_two_ts_on_same_interval(long long t1, long long t2,
                                long long interval_ms);

}  // namespace ChronoUtils
