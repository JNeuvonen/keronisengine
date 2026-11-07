#pragma once

#include <string>
#include <vector>

#include "market_making/dtypes/order.h"

namespace Core {

struct StrategyDecision {
    std::string symbol;
    std::vector<Order> orders;
    std::vector<Order> cancelled_orders;
    std::vector<Order> updated_orders;
};

}  // namespace Core