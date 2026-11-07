#include "market_making/dtypes/account_config.h"

nlohmann::json Core::AccountConfig::to_json() const {
    nlohmann::json json;
    json["account_id"] = this->account_id;
    json["quote_assets"] = nlohmann::json::array();
    for (const auto& quote_asset : this->quote_assets) {
        json["quote_assets"].push_back(quote_asset.to_json());
    }
    return json;
}