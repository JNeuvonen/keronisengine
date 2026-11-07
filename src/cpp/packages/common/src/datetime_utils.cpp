#include <datetime_utils.h>

using namespace std::chrono;

Date DatetimeUtils::ts_to_date(long long timestamp_ms) {
    auto ms = milliseconds(timestamp_ms);
    auto tp = system_clock::time_point(ms);

    auto time = system_clock::to_time_t(tp);

    std::tm calendar_time;
    gmtime_r(&time, &calendar_time);

    int millis = timestamp_ms % 1000;

    bool is_leap =
        (calendar_time.tm_year % 4 == 0 && calendar_time.tm_year % 100 != 0) ||
        (calendar_time.tm_year % 400 == 0);

    const char* days_of_week[] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                                  "Thursday", "Friday", "Saturday"};
    std::string day_of_week = days_of_week[calendar_time.tm_wday];

    return Date{1900 + calendar_time.tm_year,
                1 + calendar_time.tm_mon,
                calendar_time.tm_mday,
                calendar_time.tm_hour,
                calendar_time.tm_min,
                calendar_time.tm_sec,
                millis,
                day_of_week,
                is_leap};
}

std::string TimeUtils::ms_to_timestamptz(long long ms_since_epoch) {
    time_t seconds = ms_since_epoch / 1000;
    int milliseconds = ms_since_epoch % 1000;

    struct tm timeinfo;
    gmtime_r(&seconds, &timeinfo);

    char buffer[32];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S.", &timeinfo);

    return std::string(buffer) + std::to_string(milliseconds).substr(0, 3) +
           "UTC";
}

long long DatetimeUtils::get_fully_completed_bar_close_time_ms(
    long long interval_ms) {
    const auto now_ms = get_current_time_ms();
    return now_ms - (now_ms % interval_ms) -
           (now_ms % interval_ms == 0 ? interval_ms : 0);
}

long long DatetimeUtils::get_fully_completed_bar_close_time_from_input(
    long long input_time_ms, long long interval_ms) {
    return input_time_ms - (input_time_ms % interval_ms) -
           (input_time_ms % interval_ms == 0 ? interval_ms : 0);
}