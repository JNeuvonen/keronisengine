#pragma once

#include <string>

#include "market_making/dtypes/quote_asset_config.h"
#include "nlohmann/json.hpp"

namespace Core {

struct AccountConfig {
    long long account_id;
    std::vector<Core::QuoteAssetConfig> quote_assets;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(AccountConfig, account_id, quote_assets);
    nlohmann::json to_json() const;
};
}  // namespace Core