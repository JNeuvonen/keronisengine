#pragma once
#include <optional>
#include <string>
#include <vector>

namespace MarketData {

// The alt-data sources the engine reads. The enum keys module registration,
// provisioning and coverage -- deliberately NOT the return type. A split, a
// filing and a numeric fact are irreducibly different shapes, and a type-erased
// accessor would only move the switch to every call site.
//
// Every value is prefixed by market, because none of these concepts is
// US-specific: equities split on every exchange, and the venue-specific thing
// is the FEED -- Polygon corporate actions, EDGAR, FINRA. An unqualified
// `StockSplits` would have to be renamed the day a second market arrives.
enum class AltDataSource {
    UsEquityStockSplits,
    UsEquitySecFacts,
    UsEquityShortInterest,
    BinanceSwapFunding,
    BinanceSwapAccountLongShortRatio,
    BinanceSwapTradingStatus,
    Custom,
};

const char* to_string(AltDataSource source);
std::optional<AltDataSource> alt_data_source_from_string(const std::string& name);

// <DATA_DIR_ROOT>/alt, resolved once. Throws when DATA_DIR_ROOT is unset,
// matching OhlcvDataset: a root that resolves to nothing provisions nothing,
// and a strategy failing closed on every symbol looks identical to one whose
// universe is simply quiet today.
const std::string& alt_data_root();

// One split, as the engine sees it.
//
// Unprefixed on purpose, unlike the source that produces it: a split is the
// same shape in every market -- an execution date and a ratio -- so a second
// venue's feed would reuse this rather than restate it. The market qualifies
// the source, not the arithmetic.
//
// `ratio` is the multiplier taking a PRE-split share count onto the post-split
// basis: a 1:50 reverse split has ratio 0.02. Prices in the derived tree are
// adjusted retroactively while filed share counts are not, so multiplying an
// unadjusted count by an adjusted price without this correction overstates
// market cap by the cumulative ratio -- GDHG printed $16.8B on $1.3M of ADV.
struct StockSplitEvent {
    long long execution_ts_ms = 0;  // midnight UTC of the execution date
    long long split_from = 0;
    long long split_to = 0;
    double ratio = 1.0;
};

// One XBRL fact observation, as filed.
//
// Facts get AMENDED: the same (tag, period_end) is reported again by a later
// filing with a different value, and a reverse split restates prior-period
// share counts onto the new basis. So an observation is only meaningful
// together with when it was filed, and "the value for Q1" is a question that
// can only be answered as of a date.
//
// Unprefixed for the same reason as StockSplitEvent: the shape is a tagged
// number with a period and a filing time, which is not a US-specific idea.
struct XbrlFact {
    std::string taxonomy;    // "dei", "us-gaap"
    std::string tag;         // "EntityCommonStockSharesOutstanding"
    std::string unit;        // "shares", "USD"
    long long period_start_ms = 0;
    long long period_end_ms = 0;
    long long filed_ts_ms = 0;
    double value = 0.0;
};

// One observation of a dense series (funding settlement, 5-minute ratio).
struct TimestampedValue {
    long long timestamp_ms = 0;
    double value = 0.0;
};

// The tag carrying shares outstanding on the filing's cover page. dei rather
// than us-gaap, and "as of the latest practicable date" rather than the period
// end -- which is why it survives a reverse split sooner than the balance-sheet
// counts do.
inline constexpr const char* kSharesOutstandingTag =
    "EntityCommonStockSharesOutstanding";

}  // namespace MarketData
