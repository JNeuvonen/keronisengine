#pragma once

#include "market_making/dtypes/inventory.h"
#include "market_making/dtypes/quote_asset.h"
#include "market_making/dtypes/trade.h"

namespace Core {

struct FullAccountState {
    std::unordered_map<std::string, Core::QuoteAsset> quote_assets;
    std::unordered_map<std::string, Core::Inventory> inventories;
    std::vector<Core::Trade> trades;
};

}  // namespace Core
