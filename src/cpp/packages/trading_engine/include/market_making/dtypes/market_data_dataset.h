#pragma once

#include "nlohmann/json.hpp"

namespace Core {

struct MarketDataDataset {
    std::string path;
    std::string symbol;
    std::string exchange_name;
    std::string market_type;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(MarketDataDataset, path, symbol,
                                   exchange_name, market_type);
};

}  // namespace Core