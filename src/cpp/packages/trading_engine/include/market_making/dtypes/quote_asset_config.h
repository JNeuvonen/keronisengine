#pragma once

#include "nlohmann/json.hpp"

namespace Core {

struct QuoteAssetConfig {
    std::string asset;
    double initial_balance;
    double price_multiplier;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(QuoteAssetConfig, asset, initial_balance,
                                   price_multiplier);

    nlohmann::json to_json() const;
};

}  // namespace Core