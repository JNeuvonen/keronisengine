#pragma once
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

#include "market_data/sec_filing.h"

namespace MarketData {

// Every SEC filing for one issuer, keyed by CIK.
//
// Keyed by CIK rather than ticker on purpose. CIK is the eternal identifier --
// a company keeps it through renames, ticker changes and relisting -- so the
// store stays correct when a symbol moves or is reused by a different company.
// Symbol -> CIK resolution is a separate, point-in-time concern layered above.
//
// THE INVARIANT: every accessor takes `as_of_ms` and returns only filings whose
// observable timestamp is at or before it. A strategy cannot read a filing it
// could not have seen, because the API will not hand one over. This is
// deliberately not left to caller discipline: the research path's worst bug
// came from a Python file deciding what was knowable and C++ trusting it.
class SecDataset {
   public:
    explicit SecDataset(std::string cik) : cik_(std::move(cik)) {}

    // Filings visible at `as_of_ms`, oldest first.
    std::vector<SecFiling> visible_filings(long long as_of_ms) const;

    // Acceptance-padded time of the most recent visible filing in `forms`.
    // nullopt when the issuer has filed none of them yet.
    std::optional<long long> last_filed_ms(const FormSet& forms,
                                           long long as_of_ms) const;

    // Visible filings in `forms` within [as_of_ms - window_ms, as_of_ms].
    int count_in_window(const FormSet& forms, long long as_of_ms,
                        long long window_ms) const;

    // Convenience for the common "how stale is this signal" question. Returns
    // nullopt when nothing in `forms` has been filed, which a caller must treat
    // as "no information", never as "a long time ago".
    std::optional<long long> days_since(const FormSet& forms,
                                        long long as_of_ms) const;

    void load(const std::string& filings_root, long long start_ts_ms);
    void add(const SecFiling& filing);   // live path: watcher-fed appends

    const std::string& cik() const { return cik_; }
    std::size_t size() const;
    bool loaded() const { return loaded_; }
    long long last_poll_ms() const { return last_poll_ms_; }
    void mark_polled(long long ts_ms) { last_poll_ms_ = ts_ms; }

   private:
    std::string cik_;
    // Sorted by observable_ts_ms so every accessor is a binary search rather
    // than a scan of the issuer's whole filing history.
    std::vector<SecFiling> filings_;
    mutable std::shared_mutex mutex_;
    bool loaded_ = false;
    // Staleness is measured by the last successful POLL, not the last filing.
    // An issuer can legitimately file nothing for a year, so "no recent event"
    // says nothing about whether the feed is healthy.
    long long last_poll_ms_ = 0;
};

}  // namespace MarketData
