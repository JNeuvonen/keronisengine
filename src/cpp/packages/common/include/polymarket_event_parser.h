#pragma once

#include <algorithm>
#include <cctype>
#include <string>

#include "sqlite_datasets.h"

namespace PolymarketEventParser {

// Parse strike price from Polymarket groupItemTitle field.
// Handles formats: "64,000", "↑ 82,000", "↓ 66,000", "1,975"
inline double parse_strike_from_group_item_title(const std::string& title) {
    if (title.empty()) return 0.0;

    // Find first digit position (skips arrow prefix and whitespace)
    size_t start = 0;
    while (start < title.size() &&
           !std::isdigit(static_cast<unsigned char>(title[start]))) {
        ++start;
    }
    if (start >= title.size()) return 0.0;

    // Copy from first digit, stripping commas
    std::string cleaned;
    size_t end = start;
    for (size_t i = start; i < title.size(); ++i) {
        char c = title[i];
        if (c == ',') continue;
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
            cleaned += c;
            end = i;
        } else {
            break;
        }
    }

    if (cleaned.empty()) return 0.0;
    try {
        double value = std::stod(cleaned);
        // Handle k/K suffix (e.g., "100k" → 100000)
        size_t next = end + 1;
        if (next < title.size() &&
            (title[next] == 'k' || title[next] == 'K')) {
            value *= 1000.0;
        }
        return value;
    } catch (...) {
        return 0.0;
    }
}

// Determine product type from groupItemTitle and question text.
// Arrow prefixes (↑/↓) indicate one-touch barriers.
// Question keywords ("above"/"below"/"reach"/"dip") indicate product type.
inline PolymarketProductType parse_product_type(
    const std::string& group_item_title, const std::string& question) {
    // Check arrow prefix in groupItemTitle (UTF-8: ↑ = \xe2\x86\x91, ↓ =
    // \xe2\x86\x93)
    if (group_item_title.size() >= 3) {
        unsigned char b0 = group_item_title[0];
        unsigned char b1 = group_item_title[1];
        unsigned char b2 = group_item_title[2];
        if (b0 == 0xe2 && b1 == 0x86) {
            if (b2 == 0x91) return PolymarketProductType::ONE_TOUCH_UP;
            if (b2 == 0x93) return PolymarketProductType::ONE_TOUCH_DOWN;
        }
    }

    // Case-insensitive search in question text
    std::string q = question;
    std::transform(q.begin(), q.end(), q.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    if (q.find("above") != std::string::npos)
        return PolymarketProductType::DIGITAL_CLOSE_ABOVE;
    if (q.find("below") != std::string::npos)
        return PolymarketProductType::DIGITAL_CLOSE_BELOW;
    if (q.find("reach") != std::string::npos)
        return PolymarketProductType::ONE_TOUCH_UP;
    if (q.find("dip") != std::string::npos)
        return PolymarketProductType::ONE_TOUCH_DOWN;

    return PolymarketProductType::UNKNOWN;
}

}  // namespace PolymarketEventParser
