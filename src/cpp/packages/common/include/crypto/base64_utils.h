#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Base64Utils {

// Standard base64 encode
inline std::string encode(const uint8_t* data, size_t len) {
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((len + 2) / 3) * 4);

    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) n |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) n |= static_cast<uint32_t>(data[i + 2]);

        result += table[(n >> 18) & 0x3f];
        result += table[(n >> 12) & 0x3f];
        result += (i + 1 < len) ? table[(n >> 6) & 0x3f] : '=';
        result += (i + 2 < len) ? table[n & 0x3f] : '=';
    }
    return result;
}

inline std::string encode(const std::vector<uint8_t>& data) {
    return encode(data.data(), data.size());
}

inline std::string encode(const std::string& data) {
    return encode(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

// URL-safe base64 encode (replaces +/ with -_, strips = padding)
inline std::string encode_urlsafe(const uint8_t* data, size_t len) {
    std::string result = encode(data, len);
    for (char& c : result) {
        if (c == '+')
            c = '-';
        else if (c == '/')
            c = '_';
    }
    // Strip trailing '='
    while (!result.empty() && result.back() == '=') {
        result.pop_back();
    }
    return result;
}

inline std::string encode_urlsafe(const std::vector<uint8_t>& data) {
    return encode_urlsafe(data.data(), data.size());
}

// URL-safe base64 encode WITH padding (matches Python's base64.urlsafe_b64encode)
inline std::string encode_urlsafe_padded(const uint8_t* data, size_t len) {
    std::string result = encode(data, len);
    for (char& c : result) {
        if (c == '+')
            c = '-';
        else if (c == '/')
            c = '_';
    }
    return result;
}

// Standard base64 decode
inline std::vector<uint8_t> decode(const std::string& input) {
    auto char_to_val = [](char c) -> uint8_t {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+' || c == '-') return 62;
        if (c == '/' || c == '_') return 63;
        return 0;
    };

    std::vector<uint8_t> result;
    result.reserve(input.size() * 3 / 4);

    size_t i = 0;
    size_t len = input.size();
    while (i < len) {
        uint32_t n = 0;
        int valid_chars = 0;
        for (int j = 0; j < 4 && i < len; j++, i++) {
            if (input[i] == '=' || input[i] == '\0') {
                i++;
                continue;
            }
            n = (n << 6) | char_to_val(input[i]);
            valid_chars++;
        }
        // Shift remaining
        n <<= (6 * (4 - valid_chars));

        if (valid_chars >= 2) result.push_back((n >> 16) & 0xff);
        if (valid_chars >= 3) result.push_back((n >> 8) & 0xff);
        if (valid_chars >= 4) result.push_back(n & 0xff);
    }
    return result;
}

// URL-safe base64 decode (handles -_ and missing padding)
inline std::vector<uint8_t> decode_urlsafe(const std::string& input) {
    // Convert URL-safe to standard and add padding
    std::string standard = input;
    for (char& c : standard) {
        if (c == '-')
            c = '+';
        else if (c == '_')
            c = '/';
    }
    // Add padding
    while (standard.size() % 4 != 0) {
        standard += '=';
    }
    return decode(standard);
}

}  // namespace Base64Utils
