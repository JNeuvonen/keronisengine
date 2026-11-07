#include "market_data/sec_dataset.h"

#include <algorithm>
#include <filesystem>
#include <limits>
#include <mutex>
#include <shared_mutex>

#include "parquet_utils.h"

namespace MarketData {
namespace {

// Form families on disk. Each is its own tree under the filings root, so an
// issuer's complete history is the union across them.
constexpr const char* kFamilies[] = {
    "registrations", "material_events", "proposed_sales",
    "beneficial_ownership", "insider_transactions",
};

bool by_observable(const SecFiling& a, const SecFiling& b) {
    return a.observable_ts_ms < b.observable_ts_ms;
}

}  // namespace

void SecDataset::load(const std::string& filings_root, long long start_ts_ms) {
    std::vector<SecFiling> all;
    for (const char* family : kFamilies) {
        const std::string base =
            filings_root + "/" + family + "/" + cik_;
        if (!std::filesystem::exists(base)) continue;
        auto part = ParquetUtils::load_sec_filings(
            base, start_ts_ms, std::numeric_limits<long long>::max());
        all.insert(all.end(), std::make_move_iterator(part.begin()),
                   std::make_move_iterator(part.end()));
    }
    // Sorted by OBSERVABLE time, not filed time, because every accessor
    // bisects on observable time. Sorting by the wrong key would silently
    // return the wrong slice for forms whose pads differ.
    std::sort(all.begin(), all.end(), by_observable);

    std::unique_lock lock(mutex_);
    filings_ = std::move(all);
    loaded_ = true;
}

void SecDataset::add(const SecFiling& filing) {
    std::unique_lock lock(mutex_);
    const auto at =
        std::upper_bound(filings_.begin(), filings_.end(), filing, by_observable);
    filings_.insert(at, filing);
}

std::vector<SecFiling> SecDataset::visible_filings(long long as_of_ms) const {
    std::shared_lock lock(mutex_);
    SecFiling probe;
    probe.observable_ts_ms = as_of_ms;
    const auto end =
        std::upper_bound(filings_.begin(), filings_.end(), probe, by_observable);
    return std::vector<SecFiling>(filings_.begin(), end);
}

std::optional<long long> SecDataset::last_filed_ms(const FormSet& forms,
                                                   long long as_of_ms) const {
    std::shared_lock lock(mutex_);
    SecFiling probe;
    probe.observable_ts_ms = as_of_ms;
    auto it =
        std::upper_bound(filings_.begin(), filings_.end(), probe, by_observable);
    while (it != filings_.begin()) {
        --it;
        if (forms.empty() || forms.count(it->form))
            return it->observable_ts_ms;
    }
    return std::nullopt;
}

int SecDataset::count_in_window(const FormSet& forms, long long as_of_ms,
                                long long window_ms) const {
    std::shared_lock lock(mutex_);
    SecFiling hi_probe;
    hi_probe.observable_ts_ms = as_of_ms;
    SecFiling lo_probe;
    lo_probe.observable_ts_ms = as_of_ms - window_ms;
    const auto hi = std::upper_bound(filings_.begin(), filings_.end(), hi_probe,
                                     by_observable);
    const auto lo = std::lower_bound(filings_.begin(), filings_.end(), lo_probe,
                                     by_observable);
    int n = 0;
    for (auto it = lo; it != hi; ++it)
        if (forms.empty() || forms.count(it->form)) ++n;
    return n;
}

std::optional<long long> SecDataset::days_since(const FormSet& forms,
                                                long long as_of_ms) const {
    const auto last = last_filed_ms(forms, as_of_ms);
    if (!last) return std::nullopt;
    return (as_of_ms - *last) / SecLatency::kDayMs;
}

std::size_t SecDataset::size() const {
    std::shared_lock lock(mutex_);
    return filings_.size();
}

}  // namespace MarketData
