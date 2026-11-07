#pragma once
#include <atomic>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "market_data/alt_data_module.h"
#include "market_data/alt_data_types.h"

namespace MarketData {

// A dense per-symbol series kept by alt_data_syncer in a day-partitioned tree,
// ascending by timestamp. Every accessor takes an as-of and reads nothing
// stamped after it: the backtest loads the whole file, future included.
//
// The trees hold years of daily files, and a reload signal re-parses every
// file of a key, so a live engine asks for the history its strategies need.
// History before the earliest request is not read; with none, all of it is.
class TimestampedSeriesModule
    : public FileBackedAltDataModule<std::vector<TimestampedValue>> {
   public:
    // Strategies share one module, so a request can only widen the window.
    void request_history_from_ms(long long from_ms) {
        long long current = load_from_ms_.load();
        while (from_ms < current &&
               !load_from_ms_.compare_exchange_weak(current, from_ms)) {
        }
    }

    // A preload that overlapped a widening request runs again, so no key is
    // left parsed with the narrower window.
    std::size_t preload(const std::vector<std::string>& keys) override {
        while (true) {
            const long long window = load_from_ms_.load();
            const auto resolved =
                FileBackedAltDataModule<std::vector<TimestampedValue>>::preload(keys);
            if (load_from_ms_.load() == window) return resolved;
        }
    }

    void clear() override {
        FileBackedAltDataModule<std::vector<TimestampedValue>>::clear();
        load_from_ms_.store(kNoRequest);
    }

   protected:
    virtual std::string value_column() const = 0;
    std::vector<TimestampedValue> parse(const std::string& path) const override;
    long long content_fingerprint(const std::string& path) const override {
        return newest_mtime_ns_under(path);
    }

   private:
    static constexpr long long kNoRequest = std::numeric_limits<long long>::max();
    std::atomic<long long> load_from_ms_{kNoRequest};
};

// Funding settlements, raw/funding/binance/swap/<SYMBOL>.
class BinanceSwapFundingModule final : public TimestampedSeriesModule {
   public:
    AltDataSource source() const override {
        return AltDataSource::BinanceSwapFunding;
    }

    // Sum of rates settled in [from_ms, to_ms), the window a position open over
    // it pays. nullopt when the key was never provisioned, or when `from_ms`
    // lies outside [first settlement, last settlement + 8h] -- before listing,
    // or after the series stopped -- so a missing history is not read as zero.
    std::optional<double> settled(const std::string& symbol, long long from_ms,
                                  long long to_ms) const;

    std::optional<double> selection_average(const std::string& symbol,
                                           long long at_ms,
                                           long long cumulative_start_ms = 0) const;

   protected:
    std::string path_for(const std::string& key) const override;
    std::string value_column() const override { return "funding_rate"; }
};

// Global account long/short ratio, 5-minute, raw/long_short_ratio/binance/swap/<SYMBOL>.
// Rows carry the API's stamp, which is the data.binance.vision archive's
// create_time + 5 minutes for the same observation.
class BinanceSwapAccountLongShortRatioModule final
    : public TimestampedSeriesModule {
   public:
    AltDataSource source() const override {
        return AltDataSource::BinanceSwapAccountLongShortRatio;
    }

    // Newest row stamped in (at_ms - max_age_ms, at_ms]; nullopt when none or
    // never provisioned.
    std::optional<TimestampedValue> latest(const std::string& symbol,
                                           long long at_ms,
                                           long long max_age_ms) const;

   protected:
    std::string path_for(const std::string& key) const override;
    std::string value_column() const override { return "long_short_ratio"; }
};

}  // namespace MarketData
