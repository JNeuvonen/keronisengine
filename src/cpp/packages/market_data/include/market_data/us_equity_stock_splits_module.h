#pragma once
#include <optional>
#include <string>
#include <vector>

#include "market_data/alt_data_module.h"
#include "market_data/alt_data_types.h"

namespace MarketData {

// US-equity corporate-action splits, one file per ticker under
// alt/corporate_actions/splits/, as written by Polygon's reference feed. Keyed
// by ticker because that is how the producer writes them; SEC facts are keyed
// by CIK for the opposite reason -- a ticker is reassigned after a delisting,
// a CIK never is.
//
// THE INVARIANT: every accessor takes an as-of and cannot see a split executed
// after it. The file holds every split a ticker ever had, including ones in a
// backtest's future, and applying those makes a historical market cap look more
// correct than it could possibly have been.
class UsEquityStockSplitsModule final
    : public FileBackedAltDataModule<std::vector<StockSplitEvent>> {
   public:
    AltDataSource source() const override {
        return AltDataSource::UsEquityStockSplits;
    }

    // Splits visible at `as_of_ms`, oldest first. Empty for a provisioned
    // ticker that never split; nullopt when never provisioned.
    std::optional<std::vector<StockSplitEvent>> splits_as_of(
        const std::string& symbol, long long as_of_ms) const;

    // Cumulative ratio applied between `from_ms` and `to_ms`. Multiply a share
    // count filed at `from_ms` by this to express it on the price basis at
    // `to_ms`. nullopt when the ticker was never provisioned.
    std::optional<double> cumulative_ratio(const std::string& symbol,
                                           long long from_ms,
                                           long long to_ms) const;

   protected:
    std::string path_for(const std::string& key) const override;
    std::vector<StockSplitEvent> parse(const std::string& path) const override;
    // Most tickers never split; an absent file is that fact, not a gap.
    bool absent_file_is_empty_payload() const override { return true; }
};

}  // namespace MarketData
