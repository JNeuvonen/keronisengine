#include <strategy_utils.h>

#include <algorithm>
#include <cmath>

double StrategyUtils::polymarket_min_quantity(double price, double min_size,
                                              double min_notional) {
    if (price <= 0.0) return min_size;
    return std::max(min_size, std::ceil(min_notional / price));
}

double StrategyUtils::polymarket_dynamic_buffer(double fair_value,
                                                double min_buffer,
                                                double max_buffer) {
    double t = 4.0 * fair_value * (1.0 - fair_value);
    return min_buffer + (max_buffer - min_buffer) * t;
}
