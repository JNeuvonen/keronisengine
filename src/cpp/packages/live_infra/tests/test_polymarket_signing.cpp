#include <catch2/catch_all.hpp>

#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "crypto/base64_utils.h"
#include "crypto/eip712.h"
#include "crypto/keccak256.h"
#include "crypto/secp256k1_signer.h"
#include "trading/polymarket_client.h"

// ──────────────────────────────────────────────────────────────────────────
// Keccak-256 known vectors
// ──────────────────────────────────────────────────────────────────────────

TEST_CASE("Keccak256 empty string", "[polymarket][crypto]") {
    auto hash = Keccak::keccak256(std::string(""));
    auto hex = Keccak::to_hex(hash);
    REQUIRE(hex ==
            "c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470");
}

TEST_CASE("Keccak256 'abc'", "[polymarket][crypto]") {
    auto hash = Keccak::keccak256(std::string("abc"));
    auto hex = Keccak::to_hex(hash);
    REQUIRE(hex ==
            "4e03657aea45a94fc7d47ba826c8d667c0d1e6e33a64a036ec44f58fa12d6c45");
}

TEST_CASE("Keccak256 EIP-712 type string", "[polymarket][crypto]") {
    std::string type_str =
        "Order(uint256 salt,address maker,address signer,address taker,"
        "uint256 tokenId,uint256 makerAmount,uint256 takerAmount,"
        "uint256 expiration,uint256 nonce,uint256 feeRateBps,"
        "uint8 side,uint8 signatureType)";
    auto hash = Keccak::keccak256(type_str);
    auto hex = Keccak::to_hex(hash);

    // Verify type hash is consistent (used by EIP712::order_type_hash())
    auto type_hash = EIP712::order_type_hash();
    REQUIRE(hash == type_hash);
}

// ──────────────────────────────────────────────────────────────────────────
// Hex encoding/decoding
// ──────────────────────────────────────────────────────────────────────────

TEST_CASE("Hex round-trip", "[polymarket][crypto]") {
    std::string original = "0xdeadbeef01020304";
    auto bytes = Keccak::from_hex(original);
    REQUIRE(bytes.size() == 8);
    REQUIRE(bytes[0] == 0xde);
    REQUIRE(bytes[1] == 0xad);
    REQUIRE(bytes[7] == 0x04);
}

// ──────────────────────────────────────────────────────────────────────────
// Base64 encoding/decoding
// ──────────────────────────────────────────────────────────────────────────

TEST_CASE("Base64 standard round-trip", "[polymarket][crypto]") {
    std::string input = "Hello, World!";
    auto encoded = Base64Utils::encode(input);
    REQUIRE(encoded == "SGVsbG8sIFdvcmxkIQ==");

    auto decoded = Base64Utils::decode(encoded);
    std::string decoded_str(decoded.begin(), decoded.end());
    REQUIRE(decoded_str == input);
}

TEST_CASE("Base64 URL-safe round-trip", "[polymarket][crypto]") {
    // Test with data that produces +/ in standard base64
    uint8_t data[] = {0xfb, 0xef, 0xbe};
    auto encoded = Base64Utils::encode_urlsafe(data, 3);
    // Standard would be "+++"  -> URL-safe is "---" (no padding needed here)
    REQUIRE(encoded.find('+') == std::string::npos);
    REQUIRE(encoded.find('/') == std::string::npos);
    REQUIRE(encoded.find('=') == std::string::npos);

    auto decoded = Base64Utils::decode_urlsafe(encoded);
    REQUIRE(decoded.size() == 3);
    REQUIRE(decoded[0] == 0xfb);
    REQUIRE(decoded[1] == 0xef);
    REQUIRE(decoded[2] == 0xbe);
}

// ──────────────────────────────────────────────────────────────────────────
// EIP-712 encoding
// ──────────────────────────────────────────────────────────────────────────

TEST_CASE("EIP-712 uint256 encoding", "[polymarket][eip712]") {
    auto encoded = EIP712::encode_uint256(1);
    // Should be 31 zero bytes + 0x01
    for (int i = 0; i < 31; i++) {
        REQUIRE(encoded[i] == 0);
    }
    REQUIRE(encoded[31] == 1);
}

TEST_CASE("EIP-712 uint256 from string encoding", "[polymarket][eip712]") {
    // Test with a large Polymarket token ID
    auto encoded = EIP712::encode_uint256_from_string("12345678901234567890");
    // Verify it round-trips: convert back to check
    // 12345678901234567890 = 0xAB54A98CEB1F0AD2
    REQUIRE(encoded[24] == 0xAB);
    REQUIRE(encoded[25] == 0x54);
    REQUIRE(encoded[26] == 0xA9);
    REQUIRE(encoded[27] == 0x8C);
    REQUIRE(encoded[28] == 0xEB);
    REQUIRE(encoded[29] == 0x1F);
    REQUIRE(encoded[30] == 0x0A);
    REQUIRE(encoded[31] == 0xD2);
}

TEST_CASE("EIP-712 address encoding", "[polymarket][eip712]") {
    auto encoded =
        EIP712::encode_address("0x0000000000000000000000000000000000000000");
    // All 32 bytes should be zero
    for (int i = 0; i < 32; i++) {
        REQUIRE(encoded[i] == 0);
    }

    // Non-zero address (20 bytes = 40 hex digits): last 20 bytes after 12 zero
    // bytes
    auto encoded2 =
        EIP712::encode_address("0xdead00000000000000000000000000000000beef");
    for (int i = 0; i < 12; i++) {
        REQUIRE(encoded2[i] == 0);
    }
    REQUIRE(encoded2[12] == 0xde);
    REQUIRE(encoded2[13] == 0xad);
    REQUIRE(encoded2[30] == 0xbe);
    REQUIRE(encoded2[31] == 0xef);
}

TEST_CASE("EIP-712 domain separator consistency", "[polymarket][eip712]") {
    // Domain separators should be deterministic
    auto normal1 = EIP712::normal_domain_separator();
    auto normal2 = EIP712::normal_domain_separator();
    REQUIRE(normal1 == normal2);

    auto neg1 = EIP712::neg_risk_domain_separator();
    auto neg2 = EIP712::neg_risk_domain_separator();
    REQUIRE(neg1 == neg2);

    // Normal and neg_risk separators should be different
    REQUIRE(normal1 != neg1);
}

TEST_CASE("EIP-712 struct hash determinism", "[polymarket][eip712]") {
    EIP712::OrderFields fields;
    fields.salt = 123456;
    fields.maker = "0x4bFb41d5B3570DeFd03C39a9A4D8dE6Bd8B8982E";
    fields.signer = "0x4bFb41d5B3570DeFd03C39a9A4D8dE6Bd8B8982E";
    fields.taker = "0x0000000000000000000000000000000000000000";
    fields.token_id = "1234567890";
    fields.maker_amount = 1000000;
    fields.taker_amount = 500000;
    fields.expiration = 0;
    fields.nonce = 0;
    fields.fee_rate_bps = 100;
    fields.side = 0;
    fields.signature_type = 0;

    auto hash1 = EIP712::compute_struct_hash(fields);
    auto hash2 = EIP712::compute_struct_hash(fields);
    REQUIRE(hash1 == hash2);

    // Changing any field should change the hash
    fields.salt = 654321;
    auto hash3 = EIP712::compute_struct_hash(fields);
    REQUIRE(hash1 != hash3);
}

TEST_CASE("EIP-712 full order hash", "[polymarket][eip712]") {
    EIP712::OrderFields fields;
    fields.salt = 1;
    fields.maker = "0x4bFb41d5B3570DeFd03C39a9A4D8dE6Bd8B8982E";
    fields.signer = "0x4bFb41d5B3570DeFd03C39a9A4D8dE6Bd8B8982E";
    fields.taker = "0x0000000000000000000000000000000000000000";
    fields.token_id = "100";
    fields.maker_amount = 50000000;
    fields.taker_amount = 100000000;
    fields.expiration = 0;
    fields.nonce = 0;
    fields.fee_rate_bps = 0;
    fields.side = 0;
    fields.signature_type = 0;

    // Both normal and neg_risk should produce different hashes
    auto hash_normal = EIP712::hash_order(fields, false);
    auto hash_neg = EIP712::hash_order(fields, true);
    REQUIRE(hash_normal != hash_neg);

    // Should be deterministic
    auto hash_normal2 = EIP712::hash_order(fields, false);
    REQUIRE(hash_normal == hash_normal2);
}

// ──────────────────────────────────────────────────────────────────────────
// CLOB v2 (pUSD) order — parity against eth_account encode_typed_data
// ──────────────────────────────────────────────────────────────────────────

TEST_CASE("EIP-712 v2 order hash parity", "[polymarket][eip712][v2]") {
    // Reference values computed with eth_account encode_typed_data for the
    // v2 Order struct (domain version "2", exchange_v2 verifying contract).
    constexpr auto kDomainSep =
        "3264e159346253e26a64e00b69032db0e7d32f94628de3e6eecb50304d7af3d2";
    constexpr auto kStructHash =
        "d4498f316f2e77e39881701b9e00690cba07153091d3e8c290bdfcedbb47bd37";
    constexpr auto kDigest =
        "b6ae1a28139e6f855568cbfc655094043115224d025cf293e0f44015bdb126cc";

    EIP712::OrderFieldsV2 fields;
    fields.salt = 12345678901234ULL;
    fields.maker = "0xf39Fd6e51aad88F6F4ce6aB8827279cffFb92266";
    fields.signer = "0xf39Fd6e51aad88F6F4ce6aB8827279cffFb92266";
    fields.token_id =
        "104812032440315568595318418705900669999999999999999999999999999999999999";
    fields.maker_amount = 50000;
    fields.taker_amount = 5000000;
    fields.side = 0;
    fields.signature_type = 0;
    fields.timestamp = 1781461555123ULL;
    fields.metadata =
        "0x0000000000000000000000000000000000000000000000000000000000000000";
    fields.builder =
        "0x0000000000000000000000000000000000000000000000000000000000000000";

    REQUIRE(Keccak::to_hex(EIP712::normal_domain_separator_v2()) == kDomainSep);
    REQUIRE(Keccak::to_hex(EIP712::compute_struct_hash_v2(fields)) ==
            kStructHash);
    REQUIRE(Keccak::to_hex(EIP712::hash_order_v2(fields, false)) == kDigest);

    // neg_risk must differ; result deterministic
    REQUIRE(EIP712::hash_order_v2(fields, true) !=
            EIP712::hash_order_v2(fields, false));
    REQUIRE(EIP712::hash_order_v2(fields, false) ==
            EIP712::hash_order_v2(fields, false));
}

TEST_CASE("v2 order body byte-for-byte parity with Python CLOB",
          "[polymarket][eip712][v2]") {
    // Reference produced by py-clob-client-v2 order_to_json_v2 (via eth_account)
    // for the hardhat #0 test key (public). Asserts the FULL serialized payload
    // — field order, value types, outer wrapper, AND the deterministic ECDSA
    // signature — is byte-for-byte identical to the Python CLOB client.
    const std::string kReference =
        R"({"order":{"salt":12345678901234,"maker":"0xf39Fd6e51aad88F6F4ce6aB8827279cffFb92266","signer":"0xf39Fd6e51aad88F6F4ce6aB8827279cffFb92266","tokenId":"104812032440315568595318418705900669999999999999999999999999999999999999","makerAmount":"50000","takerAmount":"5000000","side":"BUY","expiration":"0","signatureType":0,"timestamp":"1781461555123","metadata":"0x0000000000000000000000000000000000000000000000000000000000000000","builder":"0x0000000000000000000000000000000000000000000000000000000000000000","signature":"0x3ebe4ab54ec360b018c305b78209bd082f66073fad8d1998e5d9f99d8353a0d705f91584da6dbc20e5320a60edecb4e7f910f24db26eee0ef20871b69ce9c3351c"},"owner":"test-owner-key","orderType":"GTC","deferExec":false,"postOnly":false})";

    auto key = Secp256k1Signer::parse_private_key(
        "0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80");

    EIP712::OrderFieldsV2 fields;
    fields.salt = 12345678901234ULL;
    fields.maker = "0xf39Fd6e51aad88F6F4ce6aB8827279cffFb92266";
    fields.signer = "0xf39Fd6e51aad88F6F4ce6aB8827279cffFb92266";
    fields.token_id =
        "104812032440315568595318418705900669999999999999999999999999999999999999";
    fields.maker_amount = 50000;
    fields.taker_amount = 5000000;
    fields.side = 0;
    fields.signature_type = 0;
    fields.timestamp = 1781461555123ULL;
    fields.metadata =
        "0x0000000000000000000000000000000000000000000000000000000000000000";
    fields.builder =
        "0x0000000000000000000000000000000000000000000000000000000000000000";

    auto signed_body = PolymarketDirect::PolymarketClient::build_v2_order_body(
        key, "test-owner-key", "BUY", fields, /*neg_risk=*/false, "GTC",
        /*post_only=*/false);

    REQUIRE(signed_body.body.dump() == kReference);
    REQUIRE(signed_body.order_id ==
            "0xb6ae1a28139e6f855568cbfc655094043115224d025cf293e0f44015bdb126cc");
}

// ──────────────────────────────────────────────────────────────────────────
// Secp256k1 signing
// ──────────────────────────────────────────────────────────────────────────

TEST_CASE("Secp256k1 private key parsing", "[polymarket][crypto]") {
    // Known test private key (DO NOT use in production)
    std::string hex_key =
        "0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80";
    auto parsed = Secp256k1Signer::parse_private_key(hex_key);
    REQUIRE(parsed[0] == 0xac);
    REQUIRE(parsed[31] == 0x80);
}

TEST_CASE("Secp256k1 derive address from known key", "[polymarket][crypto]") {
    // Hardhat account #0 private key
    std::string hex_key =
        "0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80";
    auto privkey = Secp256k1Signer::parse_private_key(hex_key);
    auto address = Secp256k1Signer::derive_address(privkey.data());
    // Hardhat account #0 address (checksummed)
    REQUIRE(address == "0xf39Fd6e51aad88F6F4ce6aB8827279cffFb92266");
}

TEST_CASE("Secp256k1 sign and verify determinism", "[polymarket][crypto]") {
    std::string hex_key =
        "0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80";
    auto privkey = Secp256k1Signer::parse_private_key(hex_key);

    // Hash some data
    auto hash = Keccak::keccak256(std::string("test message"));

    // Sign
    auto sig_hex = Secp256k1Signer::sign_hash_hex(hash, privkey.data());

    // Should start with 0x and be 132 chars (0x + 65 bytes * 2)
    REQUIRE(sig_hex.substr(0, 2) == "0x");
    REQUIRE(sig_hex.size() == 132);

    // Signing the same hash with the same key should produce the same
    // signature (libsecp256k1 uses RFC 6979 deterministic nonces)
    auto sig_hex2 = Secp256k1Signer::sign_hash_hex(hash, privkey.data());
    REQUIRE(sig_hex == sig_hex2);

    // Recovery byte v should be 27 or 28
    auto v_hex = sig_hex.substr(130, 2);
    REQUIRE((v_hex == "1b" || v_hex == "1c"));
}

// ──────────────────────────────────────────────────────────────────────────
// EIP-712 + Secp256k1 end-to-end signing
// ──────────────────────────────────────────────────────────────────────────

// ──────────────────────────────────────────────────────────────────────────
// Cross-language comparison: use EXACT same inputs as
// scripts/utilities/polymarkets/compare_signing_python.py
// ──────────────────────────────────────────────────────────────────────────

TEST_CASE("Cross-language signing comparison", "[polymarket][compare]") {
    // ─── Same hardcoded inputs as Python script ───────────────────────
    std::string hex_key =
        "0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80";
    auto privkey = Secp256k1Signer::parse_private_key(hex_key);
    auto address = Secp256k1Signer::derive_address(privkey.data());

    std::string token_id =
        "48772698317422898244783688259042608092301975100035649843918746286874"
        "277738819";
    double price = 0.55;
    double size = 10.0;
    std::string side_str = "BUY";
    std::string tick_size = "0.01";
    bool neg_risk = false;
    int fee_rate_bps = 0;
    uint64_t fixed_salt = 1234567890;

    // ─── Step 1: Amount calculation (inline, matching polymarket_client.cpp) ──
    // For tick_size "0.01": price=2, size=2, amount=4
    int rnd_price = 2, rnd_size = 2, rnd_amount = 4;

    std::cout << "\n=== C++ SIGNING COMPARISON ===" << std::endl;
    std::cout << "\n--- Round Config ---" << std::endl;
    std::cout << "price=" << rnd_price << ", size=" << rnd_size
              << ", amount=" << rnd_amount << std::endl;

    // BUY: taker = round_down(size), maker = taker * price
    auto round_normal = [](double x, int d) {
        double f = std::pow(10.0, d);
        return std::round(x * f) / f;
    };
    auto round_down = [](double x, int d) {
        double f = std::pow(10.0, d);
        return std::floor(x * f) / f;
    };
    auto decimal_places_fn = [](double x) -> int {
        char buf[32];
        auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), x);
        if (ec != std::errc()) return 0;
        std::string_view sv(buf, ptr - buf);
        auto dot = sv.find('.');
        if (dot == std::string_view::npos) return 0;
        auto last_nonzero = sv.find_last_not_of('0');
        if (last_nonzero <= dot) return 0;
        return static_cast<int>(last_nonzero - dot);
    };
    auto to_token_decimals = [](double x) -> uint64_t {
        return static_cast<uint64_t>(std::round(1000000.0 * x));
    };

    double raw_price = round_normal(price, rnd_price);
    double raw_taker_amt = round_down(size, rnd_size);
    double raw_maker_amt = raw_taker_amt * raw_price;

    std::cout << "raw_price = " << raw_price << std::endl;
    std::cout << "raw_taker_amt = " << raw_taker_amt << std::endl;
    std::cout << "raw_maker_amt = " << raw_maker_amt << std::endl;

    if (decimal_places_fn(raw_maker_amt) > rnd_amount) {
        auto round_up = [](double x, int d) {
            double f = std::pow(10.0, d);
            return std::ceil(x * f) / f;
        };
        raw_maker_amt = round_up(raw_maker_amt, rnd_amount + 4);
        if (decimal_places_fn(raw_maker_amt) > rnd_amount) {
            raw_maker_amt = round_down(raw_maker_amt, rnd_amount);
        }
    }

    uint8_t side_int = 0;
    uint64_t maker_amount = to_token_decimals(raw_maker_amt);
    uint64_t taker_amount = to_token_decimals(raw_taker_amt);

    std::cout << "\n--- Token Decimals ---" << std::endl;
    std::cout << "side_int = " << (int)side_int << std::endl;
    std::cout << "maker_amount = " << maker_amount << std::endl;
    std::cout << "taker_amount = " << taker_amount << std::endl;

    // Python reference values
    REQUIRE(side_int == 0);
    REQUIRE(maker_amount == 5500000);
    REQUIRE(taker_amount == 10000000);

    // ─── Step 2: EIP-712 hashing ──────────────────────────────────────
    EIP712::OrderFields fields;
    fields.salt = fixed_salt;
    fields.maker = address;
    fields.signer = address;
    fields.taker = "0x0000000000000000000000000000000000000000";
    fields.token_id = token_id;
    fields.maker_amount = maker_amount;
    fields.taker_amount = taker_amount;
    fields.expiration = 0;
    fields.nonce = 0;
    fields.fee_rate_bps = static_cast<uint64_t>(fee_rate_bps);
    fields.side = side_int;
    fields.signature_type = 0;

    std::cout << "\n--- OrderFields ---" << std::endl;
    std::cout << "salt = " << fields.salt << std::endl;
    std::cout << "maker = " << fields.maker << std::endl;
    std::cout << "signer = " << fields.signer << std::endl;
    std::cout << "taker = " << fields.taker << std::endl;
    std::cout << "tokenId = " << fields.token_id << std::endl;
    std::cout << "maker_amount = " << fields.maker_amount << std::endl;
    std::cout << "taker_amount = " << fields.taker_amount << std::endl;

    // Domain separator
    const auto& domain_sep = EIP712::normal_domain_separator();
    std::cout << "\n--- EIP-712 Hashes ---" << std::endl;
    std::cout << "domain_separator_hash = 0x" << Keccak::to_hex(domain_sep)
              << std::endl;

    // Python reference
    REQUIRE(("0x" + Keccak::to_hex(domain_sep)) ==
            "0x1a573e3617c78403b5b4b892827992f027b03d4eaf570048b8ee8cdd84d151be");

    // Struct hash
    auto struct_hash = EIP712::compute_struct_hash(fields);
    std::cout << "struct_hash = 0x" << Keccak::to_hex(struct_hash) << std::endl;

    // Full EIP-712 hash (order_id)
    auto order_hash = EIP712::compute_eip712_hash(domain_sep, struct_hash);
    auto order_id = "0x" + Keccak::to_hex(order_hash);
    std::cout << "order_id = " << order_id << std::endl;

    // Python reference: struct_hash (order_id) from the Python script
    // Note: Python's "struct_hash (order_id)" is actually the FINAL eip712 hash
    REQUIRE(order_id ==
            "0x257add85681f1e403d54c7c5324e27d110dcd01f68131e5d2b10f64fc3118b61");

    // ─── Step 3: Signature ────────────────────────────────────────────
    auto sig_hex =
        Secp256k1Signer::sign_hash_hex(order_hash, privkey.data());
    std::cout << "\n--- Signature ---" << std::endl;
    std::cout << "signature = " << sig_hex << std::endl;
    std::cout << "signature_len = " << sig_hex.size() << std::endl;

    // Python reference
    REQUIRE(sig_hex ==
            "0xf9d4ed7f5a16ce8703259ea3e2ddc9f967b4d298a0f76667442c682bce5d177e"
            "0b0c29c60293f5f372da39fa0b4bf03145a8de109567e21d84c7637607e7bd66"
            "1b");

    // ─── Step 4: JSON body ────────────────────────────────────────────
    nlohmann::ordered_json order_json;
    order_json["salt"] = fixed_salt;
    order_json["maker"] = address;
    order_json["signer"] = address;
    order_json["taker"] = std::string("0x0000000000000000000000000000000000000000");
    order_json["tokenId"] = token_id;
    order_json["makerAmount"] = std::to_string(maker_amount);
    order_json["takerAmount"] = std::to_string(taker_amount);
    order_json["expiration"] = std::string("0");
    order_json["nonce"] = std::string("0");
    order_json["feeRateBps"] = std::to_string(fee_rate_bps);
    order_json["side"] = side_str;
    order_json["signatureType"] = 0;
    order_json["signature"] = sig_hex;

    nlohmann::ordered_json body;
    body["order"] = order_json;
    body["owner"] = std::string("test_api_key");
    body["orderType"] = std::string("GTC");
    body["postOnly"] = false;

    std::string serialized = body.dump(-1, ' ', false,
        nlohmann::json::error_handler_t::replace);
    std::cout << "\n--- Serialized JSON Body ---" << std::endl;
    std::cout << serialized << std::endl;
    std::cout << "JSON length = " << serialized.size() << std::endl;

    // Compare JSON field-by-field with Python output
    // Python: {"order":{"salt":1234567890,...},...}
    // Check key structural differences
    REQUIRE(body["order"]["salt"].is_number());
    REQUIRE(body["order"]["signatureType"].is_number());
    REQUIRE(body["order"]["makerAmount"].is_string());
    REQUIRE(body["order"]["takerAmount"].is_string());
    REQUIRE(body["postOnly"].is_boolean());
}

TEST_CASE("End-to-end order signing", "[polymarket][e2e]") {
    // Use Hardhat account #0
    std::string hex_key =
        "0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80";
    auto privkey = Secp256k1Signer::parse_private_key(hex_key);
    auto address = Secp256k1Signer::derive_address(privkey.data());

    EIP712::OrderFields fields;
    fields.salt = 42;
    fields.maker = address;
    fields.signer = address;
    fields.taker = "0x0000000000000000000000000000000000000000";
    fields.token_id = "12345";
    fields.maker_amount = 5000000;   // 5 USDC
    fields.taker_amount = 10000000;  // 10 contracts
    fields.expiration = 0;
    fields.nonce = 0;
    fields.fee_rate_bps = 100;
    fields.side = 0;  // BUY
    fields.signature_type = 0;

    auto order_hash = EIP712::hash_order(fields, false);
    auto sig_hex =
        Secp256k1Signer::sign_hash_hex(order_hash, privkey.data());

    REQUIRE(sig_hex.size() == 132);
    REQUIRE(sig_hex.substr(0, 2) == "0x");

    // Signing the same order twice should produce the same signature
    auto sig_hex2 =
        Secp256k1Signer::sign_hash_hex(order_hash, privkey.data());
    REQUIRE(sig_hex == sig_hex2);
}
