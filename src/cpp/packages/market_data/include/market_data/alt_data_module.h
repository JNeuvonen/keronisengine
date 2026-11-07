#pragma once
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "market_data/alt_data_types.h"

namespace MarketData {

// What every alt-data source has in common: it is provisioned explicitly before
// trading, it can be re-read when the producer rewrites it, and the set of keys
// it could not supply is a number somebody needs to see.
//
// What they do NOT have in common is their data, so nothing here returns any.
// Typed access lives on the concrete module, reached through AltDataManager.
class AltDataModule {
   public:
    virtual ~AltDataModule() = default;

    virtual AltDataSource source() const = 0;

    // Blocking, meant for session start. Returns how many keys were resolved.
    virtual std::size_t preload(const std::vector<std::string>& keys) = 0;

    // Re-read any provisioned key whose file changed. Live only: in a backtest
    // the wall clock is not the simulation clock, so watching files would make
    // a run depend on when it was started.
    virtual std::size_t refresh_changed() = 0;
    // Reloads the loaded `keys` whether or not their fingerprint moved: the
    // caller knows the data changed. Keys never provisioned stay unloaded.
    virtual std::size_t refresh_keys(const std::vector<std::string>& keys) = 0;

    virtual bool is_loaded(const std::string& key) const = 0;
    virtual std::size_t loaded_count() const = 0;

    // Keys asked for that had no file. Surfaced rather than swallowed: the
    // missing set is not random -- it concentrates in delisted and newly listed
    // names -- so its size is a bias measurement.
    virtual std::vector<std::string> keys_without_data() const = 0;

    virtual void clear() = 0;
};

// Base for the common shape: one path per key, reloaded when its content
// fingerprint moves. A subclass supplies only where the data lives and how to
// parse it. The path may be a single file or a partitioned directory -- SEC
// facts are a year-partitioned tree per CIK, splits are one file per ticker --
// so the change check is a fingerprint hook rather than a bare file mtime.
//
// Payloads are immutable behind a shared_ptr. A reader takes a snapshot for the
// whole decision while a refresh swaps in a new one, so no reader observes a
// half-updated key and the decision path takes no lock.
template <typename Payload>
class FileBackedAltDataModule : public AltDataModule {
   public:
    std::size_t preload(const std::vector<std::string>& keys) override {
        std::size_t resolved = 0;
        std::vector<std::string> without_data;
        for (const auto& key : keys) {
            auto entry = load_entry(key);
            if (entry == nullptr) {
                without_data.push_back(key);
                continue;
            }
            std::lock_guard<std::mutex> lock(mutex_);
            entry_by_key_[key] = std::move(entry);
            ++resolved;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        keys_without_data_ = std::move(without_data);
        return resolved;
    }

    std::size_t refresh_changed() override {
        std::vector<std::string> keys;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            keys.reserve(entry_by_key_.size());
            for (const auto& [key, _] : entry_by_key_) keys.push_back(key);
        }
        std::size_t refreshed = 0;
        for (const auto& key : keys) {
            const long long fingerprint = content_fingerprint(path_for(key));
            if (fingerprint == 0) continue;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = entry_by_key_.find(key);
                if (it != entry_by_key_.end() &&
                    it->second->content_fingerprint == fingerprint) {
                    continue;
                }
            }
            // Parsed outside the lock, swapped in under it.
            auto entry = load_entry(key);
            if (entry == nullptr) continue;
            std::lock_guard<std::mutex> lock(mutex_);
            entry_by_key_[key] = std::move(entry);
            ++refreshed;
        }
        return refreshed;
    }

    std::size_t refresh_keys(const std::vector<std::string>& keys) override {
        std::size_t refreshed = 0;
        for (const auto& key : keys) {
            if (!is_loaded(key)) continue;
            auto entry = load_entry(key);
            if (entry == nullptr) continue;
            std::lock_guard<std::mutex> lock(mutex_);
            entry_by_key_[key] = std::move(entry);
            ++refreshed;
        }
        return refreshed;
    }

    bool is_loaded(const std::string& key) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return entry_by_key_.count(key) > 0;
    }

    std::size_t loaded_count() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return entry_by_key_.size();
    }

    std::vector<std::string> keys_without_data() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return keys_without_data_;
    }

    void clear() override {
        std::lock_guard<std::mutex> lock(mutex_);
        entry_by_key_.clear();
        keys_without_data_.clear();
    }

   protected:
    struct Entry {
        Payload payload;
        long long content_fingerprint = 0;
    };

    virtual std::string path_for(const std::string& key) const = 0;
    virtual Payload parse(const std::string& path) const = 0;

    // Changes when the data under `path` changes. Default is the file's mtime;
    // a directory-backed module overrides with the newest mtime beneath it.
    // Returning 0 means "cannot tell", which suppresses the refresh rather
    // than forcing one -- a re-parse per poll would be worse than a stale read.
    virtual long long content_fingerprint(const std::string& path) const {
        return file_mtime_ns(path);
    }
    // True when a key with no file is legitimate data rather than a gap --
    // most tickers never split, and that is a fact, not missing coverage.
    virtual bool absent_file_is_empty_payload() const { return false; }

    // nullopt when the key was never provisioned, which callers must not
    // collapse with "provisioned and empty".
    std::shared_ptr<const Entry> snapshot(const std::string& key) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entry_by_key_.find(key);
        return it == entry_by_key_.end() ? nullptr : it->second;
    }

    // Newest mtime under a partitioned directory tree; 0 when unreadable.
    static long long newest_mtime_ns_under(const std::string& path) {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) return 0;
        long long newest = 0;
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(path, ec)) {
            if (ec) return 0;
            if (!entry.is_regular_file(ec)) continue;
            const auto t = std::filesystem::last_write_time(entry.path(), ec);
            if (ec) continue;
            newest = std::max(
                newest, static_cast<long long>(t.time_since_epoch().count()));
        }
        return newest;
    }

    static long long file_mtime_ns(const std::string& path) {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) return 0;
        const auto t = std::filesystem::last_write_time(path, ec);
        if (ec) return 0;
        return static_cast<long long>(t.time_since_epoch().count());
    }

    std::shared_ptr<const Entry> load_entry(const std::string& key) const {
        const std::string path = path_for(key);
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            if (!absent_file_is_empty_payload()) return nullptr;
            return std::make_shared<const Entry>();
        }
        auto entry = std::make_shared<Entry>();
        entry->payload = parse(path);
        entry->content_fingerprint = content_fingerprint(path);
        return entry;
    }

   private:

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<const Entry>> entry_by_key_;
    std::vector<std::string> keys_without_data_;
};

}  // namespace MarketData
