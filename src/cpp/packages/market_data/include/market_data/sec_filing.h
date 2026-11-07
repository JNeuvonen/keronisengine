#pragma once
#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

namespace MarketData {

// One SEC filing, as the engine sees it.
//
// `observable_ts_ms` is the field that matters and the only one a strategy may
// use for timing. It is the acceptance time plus a per-form latency pad, and it
// answers "when could we have acted on this", which is never "when was it
// filed". Filings sourced from the daily index carry day precision only, so
// their observable time is bounded from above -- a filing can appear late, but
// never early.
struct SecFiling {
    std::string cik;
    std::string accn;
    std::string form;        // "S-3", "424B5", "8-K", "4", "144"
    std::string item_code;   // 8-K only; empty otherwise
    long long filed_ts_ms = 0;
    long long observable_ts_ms = 0;
};

// One validity interval of a symbol -> CIK assignment. valid_to_ms == 0 means
// "still current". Tickers get reassigned after a delisting, so resolving one
// without a time is how a dead company's filings end up attached to a live
// company's price history.
struct SymbolCikRow {
    std::string symbol;
    std::string cik;
    long long valid_from_ms = 0;
    long long valid_to_ms = 0;
};

// The set of form types a strategy cares about. Deliberately supplied by the
// caller: the module has no notion of which forms are "dilutive" or "distress",
// because that is a strategy's thesis, not a property of EDGAR.
using FormSet = std::unordered_set<std::string>;

// Latency pads mirror LATENCY_CONSERVATIVE in
// data_library/alt_data/sec/pit.py. They must stay in sync with it, since the
// Python research path and the engine have to agree on when a filing became
// actionable or the backtest is measuring a different strategy than the one
// that would trade.
namespace SecLatency {
constexpr long long kDayMs = 86'400'000LL;
constexpr long long kFactsDefaultMs = 1 * kDayMs;
constexpr long long kInsiderForm4Ms = 3 * kDayMs;
constexpr long long kProposedSales144Ms = 1 * kDayMs;
constexpr long long kBeneficial13dMs = 12 * kDayMs;
constexpr long long kBeneficial13gMs = 12 * kDayMs;
constexpr long long kMaterialEvents8kMs = 1 * kDayMs;
constexpr long long kRegistrationMs = 1 * kDayMs;

// Pad for a form type, by the table above. Unknown forms get the most
// conservative pad rather than zero: a form we have not classified must not
// become visible sooner than one we have.
//
// Inline because it is a pure lookup and every consumer -- the parquet loader
// in common/, the dataset in market_data/, and strategies -- would otherwise
// need the defining TU linked in, which targets that list sources explicitly
// rather than globbing do not get.
inline long long pad_for_form(const std::string& form) {
    if (form.empty()) return kBeneficial13dMs;

    // Beneficial ownership FIRST: "SC 13D" also starts with 'S', and letting
    // the registration branch claim it gave 13D/G a 1-day pad instead of 12,
    // i.e. eleven days of lookahead on exactly the filings that move a squeeze.
    if (form.rfind("SC 13D", 0) == 0 || form.rfind("SCHEDULE 13D", 0) == 0)
        return kBeneficial13dMs;
    if (form.rfind("SC 13G", 0) == 0 || form.rfind("SCHEDULE 13G", 0) == 0)
        return kBeneficial13gMs;

    // Registrations and prospectuses: S-1, S-3, S-4, S-8, S-11, F-1, F-3,
    // F-4 and every 424B variant. Matched on the "S-"/"F-" shape only.
    if ((form[0] == 'S' || form[0] == 'F') && form.size() > 1 && form[1] == '-')
        return kRegistrationMs;
    if (form.rfind("424", 0) == 0) return kRegistrationMs;

    if (form.rfind("8-K", 0) == 0) return kMaterialEvents8kMs;
    if (form.rfind("10-", 0) == 0 || form.rfind("20-F", 0) == 0)
        return kFactsDefaultMs;

    // Insider: Forms 3, 4, 5 and their amendments -- a bare digit, optionally
    // followed by "/A". Checked before 144 so "4" does not fall through.
    if (form == "3" || form == "4" || form == "5" || form == "3/A" ||
        form == "4/A" || form == "5/A")
        return kInsiderForm4Ms;

    if (form.rfind("144", 0) == 0) return kProposedSales144Ms;

    // Unknown form: the most conservative pad we use anywhere. A form we have
    // not classified must never become visible sooner than one we have --
    // getting this wrong shows up as a backtest that beats live and nobody
    // knows why.
    return std::max({kFactsDefaultMs, kInsiderForm4Ms, kProposedSales144Ms,
                     kBeneficial13dMs, kBeneficial13gMs, kMaterialEvents8kMs,
                     kRegistrationMs});
}
}  // namespace SecLatency

}  // namespace MarketData
