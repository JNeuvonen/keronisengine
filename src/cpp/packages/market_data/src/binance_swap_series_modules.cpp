#include "market_data/binance_swap_series_modules.h"

#include <algorithm>
#include <limits>

#include "market_data/config.h"
#include "market_data/utils.h"
#include "parquet_utils.h"

namespace MarketData {

namespace {

constexpr long long kMaxFundingIntervalMs = 8LL * 3600 * 1000;

std::vector<TimestampedValue>::const_iterator first_after(
    const std::vector<TimestampedValue>& rows, long long at_ms) {
    return std::upper_bound(
        rows.begin(), rows.end(), at_ms,
        [](long long t, const TimestampedValue& row) { return t < row.timestamp_ms; });
}

std::vector<TimestampedValue>::const_iterator first_at_or_after(
    const std::vector<TimestampedValue>& rows, long long at_ms) {
    return std::lower_bound(
        rows.begin(), rows.end(), at_ms,
        [](const TimestampedValue& row, long long t) { return row.timestamp_ms < t; });
}

}  // namespace

std::vector<TimestampedValue> TimestampedSeriesModule::parse(
    const std::string& path) const {
    const long long from_ms = load_from_ms_.load();
    return ParquetUtils::load_timestamped_values(
        path, value_column(), from_ms == kNoRequest ? 0 : from_ms,
        std::numeric_limits<long long>::max());
}

std::string BinanceSwapFundingModule::path_for(const std::string& key) const {
    return get_funding_rate_base_dir(key, "binance", "swap");
}

std::optional<double> BinanceSwapFundingModule::settled(
    const std::string& symbol, long long from_ms, long long to_ms) const {
    const auto entry = snapshot(symbol);
    if (entry == nullptr || entry->payload.empty()) return std::nullopt;
    const auto& rows = entry->payload;
    if (from_ms < rows.front().timestamp_ms ||
        from_ms > rows.back().timestamp_ms + kMaxFundingIntervalMs) {
        return std::nullopt;
    }
    double total = 0.0;
    for (auto it = first_at_or_after(rows, from_ms);
         it != rows.end() && it->timestamp_ms < to_ms; ++it) {
        total += it->value;
    }
    return total;
}

std::string BinanceSwapAccountLongShortRatioModule::path_for(
    const std::string& key) const {
    return Config::getInstance().get_data_dir_root() +
           "/raw/long_short_ratio/binance/swap/" + key;
}

std::optional<double> BinanceSwapFundingModule::selection_average(
    const std::string& symbol, long long at_ms, long long cumulative_start_ms) const {
    constexpr long long day_ms = 86400000;
    const auto entry = snapshot(symbol);
    if (entry == nullptr || entry->payload.empty()) return std::nullopt;
    const auto& rows = entry->payload;
    if (rows.front().timestamp_ms > at_ms - 7 * day_ms) return std::nullopt;
    const auto end = first_after(rows, at_ms);
    if (end == rows.begin() ||
        at_ms - std::prev(end)->timestamp_ms > 12 * 3600000LL) {
        return std::nullopt;
    }
    const auto begin = first_after(rows, at_ms - 7 * day_ms);
    if (cumulative_start_ms > 0) {
        if (cumulative_start_ms > at_ms - 7 * day_ms) return std::nullopt;
        double cumulative = 0.;
        const auto first = first_at_or_after(rows, cumulative_start_ms);
        for (auto it = first; it != begin; ++it) cumulative += it->value;
        const double previous = cumulative;
        for (auto it = begin; it != end; ++it) cumulative += it->value;
        return (cumulative - previous) / 7.;
    }
    double total = 0.;
    for (auto it = begin; it != end; ++it) {
        total += it->value;
    }
    return total / 7.;
}

std::optional<TimestampedValue> BinanceSwapAccountLongShortRatioModule::latest(
    const std::string& symbol, long long at_ms, long long max_age_ms) const {
    const auto entry = snapshot(symbol);
    if (entry == nullptr) return std::nullopt;
    const auto& rows = entry->payload;
    const auto it = first_after(rows, at_ms);
    if (it == rows.begin()) return std::nullopt;
    const auto& row = *std::prev(it);
    if (row.timestamp_ms <= at_ms - max_age_ms) return std::nullopt;
    return row;
}

}  // namespace MarketData
