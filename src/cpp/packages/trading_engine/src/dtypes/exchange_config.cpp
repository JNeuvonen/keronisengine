#include "market_making/dtypes/exchange_config.h"

nlohmann::json Core::ExchangeConfig::to_json() const {
    nlohmann::json json;
    json["exchange_name"] = this->exchange_name;
    json["market_type"] = this->market_type;
    json["taker_fee_rate"] = this->taker_fee_rate;
    json["maker_fee_rate"] = this->maker_fee_rate;
    json["accounts"] = nlohmann::json::array();
    for (const auto &account : this->accounts) {
        json["accounts"].push_back(account.to_json());
    }
    return json;
}