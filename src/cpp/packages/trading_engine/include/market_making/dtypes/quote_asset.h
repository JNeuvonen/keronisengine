#pragma once

#include "mpreal.h"
#include "nlohmann/json.hpp"

namespace Core {

struct QuoteAsset {
    std::string asset;
    mpfr::mpreal quantity;
    mpfr::mpreal used;
    mpfr::mpreal free;
    mpfr::mpreal price;
    long long account_id;
    std::string exchange_name;
    std::string market_type;

    nlohmann::json to_json() const;
    static QuoteAsset from_json(const nlohmann::json &json);
};

}  // namespace Core