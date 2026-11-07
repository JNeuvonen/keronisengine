#include "market_making/dtypes/inventory.h"

nlohmann::json Core::Inventory::to_json() const {
    nlohmann::json json;
    json["total"] = this->total.toString();
    json["long_total"] = this->long_total.toString();
    json["short_total"] = this->short_total.toString();
    json["used_margin"] = this->used_margin.has_value()
                              ? this->used_margin.value().toString()
                              : "0";
    json["maint_margin"] = this->maint_margin.has_value()
                               ? this->maint_margin.value().toString()
                               : "0";
    json["asset"] = this->asset;
    json["quote_conversion_symbol"] = this->quote_conversion_symbol;
    json["exchange_name"] = this->exchange_name;
    json["market_type"] = this->market_type;
    json["used"] = this->used.toString();
    json["free"] = this->free.toString();
    json["debt"] = this->debt.toString();
    json["account_id"] = this->account_id;
    return json;
}

Core::Inventory Core::Inventory::from_json(const nlohmann::json &json) {
    Core::Inventory inventory;
    inventory.total = mpfr::mpreal(json["total"].get<std::string>());
    inventory.used_margin = json["used_margin"].is_null()
                                ? std::nullopt
                                : std::make_optional(mpfr::mpreal(
                                      json["used_margin"].get<std::string>()));
    inventory.maint_margin =
        json["maint_margin"].is_null()
            ? std::nullopt
            : std::make_optional(
                  mpfr::mpreal(json["maint_margin"].get<std::string>()));
    inventory.asset = json["asset"].get<std::string>();
    inventory.quote_conversion_symbol =
        json["quote_conversion_symbol"].get<std::string>();
    inventory.exchange_name = json["exchange_name"].get<std::string>();
    inventory.market_type = json["market_type"].get<std::string>();
    inventory.used = mpfr::mpreal(json["used"].get<std::string>());
    inventory.free = mpfr::mpreal(json["free"].get<std::string>());
    inventory.debt = json.contains("debt")
                         ? mpfr::mpreal(json["debt"].get<std::string>())
                         : mpfr::mpreal("0");

    if (json.contains("account_id")) {
        inventory.account_id = json["account_id"].get<long long>();
    }

    // Seed per-side totals. Two sources:
    //   1. CcxtProxy responses — binance-futures hedge mode sends one row
    //      per side tagged with "position_side"=LONG/SHORT. The merge in
    //      Engine::update_inventory_and_quote_assets sums per-side rows
    //      into one Inventory per (symbol, exchange, market, account) key.
    //   2. Engine state snapshots (replay) — old snapshots may lack both
    //      position_side AND long_total/short_total; derive from sign of
    //      total. Spot / one-way futures only ever have one side.
    if (json.contains("long_total") && !json["long_total"].is_null()) {
        inventory.long_total =
            mpfr::mpreal(json["long_total"].get<std::string>());
    }
    if (json.contains("short_total") && !json["short_total"].is_null()) {
        inventory.short_total =
            mpfr::mpreal(json["short_total"].get<std::string>());
    }
    const std::string side_str =
        (json.contains("position_side") && !json["position_side"].is_null())
            ? json["position_side"].get<std::string>()
            : "";
    const bool explicit_sides_meaningful =
        (inventory.long_total != 0 || inventory.short_total != 0);
    // Resolution priority:
    //   1. Hedge-tagged LONG/SHORT row from the proxy: that row's `total`
    //      already represents one side; force the corresponding side and
    //      zero the other. Ignore explicit long/short fields here — Binance
    //      only fills one of them per row.
    //   2. Explicit non-zero long/short pair (engine state snapshot replay
    //      where to_json has serialized merged per-side totals but does
    //      NOT include position_side): trust as-is.
    //   3. Everything else — BOTH row, snapshot with zero sides, missing
    //      position_side on an old snapshot, non-hedge venues that never
    //      emit long/short at all: sign-split from net `total`.
    if (side_str == "LONG") {
        inventory.long_total = mpfr::abs(inventory.total);
        inventory.short_total = mpfr::mpreal(0);
    } else if (side_str == "SHORT") {
        inventory.long_total = mpfr::mpreal(0);
        inventory.short_total = mpfr::abs(inventory.total);
    } else if (!explicit_sides_meaningful) {
        inventory.long_total =
            inventory.total > 0 ? inventory.total : mpfr::mpreal(0);
        inventory.short_total =
            inventory.total < 0 ? -inventory.total : mpfr::mpreal(0);
    }
    return inventory;
}

mpfr::mpreal Core::Inventory::get_side_total(
    std::optional<Core::PositionSide> side) const {
    if (!side.has_value()) return this->total;
    return side.value() == Core::PositionSide::LONG_SIDE ? this->long_total
                                                          : this->short_total;
}