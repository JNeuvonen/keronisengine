#pragma once

#include <memory>
#include <string>
#include <vector>

namespace arrow {
class Table;
}

struct PolymarketTradeTick {
    std::string side;          // "BUY" or "SELL"
    std::string condition_id;  // market conditionId
    double size;               // quantity
    double price;              // trade price (0-1)
    long long timestamp;       // ms
    std::string slug;          // event slug
    std::string outcome;       // "Up", "Down"
    int outcome_index;         // 0, 1
    std::string clob_token_id; // resolved from event meta

    static std::vector<PolymarketTradeTick> from_arrow_table(
        const std::shared_ptr<arrow::Table>& table);
};
