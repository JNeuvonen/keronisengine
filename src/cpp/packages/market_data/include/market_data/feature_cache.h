#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace MarketData {

/**
 * @brief A cache structure for storing feature calculation results and their
 * metadata
 *
 * Stores both the calculated feature value and a vector of historical values
 * needed for rolling window calculations (e.g., for moving averages). The
 * timestamp helps track when the feature was last calculated.
 */
struct FeatureCacheValue {
    double value;  // The current calculated feature value
    std::unordered_map<std::string, double>
        intermediate_store;  // E.g sum of last 30 values
    std::vector<double>
        values;  // Historical values needed for rolling window calculations,
                 // doesnt need to be used for every feature
    long long timestamp_ms;  // When this cache was generated (helps with
                             // invalidation)
};

/**
 * @brief A global store for all datasets' feature caches
 *
 * Maps dataset identifiers to their individual feature caches. This enables
 * efficient feature sharing across different datasets, particularly useful when
 * multiple strategies depend on the same features from the same dataset
 * (e.g., BTCUSDT features used by multiple strategies).
 */
using FeatureCache = std::unordered_map<std::string, FeatureCacheValue>;

}  // namespace MarketData
