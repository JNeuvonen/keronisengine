#pragma once
// GlobalGeneralCache.hpp
//
// Header-only, TU-independent typed cache.
// Requires C++17 for the inline variable template.
//
// © 2025  Core Library

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace Core {

template <typename T>
class TypedCache {
   private:
    std::unordered_map<std::string, T> cache_;
    mutable std::mutex mutex_;

   public:
    void insert(const std::string& key, const T& value) {
        std::lock_guard<std::mutex> lk(mutex_);
        cache_[key] = value;
    }

    std::optional<T> get(const std::string& key) const {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = cache_.find(key);
        if (it != cache_.end()) return it->second;
        return std::nullopt;
    }

    void remove(const std::string& key) {
        std::lock_guard<std::mutex> lk(mutex_);
        cache_.erase(key);
    }

    void clear() {
        std::lock_guard<std::mutex> lk(mutex_);
        cache_.clear();
    }
};

/* ------------------------------------------------------------------------
 *     One global instance of TypedCache<T> per T, shared program-wide
 * --------------------------------------------------------------------- */
template <typename T>
inline TypedCache<T> g_typedCache;  // C++17 inline variable template

/* ------------------------------------------------------------------------
 *               Thin façade with convenient static helpers
 * --------------------------------------------------------------------- */
class GlobalGeneralCache {
   public:
    template <typename T>
    static void insert(const std::string& key, const T& value) {
        g_typedCache<T>.insert(key, value);
    }

    template <typename T>
    static std::optional<T> get(const std::string& key) {
        return g_typedCache<T>.get(key);
    }

    template <typename T>
    static void remove(const std::string& key) {
        g_typedCache<T>.remove(key);
    }

    template <typename T>
    static void clear() {
        g_typedCache<T>.clear();
    }

   private:
    // Disallow instantiation; this is a purely static utility.
    GlobalGeneralCache() = delete;
    ~GlobalGeneralCache() = delete;
};

long long get_program_start_time_ms();
void set_program_start_time_ms(long long program_start_time_ms);

}  // namespace Core