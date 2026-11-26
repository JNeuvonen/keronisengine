#pragma once
#include <string>

namespace MarketData {

struct BarClosePolicy {
    bool is_dollar_bar = false;
    bool is_tick_count_bar = false;
    bool is_time_bar = false;
    bool is_exchange_bar = false;
    bool store_only_high_and_low = false;  // order fill bar for simulation
    int tick_count = 0;
    double dollar_amount = 0;
    long long time_ms = 0;
    std::string interval_str;

    static BarClosePolicy create(bool is_dollar_bar, bool is_tick_count_bar,
                                 bool is_time_bar, bool is_exchange_bar,
                                 int tick_count, double dollar_amount,
                                 long long time_ms,
                                 bool store_only_high_and_low = false);

    static BarClosePolicy time_bars(long long time_ms) {
        return create(false, false, true, false, 0, 0, time_ms);
    }

    [[nodiscard]] std::string to_string() const;

    std::string get_bar_type_and_close_threshold() const;

    bool operator==(const BarClosePolicy& other) const {
        return is_dollar_bar == other.is_dollar_bar &&
               is_tick_count_bar == other.is_tick_count_bar &&
               is_time_bar == other.is_time_bar &&
               is_exchange_bar == other.is_exchange_bar &&
               tick_count == other.tick_count &&
               dollar_amount == other.dollar_amount &&
               time_ms == other.time_ms && interval_str == other.interval_str;
    }
};

BarClosePolicy parse_bar_type_from_path(const std::string& str);

std::string get_bar_type_base_dir(const BarClosePolicy& bar_close_policy,
                                  const std::string& exchange_name,
                                  const std::string& market_type);

}  // namespace MarketData