#pragma once
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "market_data/alt_data_module.h"
#include "market_data/alt_data_types.h"

namespace MarketData {

// US-equity XBRL facts from EDGAR, one partitioned tree per issuer under
// alt/filings/sec/facts/<CIK>/.
//
// Keyed by CIK, not ticker. A company keeps its CIK through renames and
// relisting while a ticker gets reassigned to an unrelated issuer, so resolving
// by ticker is how a dead company's filings end up attached to a live company's
// price history. Symbol -> CIK is a separate point-in-time concern layered
// above, in AltDataManager.
//
// THE INVARIANT, matching SECData.facts_as_of in
// Point-in-time visibility: a fact is visible only once
// filed_ts_ms + kFactsDefaultMs <= as_of, and for each
// (taxonomy, tag, unit, period_start, period_end) only the latest such filing
// counts. Amendments are the reason -- the same period gets refiled with a
// different value, and a reverse split restates prior-period share counts onto
// the new basis. Taking the newest observation regardless of filing time reads
// a correction that had not happened yet.
class UsEquitySecFactsModule final
    : public FileBackedAltDataModule<std::vector<XbrlFact>> {
   public:
    AltDataSource source() const override {
        return AltDataSource::UsEquitySecFacts;
    }

    // Only these tags are parsed off disk. An issuer's full history runs to
    // tens of thousands of rows and the engine wants a handful, so the set is
    // narrowed at load rather than at query.
    void set_tags_of_interest(std::unordered_set<std::string> tags);

    // Facts visible at `as_of_ms`, latest filing per period. nullopt when the
    // CIK was never provisioned -- which a caller must not treat as "no facts".
    std::optional<std::vector<XbrlFact>> facts_as_of(const std::string& cik,
                                                     long long as_of_ms) const;

    // Shares outstanding from the most recent cover page visible at `as_of_ms`,
    // with the filing time that produced it, so a caller can age it.
    struct SharesObservation {
        double shares = 0.0;
        long long filed_ts_ms = 0;
        long long period_end_ms = 0;
    };
    std::optional<SharesObservation> shares_outstanding_as_of(
        const std::string& cik, long long as_of_ms) const;

   protected:
    std::string path_for(const std::string& key) const override;
    std::vector<XbrlFact> parse(const std::string& path) const override;
    // A CIK with no facts tree is absent coverage, not "this issuer filed
    // nothing" -- the opposite of a ticker that never split.
    bool absent_file_is_empty_payload() const override { return false; }
    // The tree is year-partitioned, so the newest mtime beneath it is what
    // moves when the producer appends a filing.
    long long content_fingerprint(const std::string& path) const override;

   private:
    std::unordered_set<std::string> tags_of_interest_{kSharesOutstandingTag};
};

}  // namespace MarketData
