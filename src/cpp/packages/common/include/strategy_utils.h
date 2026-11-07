#pragma once

namespace StrategyUtils {

double polymarket_min_quantity(double price, double min_size,
                               double min_notional = 1.0);

double polymarket_dynamic_buffer(double fair_value, double min_buffer,
                                 double max_buffer);

}  // namespace StrategyUtils
