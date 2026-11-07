#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace Core {

class GlobalFeatureCache {
   public:
    GlobalFeatureCache() = default;
    ~GlobalFeatureCache() = default;
    static GlobalFeatureCache &getInstance();

    [[nodiscard]] std::string get_cache_key(
        const std::string &feat_name, const std::string &dataset_identifier,
        long long kline_end_time_ms) const;

    void insert(const std::string &key, double value);
    std::optional<double> get(const std::string &key);

   private:
    std::unordered_map<std::string, double> cache;
    std::mutex cache_mutex;
};

}  // namespace Core