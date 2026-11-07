#include <catch2/catch_all.hpp>

#include <string>

#include "trading/hyperliquid_client.h"
#include "trading/hyperliquid_signer.h"

namespace {

// Strip 0x and leading zeros for big-int comparison (the SDK vectors drop
// leading zero nibbles; the signer emits fixed-width 32-byte hex).
std::string norm(const std::string& h) {
    std::string s = h;
    if (s.rfind("0x", 0) == 0) s = s.substr(2);
    size_t i = s.find_first_not_of('0');
    return i == std::string::npos ? "" : s.substr(i);
}

const char* PK =
    "0x0123456789012345678901234567890123456789012345678901234567890123";

}  // namespace

// Parity against the official Hyperliquid Python SDK (tests/signing_test.py):
// order ETH(asset=1) buy sz=100 px=100 GTC, nonce=0, vault=None. This drives
// the full C++ -> FFI -> .so signing path the live engine uses.
TEST_CASE("HL signer FFI matches official SDK order vector",
          "[hyperliquid][signing]") {
    HlOrderSpec spec;
    spec.asset = 1;
    spec.is_buy = 1;
    spec.price = "100";
    spec.size = "100";
    spec.reduce_only = 0;
    spec.tif = "Gtc";
    spec.cloid = "";

    SECTION("mainnet") {
        HlSignedAction r = SignOrderAction(PK, 1, 0, "", &spec, 1);
        REQUIRE(r.err == nullptr);
        REQUIRE(norm(r.sig_r) ==
                norm("0xd65369825a9df5d80099e513cce430311d7d26ddf477f5b3a33d28"
                     "06b100d78e"));
        REQUIRE(norm(r.sig_s) ==
                norm("0x2b54116ff64054968aa237c20ca9ff68000f977c93289157748a31"
                     "62b6ea940e"));
        REQUIRE(r.sig_v == 28);
        HlFree(&r);
    }

    SECTION("testnet") {
        HlSignedAction r = SignOrderAction(PK, 0, 0, "", &spec, 1);
        REQUIRE(r.err == nullptr);
        REQUIRE(norm(r.sig_r) ==
                norm("0x82b2ba28e76b3d761093aaded1b1cdad4960b3af30212b343fb2e6"
                     "cdfa4e3d54"));
        REQUIRE(norm(r.sig_s) ==
                norm("0x6b53878fc99d26047f4d7e8c90eb98955a109f44209163f52d8dc4"
                     "278cbbd9f5"));
        REQUIRE(r.sig_v == 27);
        HlFree(&r);
    }
}

TEST_CASE("HL signer rejects empty order batch", "[hyperliquid][signing]") {
    HlSignedAction r = SignOrderAction(PK, 1, 0, "", nullptr, 0);
    REQUIRE(r.err != nullptr);
    HlFree(&r);
}

TEST_CASE("HL price formatting (<=5 sig figs, <=6-szDecimals decimals)",
          "[hyperliquid][formatting]") {
    using HyperliquidDirect::HyperliquidClient;
    // Integer-valued prices stay integer regardless of sig figs.
    REQUIRE(HyperliquidClient::format_price(100.0, 2) == "100");
    REQUIRE(HyperliquidClient::format_price(105000.0, 0) == "105000");
    // 5 significant figures.
    REQUIRE(HyperliquidClient::format_price(1234.567, 0) == "1234.6");
    REQUIRE(HyperliquidClient::format_price(0.012345678, 0) == "0.012346");
    // Decimal cap from szDecimals (perps: 6 - szDecimals).
    REQUIRE(HyperliquidClient::format_price(1.23456789, 4) == "1.23");
}

TEST_CASE("HL size formatting rounds to szDecimals",
          "[hyperliquid][formatting]") {
    using HyperliquidDirect::HyperliquidClient;
    REQUIRE(HyperliquidClient::format_size(1.23456, 3) == "1.235");
    REQUIRE(HyperliquidClient::format_size(2.0, 2) == "2");
    REQUIRE(HyperliquidClient::format_size(0.10000, 4) == "0.1");
}
