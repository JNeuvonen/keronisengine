#pragma once

#include <string>

#include "market_data/enums.h"
namespace MarketData {

struct MarketDataIdentifier {
    Exchange exchange;
    MarketType market_type;
    std::string symbol;
};

}  // namespace MarketData
