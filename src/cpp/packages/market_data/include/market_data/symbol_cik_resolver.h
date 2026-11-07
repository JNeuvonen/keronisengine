#pragma once
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "market_data/sec_filing.h"

namespace MarketData {

// Point-in-time ticker -> CIK, the layer that lets ticker-keyed market data
// reach CIK-keyed filings.
//
// It has to be point-in-time because tickers are recycled: after a delisting
// the symbol is reassigned to an unrelated issuer, so resolving without a time
// attaches a dead company's filings to a live company's price series. When two
// issuers claim a ticker at the same instant the resolver REFUSES rather than
// guessing, because either guess is that same bug.
//
// Owned by AltDataManager: both the facts module and the filings store need it,
// and two copies of this map would drift.
class SymbolCikResolver {
   public:
    // filings_root: <DATA_DIR_ROOT>/alt/filings/sec
    void load(const std::string& filings_root);

    std::optional<std::string> cik_for(const std::string& symbol,
                                       long long as_of_ms) const;

    bool loaded() const;
    std::size_t size() const;
    void clear();

   private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::vector<SymbolCikRow>> rows_by_symbol_;
    bool loaded_ = false;
};

}  // namespace MarketData
