#include "market_data/symbol_cik_resolver.h"

#include <algorithm>
#include <mutex>

#include "parquet_utils.h"

namespace MarketData {

void SymbolCikResolver::load(const std::string& filings_root) {
    auto rows = ParquetUtils::load_symbol_cik_map(
        filings_root + "/symbol_cik_map/data.parquet");
    std::unordered_map<std::string, std::vector<SymbolCikRow>> by_symbol;
    for (auto& row : rows) by_symbol[row.symbol].push_back(std::move(row));
    for (auto& [symbol, intervals] : by_symbol) {
        (void)symbol;
        std::sort(intervals.begin(), intervals.end(),
                  [](const SymbolCikRow& a, const SymbolCikRow& b) {
                      return a.valid_from_ms < b.valid_from_ms;
                  });
    }
    std::unique_lock lock(mutex_);
    rows_by_symbol_ = std::move(by_symbol);
    loaded_ = true;
}

std::optional<std::string> SymbolCikResolver::cik_for(
    const std::string& symbol, long long as_of_ms) const {
    std::shared_lock lock(mutex_);
    const auto it = rows_by_symbol_.find(symbol);
    if (it == rows_by_symbol_.end()) return std::nullopt;

    const std::string* match = nullptr;
    for (const auto& row : it->second) {
        if (row.valid_from_ms > as_of_ms) continue;
        if (row.valid_to_ms != 0 && row.valid_to_ms <= as_of_ms) continue;
        // More than one issuer claims this ticker at this instant. Refusing is
        // the only safe answer: picking either one attributes some company's
        // filings to another company's price series.
        if (match != nullptr) return std::nullopt;
        match = &row.cik;
    }
    if (match == nullptr) return std::nullopt;
    return *match;
}

bool SymbolCikResolver::loaded() const {
    std::shared_lock lock(mutex_);
    return loaded_;
}

std::size_t SymbolCikResolver::size() const {
    std::shared_lock lock(mutex_);
    return rows_by_symbol_.size();
}

void SymbolCikResolver::clear() {
    std::unique_lock lock(mutex_);
    rows_by_symbol_.clear();
    loaded_ = false;
}

}  // namespace MarketData
