#pragma once

#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "market_data/alt_data_module.h"
#include "market_data/alt_data_types.h"

namespace MarketData {

struct CustomObservation {
    long long available_ts_ms = 0;
    double value = 0.0;
    std::unordered_map<std::string, double> values;
};

// Generic point-in-time file-backed series for user-supplied alt-data.
// Reads JSONL (.jsonl) or JSON array (.json) files under:
//   <root>/<dataset_name>/<key>.jsonl  (or .json, or directory <key>/ of files)
// Every row must include integer `available_ts_ms` (UTC ms when observable)
// plus numeric fields (e.g. `value` or arbitrary named numeric columns).
class CustomSeriesModule final
    : public FileBackedAltDataModule<std::vector<CustomObservation>> {
   public:
    static constexpr long long kNoMaxAge = std::numeric_limits<long long>::max();

    explicit CustomSeriesModule(std::string dataset_name,
                                std::string root_override = "");

    AltDataSource source() const override { return AltDataSource::Custom; }
    const std::string& dataset_name() const { return dataset_name_; }

    // Newest observation with available_ts_ms <= as_of_ms (and within max_age_ms).
    // Returns nullopt if key was never provisioned, no row is visible, or newest is stale.
    std::optional<CustomObservation> latest(
        const std::string& key, long long as_of_ms,
        long long max_age_ms = kNoMaxAge) const;

    // Convenience accessor for a single numeric field (default "value").
    std::optional<double> latest_value(
        const std::string& key, long long as_of_ms,
        const std::string& field = "value",
        long long max_age_ms = kNoMaxAge) const;

    // Observations with available_ts_ms in [from_ms_inclusive, to_ms_exclusive).
    // Returns nullopt if key was never provisioned; empty vector if provisioned with no rows in window.
    std::optional<std::vector<CustomObservation>> window(
        const std::string& key, long long from_ms_inclusive,
        long long to_ms_exclusive) const;

   protected:
    std::string path_for(const std::string& key) const override;
    std::vector<CustomObservation> parse(const std::string& path) const override;
    long long content_fingerprint(const std::string& path) const override;

   private:
    std::string base_dir() const;

    std::string dataset_name_;
    std::string root_override_;
};

}  // namespace MarketData
