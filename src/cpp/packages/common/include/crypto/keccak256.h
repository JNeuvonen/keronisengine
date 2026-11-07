#pragma once

// Keccak-256 implementation (header-only)
// Implements the Keccak-f[1600] sponge construction with rate=1088, capacity=512
// Produces 32-byte (256-bit) digests
// Test vector: keccak256("") = c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace Keccak {

namespace detail {

static constexpr uint64_t RC[24] = {
    0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL,
    0x8000000080008000ULL, 0x000000000000808bULL, 0x0000000080000001ULL,
    0x8000000080008081ULL, 0x8000000000008009ULL, 0x000000000000008aULL,
    0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000aULL,
    0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL,
    0x8000000000008003ULL, 0x8000000000008002ULL, 0x8000000000000080ULL,
    0x000000000000800aULL, 0x800000008000000aULL, 0x8000000080008081ULL,
    0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL,
};

static constexpr int ROTATIONS[5][5] = {
    {0, 1, 62, 28, 27},
    {36, 44, 6, 55, 20},
    {3, 10, 43, 25, 39},
    {41, 45, 15, 21, 8},
    {18, 2, 61, 56, 14},
};

inline uint64_t rotl64(uint64_t x, int n) {
    return (x << n) | (x >> (64 - n));
}

inline void keccak_f1600(uint64_t state[25]) {
    for (int round = 0; round < 24; round++) {
        // Theta
        uint64_t C[5];
        for (int x = 0; x < 5; x++) {
            C[x] = state[x] ^ state[x + 5] ^ state[x + 10] ^ state[x + 15] ^
                    state[x + 20];
        }
        uint64_t D[5];
        for (int x = 0; x < 5; x++) {
            D[x] = C[(x + 4) % 5] ^ rotl64(C[(x + 1) % 5], 1);
        }
        for (int x = 0; x < 5; x++) {
            for (int y = 0; y < 5; y++) {
                state[x + 5 * y] ^= D[x];
            }
        }

        // Rho and Pi
        uint64_t B[25];
        for (int x = 0; x < 5; x++) {
            for (int y = 0; y < 5; y++) {
                B[y + 5 * ((2 * x + 3 * y) % 5)] =
                    rotl64(state[x + 5 * y], ROTATIONS[y][x]);
            }
        }

        // Chi
        for (int x = 0; x < 5; x++) {
            for (int y = 0; y < 5; y++) {
                state[x + 5 * y] =
                    B[x + 5 * y] ^
                    ((~B[(x + 1) % 5 + 5 * y]) & B[(x + 2) % 5 + 5 * y]);
            }
        }

        // Iota
        state[0] ^= RC[round];
    }
}

}  // namespace detail

// Keccak-256 hash of arbitrary data
inline std::array<uint8_t, 32> keccak256(const uint8_t* data, size_t len) {
    constexpr size_t RATE = 136;  // 1088 bits / 8
    uint64_t state[25] = {};

    // Absorb
    size_t offset = 0;
    while (offset + RATE <= len) {
        for (size_t i = 0; i < RATE / 8; i++) {
            uint64_t lane;
            std::memcpy(&lane, data + offset + i * 8, 8);
            state[i] ^= lane;
        }
        detail::keccak_f1600(state);
        offset += RATE;
    }

    // Pad last block
    uint8_t pad[RATE] = {};
    size_t remaining = len - offset;
    if (remaining > 0) {
        std::memcpy(pad, data + offset, remaining);
    }
    pad[remaining] = 0x01;  // Keccak padding (NOT SHA-3 which uses 0x06)
    pad[RATE - 1] |= 0x80;

    for (size_t i = 0; i < RATE / 8; i++) {
        uint64_t lane;
        std::memcpy(&lane, pad + i * 8, 8);
        state[i] ^= lane;
    }
    detail::keccak_f1600(state);

    // Squeeze
    std::array<uint8_t, 32> hash;
    std::memcpy(hash.data(), state, 32);
    return hash;
}

inline std::array<uint8_t, 32> keccak256(const std::string& data) {
    return keccak256(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

inline std::array<uint8_t, 32> keccak256(const std::vector<uint8_t>& data) {
    return keccak256(data.data(), data.size());
}

// Convert hash to hex string
inline std::string to_hex(const std::array<uint8_t, 32>& hash) {
    static const char hex_chars[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (uint8_t byte : hash) {
        result += hex_chars[byte >> 4];
        result += hex_chars[byte & 0x0f];
    }
    return result;
}

// Convert hex string to bytes
inline std::vector<uint8_t> from_hex(const std::string& hex) {
    std::string h = hex;
    if (h.size() >= 2 && h[0] == '0' && (h[1] == 'x' || h[1] == 'X')) {
        h = h.substr(2);
    }
    std::vector<uint8_t> bytes;
    bytes.reserve(h.size() / 2);
    for (size_t i = 0; i + 1 < h.size(); i += 2) {
        auto hex_to_nibble = [](char c) -> uint8_t {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return 10 + c - 'a';
            if (c >= 'A' && c <= 'F') return 10 + c - 'A';
            return 0;
        };
        bytes.push_back((hex_to_nibble(h[i]) << 4) | hex_to_nibble(h[i + 1]));
    }
    return bytes;
}

}  // namespace Keccak
