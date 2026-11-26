#include <catch2/catch_all.hpp>

#include <cmath>
#include <fstream>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "crypto/eip712.h"
#include "crypto/keccak256.h"
#include "crypto/secp256k1_signer.h"

// ──────────────────────────────────────────────────────────────────────────
// Inline reimplementation of the rounding/amount logic from
// polymarket_client.cpp — we test this exact logic against Python.
// ──────────────────────────────────────────────────────────────────────────

namespace TestAmounts {

struct RoundConfig {
    int price;
    int size;
    int amount;
};

RoundConfig get_round_config(const std::string& tick_size) {
    if (tick_size == "0.1") return {1, 2, 3};
    if (tick_size == "0.01") return {2, 2, 4};
    if (tick_size == "0.001") return {3, 2, 5};
    if (tick_size == "0.0001") return {4, 2, 6};
    return {4, 2, 6};
}

double round_normal(double x, int d) {
    // Match Python's banker's rounding (round half to even)
    double f = std::pow(10.0, d);
    return std::nearbyint(x * f) / f;
}

double round_down(double x, int d) {
    double f = std::pow(10.0, d);
    return std::floor(x * f) / f;
}

double round_up(double x, int d) {
    double f = std::pow(10.0, d);
    return std::ceil(x * f) / f;
}

int decimal_places(double x) {
    char buf[32];
    auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), x);
    if (ec != std::errc()) return 0;
    std::string_view sv(buf, ptr - buf);
    auto dot = sv.find('.');
    if (dot == std::string_view::npos) return 0;
    auto last_nonzero = sv.find_last_not_of('0');
    if (last_nonzero <= dot) return 0;
    return static_cast<int>(last_nonzero - dot);
}

uint64_t to_token_decimals(double x) {
    double f = 1000000.0 * x;
    if (decimal_places(f) > 0) {
        f = std::nearbyint(f);
    }
    return static_cast<uint64_t>(f);
}

struct AmountResult {
    uint8_t side;
    uint64_t maker_amount;
    uint64_t taker_amount;
};

AmountResult calculate_order_amounts(const std::string& side, double size,
                                      double price, const RoundConfig& config) {
    double raw_price = round_normal(price, config.price);

    if (side == "BUY" || side == "buy") {
        double raw_taker_amt = round_down(size, config.size);
        double raw_maker_amt = raw_taker_amt * raw_price;

        if (decimal_places(raw_maker_amt) > config.amount) {
            raw_maker_amt = round_up(raw_maker_amt, config.amount + 4);
            if (decimal_places(raw_maker_amt) > config.amount) {
                raw_maker_amt = round_down(raw_maker_amt, config.amount);
            }
        }

        return {0, to_token_decimals(raw_maker_amt),
                to_token_decimals(raw_taker_amt)};
    } else {
        double raw_maker_amt = round_down(size, config.size);
        double raw_taker_amt = raw_maker_amt * raw_price;

        if (decimal_places(raw_taker_amt) > config.amount) {
            raw_taker_amt = round_up(raw_taker_amt, config.amount + 4);
            if (decimal_places(raw_taker_amt) > config.amount) {
                raw_taker_amt = round_down(raw_taker_amt, config.amount);
            }
        }

        return {1, to_token_decimals(raw_maker_amt),
                to_token_decimals(raw_taker_amt)};
    }
}

}  // namespace TestAmounts

// ──────────────────────────────────────────────────────────────────────────
// Load test vectors JSON
// ──────────────────────────────────────────────────────────────────────────

static nlohmann::json load_test_vectors() {
    std::vector<std::string> paths = {
        "scripts/utilities/polymarkets/signing_test_vectors_10k.json",
        "../scripts/utilities/polymarkets/signing_test_vectors_10k.json",
        "../../scripts/utilities/polymarkets/signing_test_vectors_10k.json",
        "../../../scripts/utilities/polymarkets/"
        "signing_test_vectors_10k.json",
    };

    const char* data_dir = std::getenv("DATA_DIR_ROOT");
    if (data_dir) {
        std::string base(data_dir);
        auto pos = base.rfind('/');
        if (pos != std::string::npos) {
            paths.push_back(
                base.substr(0, pos) +
                "/code/trading-project/scripts/utilities/polymarkets/"
                "signing_test_vectors_10k.json");
        }
    }

    for (const auto& path : paths) {
        std::ifstream f(path);
        if (f.is_open()) {
            std::cout << "Loading test vectors from: " << path << std::endl;
            return nlohmann::json::parse(f);
        }
    }

    FAIL("Could not find signing_test_vectors_10k.json — run "
         "generate the signing test vectors file first");
    return {};
}

// ──────────────────────────────────────────────────────────────────────────
// Test 1: Amount calculation (maker_amount, taker_amount)
// ──────────────────────────────────────────────────────────────────────────

TEST_CASE("10k amount calculation comparison", "[polymarket][10k]") {
    auto data = load_test_vectors();
    const auto& vectors = data["vectors"];

    int total = vectors.size();
    int passed = 0;
    int failed = 0;

    for (int i = 0; i < total; i++) {
        const auto& v = vectors[i];

        std::string side = v["side"].get<std::string>();
        double price = v["price"].get<double>();
        double size = v["size"].get<double>();
        std::string tick_size = v["tick_size"].get<std::string>();

        auto config = TestAmounts::get_round_config(tick_size);
        auto result =
            TestAmounts::calculate_order_amounts(side, size, price, config);

        uint64_t expected_maker = v["maker_amount"].get<uint64_t>();
        uint64_t expected_taker = v["taker_amount"].get<uint64_t>();
        uint8_t expected_side = v["side_int"].get<uint8_t>();

        bool ok = (result.maker_amount == expected_maker &&
                   result.taker_amount == expected_taker &&
                   result.side == expected_side);

        if (ok) {
            passed++;
        } else {
            failed++;
            if (failed <= 20) {
                std::cout << "\n[AMOUNT MISMATCH] vector #" << i << ":"
                          << "\n  side=" << side << " price=" << price
                          << " size=" << size << " tick=" << tick_size
                          << "\n  C++:    maker=" << result.maker_amount
                          << " taker=" << result.taker_amount
                          << " side=" << (int)result.side
                          << "\n  Python: maker=" << expected_maker
                          << " taker=" << expected_taker
                          << " side=" << (int)expected_side << std::endl;
            }
        }
    }

    std::cout << "\n=== Amount Calculation Results ===" << std::endl;
    std::cout << "Total: " << total << " | Passed: " << passed
              << " | Failed: " << failed << std::endl;

    if (failed > 20) {
        std::cout << "(Showing first 20 of " << failed << " failures)"
                  << std::endl;
    }

    REQUIRE(failed == 0);
}

// ──────────────────────────────────────────────────────────────────────────
// Test 2: EIP-712 hashing (struct_hash, order_id)
// ──────────────────────────────────────────────────────────────────────────

TEST_CASE("10k EIP-712 hash comparison", "[polymarket][10k]") {
    auto data = load_test_vectors();
    const auto& vectors = data["vectors"];

    std::string maker_address = data["maker_address"].get<std::string>();
    std::string signer_address = data["signer_address"].get<std::string>();

    int total = vectors.size();
    int passed = 0;
    int failed = 0;

    for (int i = 0; i < total; i++) {
        const auto& v = vectors[i];

        EIP712::OrderFields fields;
        fields.salt = v["salt"].get<uint64_t>();
        fields.maker = maker_address;
        fields.signer = signer_address;
        fields.taker = "0x0000000000000000000000000000000000000000";
        fields.token_id = v["token_id"].get<std::string>();
        fields.maker_amount = v["maker_amount"].get<uint64_t>();
        fields.taker_amount = v["taker_amount"].get<uint64_t>();
        fields.expiration = 0;
        fields.nonce = 0;
        fields.fee_rate_bps =
            static_cast<uint64_t>(v["fee_rate_bps"].get<int>());
        fields.side = v["side_int"].get<uint8_t>();
        fields.signature_type = 0;

        bool neg_risk = v["neg_risk"].get<bool>();
        auto hash = EIP712::hash_order(fields, neg_risk);
        std::string order_id = "0x" + Keccak::to_hex(hash);

        std::string expected_order_id = v["order_id"].get<std::string>();

        if (order_id == expected_order_id) {
            passed++;
        } else {
            failed++;
            if (failed <= 20) {
                auto sh = EIP712::compute_struct_hash(fields);
                std::string struct_hash_hex = Keccak::to_hex(sh);
                std::string expected_struct =
                    v["struct_hash"].get<std::string>();

                std::cout << "\n[HASH MISMATCH] vector #" << i << ":"
                          << "\n  salt=" << fields.salt
                          << " token_id="
                          << fields.token_id.substr(0, 20) << "..."
                          << "\n  maker_amount=" << fields.maker_amount
                          << " taker_amount=" << fields.taker_amount
                          << "\n  side=" << (int)fields.side
                          << " neg_risk=" << neg_risk
                          << "\n  C++ struct_hash:  " << struct_hash_hex
                          << "\n  Py  struct_hash:  " << expected_struct
                          << "\n  C++ order_id:     " << order_id
                          << "\n  Py  order_id:     " << expected_order_id
                          << std::endl;
            }
        }
    }

    std::cout << "\n=== EIP-712 Hash Results ===" << std::endl;
    std::cout << "Total: " << total << " | Passed: " << passed
              << " | Failed: " << failed << std::endl;

    REQUIRE(failed == 0);
}

// ──────────────────────────────────────────────────────────────────────────
// Test 3: ECDSA signature
// ──────────────────────────────────────────────────────────────────────────

TEST_CASE("10k signature comparison", "[polymarket][10k]") {
    auto data = load_test_vectors();
    const auto& vectors = data["vectors"];

    std::string hex_key = data["private_key"].get<std::string>();
    auto privkey = Secp256k1Signer::parse_private_key(hex_key);

    std::string maker_address = data["maker_address"].get<std::string>();
    std::string signer_address = data["signer_address"].get<std::string>();

    int total = vectors.size();
    int passed = 0;
    int failed = 0;

    for (int i = 0; i < total; i++) {
        const auto& v = vectors[i];

        EIP712::OrderFields fields;
        fields.salt = v["salt"].get<uint64_t>();
        fields.maker = maker_address;
        fields.signer = signer_address;
        fields.taker = "0x0000000000000000000000000000000000000000";
        fields.token_id = v["token_id"].get<std::string>();
        fields.maker_amount = v["maker_amount"].get<uint64_t>();
        fields.taker_amount = v["taker_amount"].get<uint64_t>();
        fields.expiration = 0;
        fields.nonce = 0;
        fields.fee_rate_bps =
            static_cast<uint64_t>(v["fee_rate_bps"].get<int>());
        fields.side = v["side_int"].get<uint8_t>();
        fields.signature_type = 0;

        bool neg_risk = v["neg_risk"].get<bool>();
        auto hash = EIP712::hash_order(fields, neg_risk);
        auto sig = Secp256k1Signer::sign_hash_hex(hash, privkey.data());

        std::string expected_sig = v["signature"].get<std::string>();

        if (sig == expected_sig) {
            passed++;
        } else {
            failed++;
            if (failed <= 10) {
                std::cout << "\n[SIG MISMATCH] vector #" << i << ":"
                          << "\n  C++: " << sig
                          << "\n  Py:  " << expected_sig << std::endl;
            }
        }
    }

    std::cout << "\n=== Signature Results ===" << std::endl;
    std::cout << "Total: " << total << " | Passed: " << passed
              << " | Failed: " << failed << std::endl;

    REQUIRE(failed == 0);
}

// ──────────────────────────────────────────────────────────────────────────
// Test 4: Full pipeline (amount calc → hash → sign) — mirrors production
// ──────────────────────────────────────────────────────────────────────────

TEST_CASE("10k full pipeline comparison", "[polymarket][10k]") {
    auto data = load_test_vectors();
    const auto& vectors = data["vectors"];

    std::string hex_key = data["private_key"].get<std::string>();
    auto privkey = Secp256k1Signer::parse_private_key(hex_key);

    std::string maker_address = data["maker_address"].get<std::string>();
    std::string signer_address = data["signer_address"].get<std::string>();

    int total = vectors.size();
    int amount_fail = 0;
    int hash_fail = 0;
    int sig_fail = 0;
    int all_pass = 0;

    for (int i = 0; i < total; i++) {
        const auto& v = vectors[i];

        // Step 1: Amounts
        std::string side = v["side"].get<std::string>();
        double price = v["price"].get<double>();
        double size = v["size"].get<double>();
        std::string tick_size = v["tick_size"].get<std::string>();

        auto config = TestAmounts::get_round_config(tick_size);
        auto amounts =
            TestAmounts::calculate_order_amounts(side, size, price, config);

        uint64_t expected_maker = v["maker_amount"].get<uint64_t>();
        uint64_t expected_taker = v["taker_amount"].get<uint64_t>();

        if (amounts.maker_amount != expected_maker ||
            amounts.taker_amount != expected_taker) {
            amount_fail++;
            if (amount_fail <= 5) {
                std::cout << "\n[PIPELINE AMOUNT FAIL] #" << i << ": "
                          << side << " " << price << " x " << size
                          << " tick=" << tick_size
                          << " | C++ maker=" << amounts.maker_amount
                          << " taker=" << amounts.taker_amount
                          << " | Py maker=" << expected_maker
                          << " taker=" << expected_taker << std::endl;
            }
            continue;
        }

        // Step 2: Hash (using C++-computed amounts)
        EIP712::OrderFields fields;
        fields.salt = v["salt"].get<uint64_t>();
        fields.maker = maker_address;
        fields.signer = signer_address;
        fields.taker = "0x0000000000000000000000000000000000000000";
        fields.token_id = v["token_id"].get<std::string>();
        fields.maker_amount = amounts.maker_amount;
        fields.taker_amount = amounts.taker_amount;
        fields.expiration = 0;
        fields.nonce = 0;
        fields.fee_rate_bps =
            static_cast<uint64_t>(v["fee_rate_bps"].get<int>());
        fields.side = amounts.side;
        fields.signature_type = 0;

        bool neg_risk = v["neg_risk"].get<bool>();
        auto hash = EIP712::hash_order(fields, neg_risk);
        std::string order_id = "0x" + Keccak::to_hex(hash);

        std::string expected_order_id = v["order_id"].get<std::string>();
        if (order_id != expected_order_id) {
            hash_fail++;
            if (hash_fail <= 5) {
                std::cout << "\n[PIPELINE HASH FAIL] #" << i << ":"
                          << "\n  C++: " << order_id
                          << "\n  Py:  " << expected_order_id << std::endl;
            }
            continue;
        }

        // Step 3: Signature
        auto sig = Secp256k1Signer::sign_hash_hex(hash, privkey.data());
        std::string expected_sig = v["signature"].get<std::string>();
        if (sig != expected_sig) {
            sig_fail++;
            if (sig_fail <= 5) {
                std::cout << "\n[PIPELINE SIG FAIL] #" << i << ":"
                          << "\n  C++: " << sig
                          << "\n  Py:  " << expected_sig << std::endl;
            }
            continue;
        }

        all_pass++;
    }

    std::cout << "\n=== Full Pipeline Results ===" << std::endl;
    std::cout << "Total: " << total << std::endl;
    std::cout << "All passed: " << all_pass << std::endl;
    std::cout << "Amount failures: " << amount_fail << std::endl;
    std::cout << "Hash failures: " << hash_fail << std::endl;
    std::cout << "Signature failures: " << sig_fail << std::endl;

    REQUIRE(amount_fail == 0);
    REQUIRE(hash_fail == 0);
    REQUIRE(sig_fail == 0);
}

// ──────────────────────────────────────────────────────────────────────────
// Test 5: Byte-for-byte JSON body comparison
// Builds the exact JSON that C++ would send to the CLOB API and compares
// against the Python-serialized json_body in the test vectors.
// ──────────────────────────────────────────────────────────────────────────

TEST_CASE("10k JSON body byte-for-byte comparison", "[polymarket][10k]") {
    auto data = load_test_vectors();
    const auto& vectors = data["vectors"];

    // Skip if vectors don't have json_body (old format)
    if (!vectors.empty() && !vectors[0].contains("json_body")) {
        WARN("Test vectors missing json_body field — regenerate with "
             "the signing test vectors generator");
        SKIP();
    }

    std::string hex_key = data["private_key"].get<std::string>();
    auto privkey = Secp256k1Signer::parse_private_key(hex_key);

    std::string maker_address = data["maker_address"].get<std::string>();
    std::string signer_address = data["signer_address"].get<std::string>();
    constexpr auto ZERO_ADDR =
        "0x0000000000000000000000000000000000000000";

    int total = vectors.size();
    int passed = 0;
    int failed = 0;

    for (int i = 0; i < total; i++) {
        const auto& v = vectors[i];

        // Recompute amounts
        std::string side = v["side"].get<std::string>();
        double price = v["price"].get<double>();
        double size = v["size"].get<double>();
        std::string tick_size = v["tick_size"].get<std::string>();
        uint64_t salt = v["salt"].get<uint64_t>();
        int fee_rate_bps = v["fee_rate_bps"].get<int>();
        std::string token_id = v["token_id"].get<std::string>();
        bool neg_risk = v["neg_risk"].get<bool>();

        auto config = TestAmounts::get_round_config(tick_size);
        auto amounts =
            TestAmounts::calculate_order_amounts(side, size, price, config);

        // Hash and sign (same as production code)
        EIP712::OrderFields fields;
        fields.salt = salt;
        fields.maker = maker_address;
        fields.signer = signer_address;
        fields.taker = ZERO_ADDR;
        fields.token_id = token_id;
        fields.maker_amount = amounts.maker_amount;
        fields.taker_amount = amounts.taker_amount;
        fields.expiration = 0;
        fields.nonce = 0;
        fields.fee_rate_bps = static_cast<uint64_t>(fee_rate_bps);
        fields.side = amounts.side;
        fields.signature_type = 0;

        auto hash = EIP712::hash_order(fields, neg_risk);
        auto signature =
            Secp256k1Signer::sign_hash_hex(hash, privkey.data());

        // Build JSON body exactly as polymarket_client.cpp does
        nlohmann::ordered_json order_json;
        order_json["salt"] = salt;
        order_json["maker"] = maker_address;
        order_json["signer"] = signer_address;
        order_json["taker"] = std::string(ZERO_ADDR);
        order_json["tokenId"] = token_id;
        order_json["makerAmount"] = std::to_string(amounts.maker_amount);
        order_json["takerAmount"] = std::to_string(amounts.taker_amount);
        order_json["expiration"] = "0";
        order_json["nonce"] = "0";
        order_json["feeRateBps"] = std::to_string(fee_rate_bps);
        order_json["side"] = side;
        order_json["signatureType"] = 0;
        order_json["signature"] = signature;

        nlohmann::ordered_json body;
        body["order"] = order_json;
        body["owner"] = "test_api_key";
        body["orderType"] = "GTC";
        body["postOnly"] = false;

        std::string cpp_json = body.dump(-1, ' ', false,
            nlohmann::ordered_json::error_handler_t::replace);

        std::string expected_json = v["json_body"].get<std::string>();

        if (cpp_json == expected_json) {
            passed++;
        } else {
            failed++;
            if (failed <= 10) {
                std::cout << "\n[JSON MISMATCH] vector #" << i << ":"
                          << "\n  salt=" << salt << " side=" << side
                          << " price=" << price << " size=" << size
                          << " tick=" << tick_size;

                // Find first divergent byte
                size_t min_len =
                    std::min(cpp_json.size(), expected_json.size());
                for (size_t b = 0; b < min_len; b++) {
                    if (cpp_json[b] != expected_json[b]) {
                        size_t ctx_start = (b > 20) ? b - 20 : 0;
                        size_t ctx_len = std::min<size_t>(60, cpp_json.size() - ctx_start);
                        std::cout << "\n  First diff at byte " << b << ":"
                                  << "\n  C++: ..."
                                  << cpp_json.substr(ctx_start, ctx_len) << "..."
                                  << "\n  Py:  ..."
                                  << expected_json.substr(
                                         ctx_start,
                                         std::min<size_t>(60, expected_json.size() - ctx_start))
                                  << "...";
                        break;
                    }
                }
                if (cpp_json.size() != expected_json.size()) {
                    std::cout << "\n  Length: C++=" << cpp_json.size()
                              << " Py=" << expected_json.size();
                }
                std::cout << std::endl;
            }
        }
    }

    std::cout << "\n=== JSON Body Comparison Results ===" << std::endl;
    std::cout << "Total: " << total << " | Passed: " << passed
              << " | Failed: " << failed << std::endl;

    REQUIRE(failed == 0);
}
