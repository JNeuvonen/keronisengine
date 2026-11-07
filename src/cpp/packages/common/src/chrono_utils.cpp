#include "chrono_utils.h"

#include <iomanip>
#include <sstream>

namespace ChronoUtils {

std::chrono::system_clock::time_point make_timepoint(int year, int month,
                                                     int day, int hour,
                                                     int minute, int sec) {
    std::tm tm{};
    tm.tm_year = year - 1900;  // years since 1900
    tm.tm_mon = month - 1;     // months since January [0, 11]
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = sec;
    std::time_t time = std::mktime(&tm);
    return std::chrono::system_clock::from_time_t(time);
}

std::chrono::system_clock::time_point make_now_utc_timepoint() {
    return std::chrono::system_clock::now();
}

std::tm gmtime_safe(std::time_t t) {
    std::tm tm{};
    gmtime_r(&t, &tm);
    return tm;
}

std::chrono::system_clock::time_point make_utc_timepoint_from_epoch_ms(
    long long ms_since_epoch) {
    return std::chrono::system_clock::time_point{
        std::chrono::milliseconds(ms_since_epoch)};
}

std::string chrono_timepoint_to_string(
    std::chrono::system_clock::time_point tp) {
    auto now_c = std::chrono::system_clock::to_time_t(tp);
    std::tm tm_buf{};
#if defined(_MSC_VER)
    gmtime_s(&tm_buf, &now_c);  // Windows
#else
    gmtime_r(&now_c, &tm_buf);  // POSIX
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

long long to_ms_timestamp(std::chrono::system_clock::time_point tp) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               tp.time_since_epoch())
        .count();
}

bool is_two_ts_on_same_interval(long long t1, long long t2,
                                long long interval_ms) {
    long long interval_start1 = (t1 / interval_ms) * interval_ms;
    long long interval_start2 = (t2 / interval_ms) * interval_ms;

    return interval_start1 == interval_start2;
}

}  // namespace ChronoUtils
