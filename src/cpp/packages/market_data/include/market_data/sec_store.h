#pragma once
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "market_data/sec_dataset.h"

namespace MarketData {

// Process-wide access to SEC filings, keyed the way the data is keyed.
//
// Datasets live under CIK, not symbol: a company keeps its CIK through renames
// and relisting, and a ticker can be reassigned to an unrelated company. The
// symbol layer is a point-in-time resolution on top, so a lookup for "ABCD" in
// 2019 and in 2026 can legitimately reach different issuers.
//
// Provisioning is explicit and loading never happens inside a decision path.
// `preload` is called once per session with the eligible universe; `dataset_for`
// is a pure in-memory lookup that returns nullopt when a symbol was not
// provisioned or cannot be resolved. Callers must fail closed on nullopt --
// "no filings visible" and "we never loaded this issuer" are different facts
// and must not be collapsed into the same trading decision.
class SecStore {
   public:
    static SecStore& instance();

    // filings_root: <DATA_DIR_ROOT>/alt/filings/sec
    // Both delegate to AltDataManager's SymbolCikResolver, which owns the
    // ticker -> CIK map; the facts module resolves through the same one.
    void load_symbol_map(const std::string& filings_root);

    // Resolve a ticker as of a moment. nullopt when unmapped or ambiguous.
    std::optional<std::string> cik_for(const std::string& symbol,
                                       long long as_of_ms) const;

    // Load every issuer behind `symbols`. Blocking, and meant for session
    // start; returns the number of datasets actually provisioned.
    std::size_t preload(const std::string& filings_root,
                        const std::vector<std::string>& symbols,
                        long long as_of_ms, long long history_start_ms);

    std::optional<std::shared_ptr<SecDataset>> dataset_for(
        const std::string& symbol, long long as_of_ms) const;
    std::optional<std::shared_ptr<SecDataset>> dataset_for_cik(
        const std::string& cik) const;

    bool symbol_map_loaded() const;
    std::size_t symbol_map_size() const;
    std::size_t provisioned_count() const;
    // Symbols asked for that could not be resolved to a CIK. Surfaced rather
    // than swallowed: the unmapped set is not random, it is concentrated in
    // delisted names, so its size is a bias measurement.
    std::vector<std::string> unresolved_symbols() const;

    void clear();

   private:
    SecStore() = default;
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<SecDataset>> by_cik_;
    std::vector<std::string> unresolved_;
};

}  // namespace MarketData
