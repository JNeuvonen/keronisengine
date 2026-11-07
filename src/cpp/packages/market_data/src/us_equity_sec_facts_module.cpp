#include "market_data/us_equity_sec_facts_module.h"

#include <algorithm>
#include <map>
#include <tuple>

#include "market_data/sec_filing.h"
#include "parquet_utils.h"

namespace MarketData {

void UsEquitySecFactsModule::set_tags_of_interest(
    std::unordered_set<std::string> tags) {
    tags_of_interest_ = std::move(tags);
}

std::string UsEquitySecFactsModule::path_for(const std::string& key) const {
    return alt_data_root() + "/filings/sec/facts/" + key;
}

std::vector<XbrlFact> UsEquitySecFactsModule::parse(
    const std::string& path) const {
    return ParquetUtils::load_us_equity_sec_facts(path, tags_of_interest_);
}

long long UsEquitySecFactsModule::content_fingerprint(
    const std::string& path) const {
    return newest_mtime_ns_under(path);
}

std::optional<std::vector<XbrlFact>> UsEquitySecFactsModule::facts_as_of(
    const std::string& cik, long long as_of_ms) const {
    const auto entry = snapshot(cik);
    if (entry == nullptr) return std::nullopt;

    const long long cutoff = as_of_ms - SecLatency::kFactsDefaultMs;
    // Latest visible filing per period, matching pit.py's group-by-then-last.
    using PeriodKey = std::tuple<std::string, std::string, std::string,
                                 long long, long long>;
    std::map<PeriodKey, XbrlFact> latest;
    for (const auto& fact : entry->payload) {
        if (fact.filed_ts_ms > cutoff) break;  // sorted by filed_ts_ms
        const PeriodKey key{fact.taxonomy, fact.tag, fact.unit,
                            fact.period_start_ms, fact.period_end_ms};
        auto it = latest.find(key);
        if (it == latest.end() || fact.filed_ts_ms >= it->second.filed_ts_ms) {
            latest[key] = fact;
        }
    }
    std::vector<XbrlFact> out;
    out.reserve(latest.size());
    for (auto& [key, fact] : latest) {
        (void)key;
        out.push_back(std::move(fact));
    }
    std::sort(out.begin(), out.end(),
              [](const XbrlFact& a, const XbrlFact& b) {
                  return a.period_end_ms < b.period_end_ms;
              });
    return out;
}

std::optional<UsEquitySecFactsModule::SharesObservation>
UsEquitySecFactsModule::shares_outstanding_as_of(const std::string& cik,
                                                 long long as_of_ms) const {
    const auto visible = facts_as_of(cik, as_of_ms);
    if (!visible.has_value()) return std::nullopt;

    // The most recently FILED cover page, not the latest period end: the cover
    // reports shares as of the latest practicable date, so a newer filing
    // covering an older period still carries the fresher count.
    const XbrlFact* best = nullptr;
    for (const auto& fact : visible.value()) {
        if (fact.tag != kSharesOutstandingTag) continue;
        if (fact.value <= 0.0) continue;
        if (best == nullptr || fact.filed_ts_ms > best->filed_ts_ms) {
            best = &fact;
        }
    }
    if (best == nullptr) return std::nullopt;
    SharesObservation obs;
    obs.shares = best->value;
    obs.filed_ts_ms = best->filed_ts_ms;
    obs.period_end_ms = best->period_end_ms;
    return obs;
}

}  // namespace MarketData
