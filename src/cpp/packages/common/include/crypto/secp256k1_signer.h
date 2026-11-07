#pragma once

// ECDSA secp256k1 signing with recovery ID
// Uses libsecp256k1 for signing, OpenSSL for public key -> address derivation

#include <secp256k1.h>
#include <secp256k1_recovery.h>

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "crypto/keccak256.h"

namespace Secp256k1Signer {

inline secp256k1_context* get_sign_context() {
    thread_local secp256k1_context* ctx =
        secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
    return ctx;
}

// Derive Ethereum address from a private key
inline std::string derive_address(const uint8_t privkey[32]) {
    secp256k1_context* ctx = get_sign_context();
    if (!ctx) {
        throw std::runtime_error("Failed to create secp256k1 context");
    }

    secp256k1_pubkey pubkey;
    if (!secp256k1_ec_pubkey_create(ctx, &pubkey, privkey)) {
        throw std::runtime_error("Invalid private key");
    }

    // Serialize uncompressed public key (65 bytes: 0x04 + x + y)
    uint8_t pub_serialized[65];
    size_t pub_len = 65;
    secp256k1_ec_pubkey_serialize(ctx, pub_serialized, &pub_len, &pubkey,
                                  SECP256K1_EC_UNCOMPRESSED);

    // Ethereum address = last 20 bytes of keccak256(pubkey[1:])
    auto hash = Keccak::keccak256(pub_serialized + 1, 64);

    // Format as checksummed address
    std::string addr_hex;
    addr_hex.reserve(40);
    static const char hex_chars[] = "0123456789abcdef";
    for (int i = 12; i < 32; i++) {
        addr_hex += hex_chars[hash[i] >> 4];
        addr_hex += hex_chars[hash[i] & 0x0f];
    }

    // EIP-55 checksum
    auto addr_hash = Keccak::keccak256(addr_hex);
    std::string checksummed = "0x";
    checksummed.reserve(42);
    for (size_t i = 0; i < 40; i++) {
        uint8_t hash_nibble =
            (i % 2 == 0) ? (addr_hash[i / 2] >> 4) : (addr_hash[i / 2] & 0x0f);
        if (addr_hex[i] >= 'a' && addr_hex[i] <= 'f' && hash_nibble >= 8) {
            checksummed += static_cast<char>(addr_hex[i] - 32);  // uppercase
        } else {
            checksummed += addr_hex[i];
        }
    }
    return checksummed;
}

// Sign a 32-byte hash with ECDSA + recovery ID
// Returns 65 bytes: r(32) || s(32) || v(1) where v = recid + 27
inline std::array<uint8_t, 65> sign_hash(const uint8_t hash[32],
                                          const uint8_t privkey[32]) {
    secp256k1_context* ctx = get_sign_context();
    if (!ctx) {
        throw std::runtime_error("Failed to create secp256k1 context");
    }

    secp256k1_ecdsa_recoverable_signature sig;
    if (!secp256k1_ecdsa_sign_recoverable(ctx, &sig, hash, privkey, nullptr,
                                           nullptr)) {
        throw std::runtime_error("ECDSA signing failed");
    }

    uint8_t output64[64];
    int recid;
    secp256k1_ecdsa_recoverable_signature_serialize_compact(ctx, output64,
                                                             &recid, &sig);

    // libsecp256k1 already produces canonical (low-s) signatures by default.
    // EIP-2 requires s to be in the lower half of the curve order.

    std::array<uint8_t, 65> result;
    std::memcpy(result.data(), output64, 64);
    result[64] = static_cast<uint8_t>(recid + 27);

    return result;
}

// Sign and return hex-encoded signature with 0x prefix
inline std::string sign_hash_hex(const std::array<uint8_t, 32>& hash,
                                  const uint8_t privkey[32]) {
    auto sig = sign_hash(hash.data(), privkey);
    std::string hex = "0x";
    hex.reserve(132);  // 0x + 130 hex chars
    static const char hex_chars[] = "0123456789abcdef";
    for (uint8_t byte : sig) {
        hex += hex_chars[byte >> 4];
        hex += hex_chars[byte & 0x0f];
    }
    return hex;
}

// Parse a hex private key string (with or without 0x prefix) into 32 bytes
inline std::array<uint8_t, 32> parse_private_key(const std::string& hex_key) {
    auto bytes = Keccak::from_hex(hex_key);
    if (bytes.size() != 32) {
        throw std::runtime_error(
            "Invalid private key length: expected 32 bytes, got " +
            std::to_string(bytes.size()));
    }
    std::array<uint8_t, 32> result;
    std::memcpy(result.data(), bytes.data(), 32);
    return result;
}

}  // namespace Secp256k1Signer
