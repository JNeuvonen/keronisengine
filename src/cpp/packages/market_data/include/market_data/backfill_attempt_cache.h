#pragma once

#include <sqlite3.h>

#include <mutex>
#include <string>

namespace MarketData {

// Remembers which datasets keep coming back empty so a restart does not re-ask
// the whole universe. Backs onto the caches/ KV convention the raw-trades
// download trackers already use.
//
// A strike is only counted once per UTC day. Three restarts inside one bad
// afternoon would otherwise demote thousands of healthy symbols to the weekly
// path, which is exactly when the backfill can least afford it -- and a
// throttled fetch is indistinguishable from an empty one at the HTTP layer.
class BackfillAttemptCache {
   public:
    static BackfillAttemptCache& instance();

    // key: "<exchange>:<market_type>:<symbol>:<interval_ms>"
    bool should_skip(const std::string& key, long long now_ms);
    void record_empty(const std::string& key, long long now_ms);
    void record_data(const std::string& key, long long now_ms);

    static constexpr int kEmptyStreakToSkip = 3;
    static constexpr long long kRetryAfterMs = 7LL * 24 * 60 * 60 * 1000;

   private:
    BackfillAttemptCache();
    ~BackfillAttemptCache();
    void exec(const std::string& sql);
    std::string read(const std::string& key);
    void write(const std::string& key, const std::string& value);

    sqlite3* db_ = nullptr;
    std::mutex mutex_;
};

}  // namespace MarketData
