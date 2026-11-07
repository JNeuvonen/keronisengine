#include "market_data/bar_close_policy.h"

#include <stdexcept>

namespace MarketData {

std::string BarClosePolicy::to_string() const {
    if (is_dollar_bar) {
        return "dollar_vol_" + std::to_string(dollar_amount);
    }
    if (is_tick_count_bar) {
        return "tick_count_" + std::to_string(tick_count);
    }

    if (is_exchange_bar) {
        return "exchange_ms_" + std::to_string(time_ms);
    }

    return "time_ms_" + std::to_string(time_ms);
}

BarClosePolicy BarClosePolicy::create(bool is_dollar_bar,
                                      bool is_tick_count_bar, bool is_time_bar,
                                      bool is_exchange_bar, int tick_count,
                                      double dollar_amount, long long time_ms,
                                      bool store_only_high_and_low) {
    BarClosePolicy policy;

    int num_bool_args = is_dollar_bar + is_tick_count_bar + is_time_bar;
    if (num_bool_args != 1) {
        throw std::invalid_argument(
            "Exactly one of is_dollar_bar, is_tick_count_bar, or is_time_bar "
            "must be true");
    }

    policy.is_dollar_bar = is_dollar_bar;
    policy.is_tick_count_bar = is_tick_count_bar;
    policy.is_time_bar = is_time_bar;
    policy.is_exchange_bar = is_exchange_bar;
    policy.store_only_high_and_low = store_only_high_and_low;

    if (is_dollar_bar) {
        if (dollar_amount <= 0) {
            throw std::invalid_argument("dollar_amount must be greater than 0");
        }
        policy.dollar_amount = dollar_amount;
    }

    if (is_tick_count_bar) {
        if (tick_count <= 0) {
            throw std::invalid_argument("tick_count must be greater than 0");
        }
        policy.tick_count = tick_count;
    }

    if (is_time_bar) {
        if (time_ms <= 0) {
            throw std::invalid_argument("time_ms must be greater than 0");
        }
        policy.time_ms = time_ms;
    }

    return policy;
}

std::string BarClosePolicy::get_bar_type_and_close_threshold() const {
    if (this->store_only_high_and_low) {
        return "order_fill_bar/" + std::to_string(this->time_ms) + "_ms";
    }

    if (this->is_time_bar && !this->is_exchange_bar) {
        return "time/" + std::to_string(this->time_ms) + "_ms";
    }

    if (this->is_dollar_bar) {
        return "dollar_vol/" + std::to_string(this->dollar_amount);
    }

    if (this->is_tick_count_bar) {
        return "tick_count/" + std::to_string(this->tick_count);
    }

    if (this->is_exchange_bar) {
        return "exchange/" + std::to_string(this->time_ms) + "_ms";
    }

    return "";
}

BarClosePolicy parse_bar_type_from_path(const std::string& str) {
    bool is_time_bar = false;
    bool is_dollar_bar = false;
    bool is_tick_count_bar = false;
    bool is_exchange_bar = false;
    int tick_count = 0;
    double dollar_amount = 0;
    long long time_ms = 0;

    size_t time_pos = str.find("/time/");
    size_t dollar_pos = str.find("/dollar_vol/");
    size_t tick_pos = str.find("/tick_count/");
    size_t exchange_pos = str.find("/exchange/");

    if (exchange_pos != std::string::npos) {
        is_time_bar = true;
        is_exchange_bar = true;
        size_t ms_pos = str.find("_ms", exchange_pos);
        if (ms_pos != std::string::npos) {
            time_ms = std::stoll(
                str.substr(exchange_pos + 10, ms_pos - (exchange_pos + 10)));
        }
    } else if (time_pos != std::string::npos) {
        is_time_bar = true;
        size_t ms_pos = str.find("_ms", time_pos);
        if (ms_pos != std::string::npos) {
            time_ms =
                std::stoll(str.substr(time_pos + 6, ms_pos - (time_pos + 6)));
        }
    } else if (dollar_pos != std::string::npos) {
        is_dollar_bar = true;
        size_t next_slash = str.find("/", dollar_pos + 1);
        if (next_slash != std::string::npos) {
            dollar_amount = std::stod(
                str.substr(dollar_pos + 12, next_slash - (dollar_pos + 12)));
        } else {
            dollar_amount = std::stod(str.substr(dollar_pos + 12));
        }
    } else if (tick_pos != std::string::npos) {
        is_tick_count_bar = true;
        size_t next_slash = str.find("/", tick_pos + 1);
        if (next_slash != std::string::npos) {
            tick_count = std::stoi(
                str.substr(tick_pos + 12, next_slash - (tick_pos + 12)));
        } else {
            tick_count = std::stoi(str.substr(tick_pos + 12));
        }
    }

    return BarClosePolicy::create(is_dollar_bar, is_tick_count_bar, is_time_bar,
                                  is_exchange_bar, tick_count, dollar_amount,
                                  time_ms);
}

std::string get_bar_type_base_dir(const BarClosePolicy& bar_close_policy,
                                  const std::string& exchange_name,
                                  const std::string& market_type) {
    return "derived/bars/" + exchange_name + "/" + market_type + "/" +
           bar_close_policy.get_bar_type_and_close_threshold();
}

}  // namespace MarketData
