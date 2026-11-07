#pragma once
#include <string>

#include "market_data/bar_close_policy.h"

namespace MarketData {

struct OhlcvBarMeta {
    std::string symbol;
    std::string exchange_name;
    std::string market_type;
    BarClosePolicy bar_close_policy;
    long long last_storage_tick_close_time_ms;
    std::string last_storage_tick_id;
    [[nodiscard]] std::string get_policy_key() const;
    [[nodiscard]] std::string get_bar_type_str() const;
    [[nodiscard]] std::string get_symbol_base_directory_path() const;
    [[nodiscard]] std::string get_parquets_path() const;
    [[nodiscard]] bool shares_same_symbol_exchange_market(
        const OhlcvBarMeta& other) const;
    void read_last_tick_close_time_ms();

    std::string to_string() const;
};

std::string get_bar_base_directory_path(const std::string& exchange_name,
                                        const std::string& market_type,
                                        const BarClosePolicy& bar_close_policy);

// True when the dataset's only month partition on disk is the current one.
//
// This is the signature of a stream-only stub: the backfill produced nothing,
// so live bars were written into an empty dataset. A last-timestamp test calls
// such a symbol healthy, which is why it has to be checked structurally.
bool has_only_current_month_partition(const OhlcvBarMeta& meta);

// Whether fetch_from_proxy may open init_fetch_done (the gate that lets streamed
// bars into the dataset). head_ms on disk must be trustworthy (not a stream-only
// stub), and for venues with a bounded backfill the fill must have CAUGHT UP:
// a dataset still years behind cannot accept streamed bars, because the stream
// writes the current month, the next run resumes from that newest row, and
// everything between is orphaned with nothing left to ask for it.
bool backfill_allows_stream_ingest(const OhlcvBarMeta& meta,
                                   long long bars_fetched, long long head_ms,
                                   long long now_ms);

}  // namespace MarketData