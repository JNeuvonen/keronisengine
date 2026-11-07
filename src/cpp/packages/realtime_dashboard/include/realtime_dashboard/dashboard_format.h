#pragma once
#ifndef DISABLE_VISUALIZATION

#include <string>

namespace Charting::Format {

// Display formatting shared by the panels. These are pure and side-effect free,
// which is what makes them worth separating from rendering: they are the only
// part of a panel that can be checked without a GL context.

std::string to_lower(const std::string& value);

// "1d" / "4h" / "15m" / "5s" / "250ms". "-" when non-positive.
std::string interval_ms(long long interval_ms);

// "YYYY-MM-DD HH:MM:SS" in UTC. "-" when non-positive.
std::string utc_ms(long long timestamp_ms);

// Coarsening age: seconds under 90s, then minutes, hours, days.
std::string age_ms(long long age_ms);

// Age of an order against a reference now, as "12s" / "3m 4s" / "2h 10m".
std::string order_age(long long create_ts, long long now_ts);

// Wall-clock creation time of an order, venue timezone aware.
std::string order_created_time(long long create_ts);

}  // namespace Charting::Format

#endif
