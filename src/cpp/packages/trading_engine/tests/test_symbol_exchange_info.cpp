#include <catch2/catch_test_macros.hpp>

#include "market_making/dtypes/def_only_structs.h"

TEST_CASE("SymbolExchangeInfo disk cache round-trips through to_json",
          "[symbol_exchange_info]") {
    const auto cached = nlohmann::json::parse(R"({
        "allows_margin": false, "base": "ALEPH", "is_active": true,
        "limits": {"amount": {"max": "1000000", "min": "1"},
                   "cost": {"max": "1000000", "min": "3"},
                   "price": {"max": "100000000", "min": "-10"}},
        "precision": {"amount": 1.0, "base": 0.0, "price": 1e-06, "quote": 0.0},
        "precision_int": {"amount": 0, "price": 6},
        "quote": "USDT", "symbol": "ALEPHUSDT", "type": "spot"})");

    const auto info = Core::SymbolExchangeInfo::from_json(cached);
    CHECK(info.limits.amount.min == 1);
    CHECK(info.limits.cost.min == 3);
    CHECK(info.limits.price.max == 100000000);
    CHECK(info.precision_int.price == 6);

    const auto again = Core::SymbolExchangeInfo::from_json(info.to_json());
    CHECK(again.limits.cost.max == info.limits.cost.max);
    CHECK(again.symbol == "ALEPHUSDT");
}

TEST_CASE("SymbolExchangeInfo accepts numeric and null limits",
          "[symbol_exchange_info]") {
    auto json = nlohmann::json::parse(R"({
        "base": "BTC", "quote": "USDT", "symbol": "BTCUSDT", "type": "spot",
        "is_active": true,
        "limits": {"amount": {"max": 9000.0, "min": 0.0001},
                   "cost": {"max": null, "min": 5.0},
                   "price": {"max": 1000000.0, "min": 0.01}},
        "precision": {"amount": 0.0001, "base": 0.0, "price": 0.01, "quote": 0.0},
        "precision_int": {"amount": 4, "price": 2}})");

    const auto info = Core::SymbolExchangeInfo::from_json(json);
    CHECK(info.limits.cost.max == 0);
    CHECK(info.limits.cost.min == 5);
    CHECK_FALSE(info.allows_margin);
}
