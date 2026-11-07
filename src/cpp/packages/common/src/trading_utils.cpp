#include <trading_utils.h>

bool TradingUtils::is_stop_loss_hit(const std::vector<double>& prices,
                                    double stop_loss_threshold,
                                    bool is_short_strategy) {
    if (prices.size() < 2) {
        return false;
    }

    double first_price = prices.front();
    double last_price = prices.back();

    if (is_short_strategy) {
        return last_price >= first_price * (1.0 + stop_loss_threshold);
    } else {
        return last_price <= first_price * (1.0 - stop_loss_threshold);
    }
}

bool TradingUtils::is_take_profit_hit(const std::vector<double>& prices,
                                      double take_profit_threshold,
                                      bool is_short_strategy) {
    if (prices.size() < 2) {
        return false;
    }

    double first_price = prices.front();
    double last_price = prices.back();

    if (is_short_strategy) {
        return last_price <= first_price * (1.0 - take_profit_threshold);
    } else {
        return last_price >= first_price * (1.0 + take_profit_threshold);
    }
}

bool TradingUtils::should_close_by_time(long long enter_time_ms,
                                        long long current_time_ms,
                                        long max_hold_duration_ms) {
    return (current_time_ms - enter_time_ms) >= max_hold_duration_ms;
}

bool TradingUtils::is_stop_loss_hit(double open_price, double current_price,
                                    double stop_loss_threshold,
                                    bool is_short_strategy) {
    if (is_short_strategy) {
        return current_price >= open_price * (1.0 + stop_loss_threshold);
    } else {
        return current_price <= open_price * (1.0 - stop_loss_threshold);
    }
}

bool TradingUtils::is_take_profit_hit(double open_price, double current_price,
                                      double take_profit_threshold,
                                      bool is_short_strategy) {
    if (is_short_strategy) {
        return current_price <= open_price * (1.0 - take_profit_threshold);
    } else {
        return current_price >= open_price * (1.0 + take_profit_threshold);
    }
}

double TradingUtils::calculate_percentage_result(double open_price,
                                                 double close_price,
                                                 bool is_short_strategy) {
    if (open_price == 0) {
        return 0;
    }
    if (is_short_strategy) {
        return ((open_price - close_price) / open_price) * 100;
    } else {
        return ((close_price - open_price) / open_price) * 100;
    }
}

double TradingUtils::calculate_pnl(double open_price, double close_price,
                                   double size, bool is_short_strategy) {
    if (is_short_strategy) {
        return (open_price - close_price) * size;
    } else {
        return (close_price - open_price) * size;
    }
}
