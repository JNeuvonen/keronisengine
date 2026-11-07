#include "market_making/dtypes/global_feature_cache.h"

namespace Core {

GlobalFeatureCache &GlobalFeatureCache::getInstance() {
    static GlobalFeatureCache instance;
    return instance;
}

void GlobalFeatureCache::insert(const std::string &key, double value) {
    std::lock_guard<std::mutex> lock(this->cache_mutex);
    this->cache[key] = value;
}

std::optional<double> GlobalFeatureCache::get(const std::string &key) {
    std::lock_guard<std::mutex> lock(this->cache_mutex);
    auto it = this->cache.find(key);
    if (it == this->cache.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::string GlobalFeatureCache::get_cache_key(
    const std::string &feat_name, const std::string &dataset_identifier,
    long long kline_end_time_ms) const {
    return feat_name + ":" + dataset_identifier + ":" +
           std::to_string(kline_end_time_ms);
}

}  // namespace Core