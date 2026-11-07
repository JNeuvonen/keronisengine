#include "market_data/alt_data_manager.h"
#include "market_data/sec_store.h"

#include <algorithm>
#include <mutex>
#include <shared_mutex>
#include <unordered_set>

#include "parquet_utils.h"

namespace MarketData {

SecStore& SecStore::instance() {
    static SecStore store;
    return store;
}

// Symbol -> CIK moved to AltDataManager, which the facts module resolves
// through too. Kept as a delegating call so existing callers and the filings path do
// not need to change; two copies of this map would drift apart.
void SecStore::load_symbol_map(const std::string& filings_root) {
    AltDataManager::instance().symbol_cik().load(filings_root);
}

std::optional<std::string> SecStore::cik_for(const std::string& symbol,
                                             long long as_of_ms) const {
    return AltDataManager::instance().symbol_cik().cik_for(symbol, as_of_ms);
}

std::size_t SecStore::preload(const std::string& filings_root,
                              const std::vector<std::string>& symbols,
                              long long as_of_ms, long long history_start_ms) {
    std::unordered_set<std::string> wanted;
    std::vector<std::string> unresolved;
    for (const auto& s : symbols) {
        const auto cik = cik_for(s, as_of_ms);
        if (!cik) {
            unresolved.push_back(s);
            continue;
        }
        wanted.insert(*cik);
    }

    std::size_t made = 0;
    for (const auto& cik : wanted) {
        {
            std::shared_lock lock(mutex_);
            if (by_cik_.count(cik)) continue;
        }
        auto ds = std::make_shared<SecDataset>(cik);
        ds->load(filings_root, history_start_ms);
        std::unique_lock lock(mutex_);
        by_cik_.emplace(cik, std::move(ds));
        ++made;
    }
    std::unique_lock lock(mutex_);
    unresolved_ = std::move(unresolved);
    return made;
}

std::optional<std::shared_ptr<SecDataset>> SecStore::dataset_for(
    const std::string& symbol, long long as_of_ms) const {
    const auto cik = cik_for(symbol, as_of_ms);
    if (!cik) return std::nullopt;
    return dataset_for_cik(*cik);
}

std::optional<std::shared_ptr<SecDataset>> SecStore::dataset_for_cik(
    const std::string& cik) const {
    std::shared_lock lock(mutex_);
    const auto it = by_cik_.find(cik);
    if (it == by_cik_.end()) return std::nullopt;
    return it->second;
}

bool SecStore::symbol_map_loaded() const {
    return AltDataManager::instance().symbol_cik().loaded();
}

std::size_t SecStore::symbol_map_size() const {
    return AltDataManager::instance().symbol_cik().size();
}

std::size_t SecStore::provisioned_count() const {
    std::shared_lock lock(mutex_);
    return by_cik_.size();
}

std::vector<std::string> SecStore::unresolved_symbols() const {
    std::shared_lock lock(mutex_);
    return unresolved_;
}

void SecStore::clear() {
    std::unique_lock lock(mutex_);
    by_cik_.clear();
    unresolved_.clear();
}

}  // namespace MarketData
