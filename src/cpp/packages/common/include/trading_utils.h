#pragma once
#include <string>
#include <vector>

namespace TradingUtils {

struct TradeSignal {
    std::string symbol;
    double sizing_coeff;
};

struct LimitOrderSizeAndPrice {
    double size;
    double price;
};

bool is_stop_loss_hit(const std::vector<double>& prices,
                      double stop_loss_threshold, bool is_short_strategy);

bool is_take_profit_hit(const std::vector<double>& prices,
                        double take_profit_threshold, bool is_short_strategy);

bool should_close_by_time(long long enter_time_ms, long long current_time_ms,
                          long max_hold_duration_ms);

bool is_stop_loss_hit(double open_price, double current_price,
                      double stop_loss_threshold, bool is_short_strategy);

bool is_take_profit_hit(double open_price, double current_price,
                        double take_profit_threshold, bool is_short_strategy);
double calculate_percentage_result(double open_price, double close_price,
                                   bool is_short_strategy);
double calculate_pnl(double open_price, double close_price, double size,
                     bool is_short_strategy);

}  // namespace TradingUtils
