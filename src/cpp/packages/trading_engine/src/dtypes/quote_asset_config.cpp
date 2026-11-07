#include "market_making/dtypes/quote_asset_config.h"

nlohmann::json Core::QuoteAssetConfig::to_json() const {
    nlohmann::json json;
    json["asset"] = this->asset;
    json["initial_balance"] = this->initial_balance;
    json["price_multiplier"] = this->price_multiplier;
    return json;
}