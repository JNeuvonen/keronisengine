#pragma once

// EIP-712 encoding for Polymarket Order struct
// Ports signing logic from py_order_utils/builders/base_builder.py

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "crypto/keccak256.h"

namespace EIP712 {

// ABI-encode a uint256 (left-padded to 32 bytes, big-endian)
inline std::array<uint8_t, 32> encode_uint256(uint64_t value) {
    std::array<uint8_t, 32> result = {};
    for (int i = 31; i >= 0 && value > 0; i--) {
        result[i] = static_cast<uint8_t>(value & 0xff);
        value >>= 8;
    }
    return result;
}

// ABI-encode a uint256 from a decimal string (for large token IDs)
inline std::array<uint8_t, 32> encode_uint256_from_string(
    const std::string& decimal_str) {
    // Simple big-integer decimal-to-binary conversion
    // Polymarket token IDs can exceed uint64_t range
    std::array<uint8_t, 32> result = {};

    // Work with the decimal string digit by digit
    std::vector<uint8_t> digits;
    for (char c : decimal_str) {
        if (c >= '0' && c <= '9') {
            digits.push_back(c - '0');
        }
    }

    if (digits.empty()) return result;

    // Repeatedly divide by 256 to extract bytes from LSB to MSB
    std::vector<uint8_t> bytes;
    while (!digits.empty()) {
        // Remove leading zeros
        while (!digits.empty() && digits[0] == 0) {
            digits.erase(digits.begin());
        }
        if (digits.empty()) break;

        // Divide all digits by 256, collecting remainder
        uint32_t remainder = 0;
        std::vector<uint8_t> quotient;
        for (uint8_t d : digits) {
            remainder = remainder * 10 + d;
            quotient.push_back(remainder / 256);
            remainder %= 256;
        }
        bytes.push_back(static_cast<uint8_t>(remainder));
        digits = quotient;
    }

    // Copy bytes into result (right-aligned, big-endian)
    for (size_t i = 0; i < bytes.size() && i < 32; i++) {
        result[31 - i] = bytes[i];
    }
    return result;
}

// ABI-encode an Ethereum address (left-padded to 32 bytes)
inline std::array<uint8_t, 32> encode_address(const std::string& address) {
    auto addr_bytes = Keccak::from_hex(address);
    std::array<uint8_t, 32> result = {};
    // Address is 20 bytes, placed at offset 12
    size_t start = 32 - addr_bytes.size();
    for (size_t i = 0; i < addr_bytes.size() && i + start < 32; i++) {
        result[start + i] = addr_bytes[i];
    }
    return result;
}

// ABI-encode a uint8 (left-padded to 32 bytes)
inline std::array<uint8_t, 32> encode_uint8(uint8_t value) {
    std::array<uint8_t, 32> result = {};
    result[31] = value;
    return result;
}

// ABI-encode a bytes32 from a 0x-hex string (left-aligned, as ABI bytes32).
// Used for the v2 metadata/builder fields, which are full 32-byte values
// (typically all-zero).
inline std::array<uint8_t, 32> encode_bytes32(const std::string& hex) {
    std::array<uint8_t, 32> result = {};
    auto bytes = Keccak::from_hex(hex);
    for (size_t i = 0; i < bytes.size() && i < 32; i++) {
        result[i] = bytes[i];
    }
    return result;
}

// Precomputed type hash for the Order struct:
// keccak256("Order(uint256 salt,address maker,address signer,address
// taker,uint256 tokenId,uint256 makerAmount,uint256 takerAmount,uint256
// expiration,uint256 nonce,uint256 feeRateBps,uint8 side,uint8
// signatureType)")
inline std::array<uint8_t, 32> order_type_hash() {
    static const auto hash = Keccak::keccak256(
        "Order(uint256 salt,address maker,address signer,address taker,"
        "uint256 tokenId,uint256 makerAmount,uint256 takerAmount,"
        "uint256 expiration,uint256 nonce,uint256 feeRateBps,"
        "uint8 side,uint8 signatureType)");
    return hash;
}

// EIP-712 domain separator hash
// domain name: "Polymarket CTF Exchange", chainId: 137. The CTF Exchange v1
// uses version "1"; the v2 (pUSD) exchange uses version "2".
inline std::array<uint8_t, 32> compute_domain_separator(
    const std::string& verifying_contract, const std::string& version) {
    // EIP712Domain(string name,string version,uint256 chainId,address
    // verifyingContract)
    auto domain_type_hash = Keccak::keccak256(
        "EIP712Domain(string name,string version,uint256 chainId,address "
        "verifyingContract)");
    auto name_hash = Keccak::keccak256("Polymarket CTF Exchange");
    auto version_hash = Keccak::keccak256(version);
    auto chain_id = encode_uint256(137);
    auto contract_addr = encode_address(verifying_contract);

    // Concatenate: domainTypeHash + nameHash + versionHash + chainId +
    // verifyingContract
    std::vector<uint8_t> encoded;
    encoded.reserve(160);  // 5 * 32
    encoded.insert(encoded.end(), domain_type_hash.begin(),
                   domain_type_hash.end());
    encoded.insert(encoded.end(), name_hash.begin(), name_hash.end());
    encoded.insert(encoded.end(), version_hash.begin(), version_hash.end());
    encoded.insert(encoded.end(), chain_id.begin(), chain_id.end());
    encoded.insert(encoded.end(), contract_addr.begin(), contract_addr.end());

    return Keccak::keccak256(encoded);
}

// Domain separators for the two Polymarket v1 exchanges (version "1")
inline const std::array<uint8_t, 32>& normal_domain_separator() {
    static const auto sep = compute_domain_separator(
        "0x4bFb41d5B3570DeFd03C39a9A4D8dE6Bd8B8982E", "1");
    return sep;
}

inline const std::array<uint8_t, 32>& neg_risk_domain_separator() {
    static const auto sep = compute_domain_separator(
        "0xC5d563A36AE78145C45a50134d48A1215220f80a", "1");
    return sep;
}

// Domain separators for the v2 (pUSD) exchanges (version "2")
inline const std::array<uint8_t, 32>& normal_domain_separator_v2() {
    static const auto sep = compute_domain_separator(
        "0xE111180000d2663C0091e4f400237545B87B996B", "2");
    return sep;
}

inline const std::array<uint8_t, 32>& neg_risk_domain_separator_v2() {
    static const auto sep = compute_domain_separator(
        "0xe2222d279d744050d28e00520010520000310F59", "2");
    return sep;
}

struct OrderFields {
    uint64_t salt;
    std::string maker;       // checksummed address
    std::string signer;      // checksummed address
    std::string taker;       // checksummed address
    std::string token_id;    // decimal string (can be very large)
    uint64_t maker_amount;   // in token decimals (×10^6)
    uint64_t taker_amount;   // in token decimals (×10^6)
    uint64_t expiration;
    uint64_t nonce;
    uint64_t fee_rate_bps;
    uint8_t side;            // 0 = BUY, 1 = SELL
    uint8_t signature_type;  // 0 = EOA
};

// Compute struct hash: keccak256(abi.encode(ORDER_TYPE_HASH, ...fields))
inline std::array<uint8_t, 32> compute_struct_hash(const OrderFields& order) {
    std::vector<uint8_t> encoded;
    encoded.reserve(13 * 32);  // 13 fields (type hash + 12 fields)

    auto append = [&encoded](const std::array<uint8_t, 32>& data) {
        encoded.insert(encoded.end(), data.begin(), data.end());
    };

    append(order_type_hash());
    append(encode_uint256(order.salt));
    append(encode_address(order.maker));
    append(encode_address(order.signer));
    append(encode_address(order.taker));
    append(encode_uint256_from_string(order.token_id));
    append(encode_uint256(order.maker_amount));
    append(encode_uint256(order.taker_amount));
    append(encode_uint256(order.expiration));
    append(encode_uint256(order.nonce));
    append(encode_uint256(order.fee_rate_bps));
    append(encode_uint8(order.side));
    append(encode_uint8(order.signature_type));

    return Keccak::keccak256(encoded);
}

// Compute final EIP-712 hash: keccak256(0x19 || 0x01 || domainSeparator ||
// structHash)
inline std::array<uint8_t, 32> compute_eip712_hash(
    const std::array<uint8_t, 32>& domain_separator,
    const std::array<uint8_t, 32>& struct_hash) {
    std::vector<uint8_t> message;
    message.reserve(66);  // 2 + 32 + 32
    message.push_back(0x19);
    message.push_back(0x01);
    message.insert(message.end(), domain_separator.begin(),
                   domain_separator.end());
    message.insert(message.end(), struct_hash.begin(), struct_hash.end());
    return Keccak::keccak256(message);
}

// Full signing hash for an order
inline std::array<uint8_t, 32> hash_order(const OrderFields& order,
                                           bool neg_risk) {
    auto struct_hash = compute_struct_hash(order);
    const auto& domain_sep =
        neg_risk ? neg_risk_domain_separator() : normal_domain_separator();
    return compute_eip712_hash(domain_sep, struct_hash);
}

// ─── CLOB v2 (pUSD) order ────────────────────────────────────────────────
// The v2 order struct drops taker/expiration/nonce/feeRateBps from the signed
// payload and adds timestamp/metadata/builder. Domain version is "2".

// keccak256("Order(uint256 salt,address maker,address signer,uint256 tokenId,
// uint256 makerAmount,uint256 takerAmount,uint8 side,uint8 signatureType,
// uint256 timestamp,bytes32 metadata,bytes32 builder)")
inline std::array<uint8_t, 32> order_type_hash_v2() {
    static const auto hash = Keccak::keccak256(
        "Order(uint256 salt,address maker,address signer,uint256 tokenId,"
        "uint256 makerAmount,uint256 takerAmount,uint8 side,"
        "uint8 signatureType,uint256 timestamp,bytes32 metadata,"
        "bytes32 builder)");
    return hash;
}

struct OrderFieldsV2 {
    uint64_t salt;
    std::string maker;       // funder address (== signer for EOA)
    std::string signer;      // signing EOA
    std::string token_id;    // decimal string
    uint64_t maker_amount;   // ×10^6
    uint64_t taker_amount;   // ×10^6
    uint8_t side;            // 0 = BUY, 1 = SELL
    uint8_t signature_type;  // 0 = EOA, 1 = POLY_PROXY, 2 = POLY_GNOSIS_SAFE
    uint64_t timestamp;      // ms
    std::string metadata;    // 0x-hex bytes32 (typically zero)
    std::string builder;     // 0x-hex bytes32 (typically zero)
};

inline std::array<uint8_t, 32> compute_struct_hash_v2(
    const OrderFieldsV2& order) {
    std::vector<uint8_t> encoded;
    encoded.reserve(12 * 32);  // type hash + 11 fields

    auto append = [&encoded](const std::array<uint8_t, 32>& data) {
        encoded.insert(encoded.end(), data.begin(), data.end());
    };

    append(order_type_hash_v2());
    append(encode_uint256(order.salt));
    append(encode_address(order.maker));
    append(encode_address(order.signer));
    append(encode_uint256_from_string(order.token_id));
    append(encode_uint256(order.maker_amount));
    append(encode_uint256(order.taker_amount));
    append(encode_uint8(order.side));
    append(encode_uint8(order.signature_type));
    append(encode_uint256(order.timestamp));
    append(encode_bytes32(order.metadata));
    append(encode_bytes32(order.builder));

    return Keccak::keccak256(encoded);
}

inline std::array<uint8_t, 32> hash_order_v2(const OrderFieldsV2& order,
                                             bool neg_risk) {
    auto struct_hash = compute_struct_hash_v2(order);
    const auto& domain_sep = neg_risk ? neg_risk_domain_separator_v2()
                                      : normal_domain_separator_v2();
    return compute_eip712_hash(domain_sep, struct_hash);
}

}  // namespace EIP712
