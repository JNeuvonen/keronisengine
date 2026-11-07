#pragma once
#include "nlohmann/json.hpp"

namespace Core {

struct FeeConfig {
    double taker_fee_rate;
    double maker_fee_rate;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(FeeConfig, taker_fee_rate, maker_fee_rate);
    nlohmann::json to_json() const;
};

}  // namespace Core