#pragma once

#include "market_making/dtypes/account_config.h"
#include "nlohmann/json.hpp"

namespace Core {

struct ExchangeConfig {
    std::string exchange_name;
    std::string market_type;
    double taker_fee_rate;
    double maker_fee_rate;
    double maker_rebate_rate = 0;
    std::vector<Core::AccountConfig> accounts;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        ExchangeConfig, exchange_name, market_type, taker_fee_rate,
        maker_fee_rate, maker_rebate_rate, accounts);

    nlohmann::json to_json() const;
};
}  // namespace Core
