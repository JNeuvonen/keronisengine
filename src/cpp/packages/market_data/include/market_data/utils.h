#pragma once
#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace MarketData {
struct StreamInfo;
struct OhlcvBarMeta;
struct BarClosePolicy;

std::vector<StreamInfo> get_stream_infos(
    const std::string& exchange_name, const std::string& market_type,
    const std::string& stream_type, const std::vector<StreamInfo>& streams);

std::string get_stream_url(const std::string& exchange_name,
                           const std::string& market_type,
                           const std::string& stream_type,
                           std::optional<std::string> interval,
                           const std::vector<StreamInfo>& stream_infos);

std::string get_raw_trade_ticks_base_dir(const std::string& symbol,
                                         const std::string& exchange_name,
                                         const std::string& market_type);
std::string get_derived_bars_base_dir(const std::string& symbol,
                                      const std::string& exchange_name,
                                      const std::string& market_type,
                                      const BarClosePolicy& bar_close_policy,
                                      bool omit_data_dir_root = false);

std::string get_funding_rate_base_dir(const std::string& symbol,
                                      const std::string& exchange_name,
                                      const std::string& market_type,
                                      bool omit_data_dir_root = false);

std::vector<std::string> get_unique_symbols_from_bar_dir(
    const std::string& market_data_library_root,
    const std::string& exchange_name, const std::string& market_type);

std::string convert_interval_ms_to_str(int interval_ms);
std::string convert_interval_ms_to_bitget_str_format(int interval_ms);
long long convert_interval_str_to_ms(const std::string& interval);
int convert_ms_to_num_of_minutes(long long interval_ms);

long long get_latest_fully_completed_kline_open_ms(long long interval_ms);

long long convert_interval_str_to_ms(const std::string& interval);

bool symbol_ends_in_usdc(const std::string& symbol);
bool is_time_ms_within_real_trading_hours(long long time_ms);
bool is_time_ms_within_extended_trading_hours(long long time_ms);
bool is_market_open_bar(long long time_ms);
std::optional<long long> get_market_open_time_ms(long long current_time_ms);
std::optional<long long> get_market_close_time_ms(long long current_time_ms);
std::optional<long long> get_time_ms_until_market_close(
    long long current_time_ms);

// Clamp the on-disk read window for a freshly-constructed OhlcvDataset
// based on bar-type density. For dense bar types (1s and finer), reading
// from `default_start` all the way to wall clock would force
// ParquetUtils::scan_partitions to materialize hundreds of GB of Arrow
// tables before OhlcvDataset's post-load rm_unused_klines could trim
// anything — peak memory blows the process. This function returns a
// later start time for dense bars (e.g. now - 30d for binance:swap 1s)
// while passing `default_start` through unchanged for sparser bar types.
//
// The cap must always be wider than the longest rolling-window feature
// any strategy reads off the dataset. The 30-day cap covers windows up
// to ~3 weeks with margin. Bump if needed.
std::chrono::system_clock::time_point get_dataset_read_start_time(
    const OhlcvBarMeta& meta,
    std::chrono::system_clock::time_point default_start);

// Per-bar-type floor on the post-init-sync in-memory kline count. Applied as
// `max(caller_cap, get_min_klines_to_keep_after_init(meta))` so a caller's cap
// is honored when wider, but a too-narrow cap (e.g. the historic 200K default)
// does NOT silently trim the dataset below what the longest rolling-window
// feature on this bar type needs.
//
// Without this floor, OhlcvDataset's post-init trim caps binance/swap/1s at
// 200K bars = ~2.3 days, narrower than a 7-day window (604,800 bars): 3d
// and 7d rolling accumulators would hold the same data and produce
// bit-identical values. Returns 0 (= "no floor") for bar types not
// known to drive long-window features.
long long get_min_klines_to_keep_after_init(const OhlcvBarMeta& meta);

// Venue-wide ceiling on resident klines per dataset; 0 means no ceiling.
long long get_max_klines_to_keep(const OhlcvBarMeta& meta);

}  // namespace MarketData
