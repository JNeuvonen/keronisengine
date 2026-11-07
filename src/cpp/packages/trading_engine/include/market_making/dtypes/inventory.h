#pragma once

#include <optional>
#include <string>

#include "market_making/dtypes/enums.h"
#include "mpreal.h"
#include "nlohmann/json.hpp"

namespace Core {

struct Inventory {
    // Net signed position across both sides — kept for back-compat with
    // callers that don't care about hedge-mode split. Equals (long_total -
    // short_total). For Binance USDS-M futures hedge mode the long_total and
    // short_total are tracked independently from per-side position rows; for
    // non-hedge venues (spot, one-way futures) long_total = max(total, 0)
    // and short_total = max(-total, 0) so single-side callers still work.
    mpfr::mpreal total;
    mpfr::mpreal long_total;
    mpfr::mpreal short_total;
    mpfr::mpreal used;
    mpfr::mpreal free;
    mpfr::mpreal debt;
    std::optional<mpfr::mpreal> used_margin;
    std::optional<mpfr::mpreal> maint_margin;
    std::string asset;
    std::string quote_conversion_symbol;
    std::string exchange_name;
    std::string market_type;
    long long account_id;

    nlohmann::json to_json() const;
    static Inventory from_json(const nlohmann::json &json);

    // Return long_total / short_total / total depending on side. Pass
    // nullopt for net (back-compat with non-hedge callers).
    mpfr::mpreal get_side_total(std::optional<PositionSide> side) const;
};

}  // namespace Core