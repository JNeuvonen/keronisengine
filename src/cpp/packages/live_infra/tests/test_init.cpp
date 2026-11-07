#include <catch2/catch_all.hpp>

#include "trading/account.h"

TEST_CASE("Test init", "[live_infra]") { REQUIRE(true); }

TEST_CASE("IBKR account maps to ibkr spot routing fields",
          "[live_infra][ibkr][account]") {
    Trading::Account account(
        Trading::MarketType::SPOT_MARKET_TYPE,
        Trading::ExchangeType::IBKR_ACCOUNT,
        42,
        "access",
        "secret",
        [](const std::string&) {},
        [](const std::string&) {});

    REQUIRE(account.exchange_name == "ibkr");
    REQUIRE(account.market_type == "spot");
}
