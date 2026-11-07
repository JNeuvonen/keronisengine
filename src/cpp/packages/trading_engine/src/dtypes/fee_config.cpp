#include "market_making/dtypes/fee_config.h"

nlohmann::json Core::FeeConfig::to_json() const {
    nlohmann::json json;
    json["taker_fee_rate"] = this->taker_fee_rate;
    json["maker_fee_rate"] = this->maker_fee_rate;
    return json;
}