#include "market_data/backfill_attempt_cache.h"

#include <filesystem>

#include "common_types/log_event_types.h"
#include "disk_logging/log.h"
#include "market_data/config.h"
#include "nlohmann/json.hpp"

namespace MarketData {

namespace {
std::string utc_day(long long ms) {
    const std::time_t s = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
    gmtime_r(&s, &tm);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday);
    return std::string(buf);
}
}  // namespace

BackfillAttemptCache& BackfillAttemptCache::instance() {
    static BackfillAttemptCache c;
    return c;
}

BackfillAttemptCache::BackfillAttemptCache() {
    const std::string path = MarketData::Config::getInstance().get_data_dir_root() +
                             "/caches/polygon-bar-backfill-attempts.db";
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_DB_CONNECT_FAILED,
                  "BackfillAttemptCache: failed to open db: " + path,
                  (nlohmann::json{{"db_path", path}}));
        db_ = nullptr;
        return;
    }
    exec("PRAGMA journal_mode=WAL;");
    exec("PRAGMA busy_timeout=5000;");
    exec(
        "CREATE TABLE IF NOT EXISTS cache (key TEXT PRIMARY KEY, value TEXT NOT "
        "NULL, created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP, updated_at "
        "TIMESTAMP DEFAULT CURRENT_TIMESTAMP);");
}

BackfillAttemptCache::~BackfillAttemptCache() {
    if (db_ != nullptr) sqlite3_close(db_);
}

void BackfillAttemptCache::exec(const std::string& sql) {
    if (db_ == nullptr) return;
    sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, nullptr);
}

std::string BackfillAttemptCache::read(const std::string& key) {
    if (db_ == nullptr) return "";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT value FROM cache WHERE key = ?", -1,
                           &stmt, nullptr) != SQLITE_OK) {
        return "";
    }
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    std::string out;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* v = sqlite3_column_text(stmt, 0);
        if (v != nullptr) out = reinterpret_cast<const char*>(v);
    }
    sqlite3_finalize(stmt);
    return out;
}

void BackfillAttemptCache::write(const std::string& key,
                                 const std::string& value) {
    if (db_ == nullptr) return;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "INSERT INTO cache (key, value) VALUES (?, ?) ON "
                           "CONFLICT(key) DO UPDATE SET value = excluded.value, "
                           "updated_at = CURRENT_TIMESTAMP",
                           -1, &stmt, nullptr) != SQLITE_OK) {
        return;
    }
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

bool BackfillAttemptCache::should_skip(const std::string& key,
                                       long long now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string raw = read(key);
    if (raw.empty()) return false;
    try {
        const auto j = nlohmann::json::parse(raw);
        const int streak = j.value("empty_streak", 0);
        const long long last = j.value("last_attempt_ms", 0LL);
        return streak >= kEmptyStreakToSkip && (now_ms - last) < kRetryAfterMs;
    } catch (...) {
        return false;
    }
}

void BackfillAttemptCache::record_empty(const std::string& key,
                                        long long now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    int streak = 0;
    std::string last_day;
    try {
        const std::string raw = read(key);
        if (!raw.empty()) {
            const auto j = nlohmann::json::parse(raw);
            streak = j.value("empty_streak", 0);
            last_day = j.value("last_day", "");
        }
    } catch (...) {
    }
    const std::string today = utc_day(now_ms);
    if (last_day != today) ++streak;
    write(key, nlohmann::json{{"empty_streak", streak},
                              {"last_attempt_ms", now_ms},
                              {"last_day", today}}
                   .dump());
}

void BackfillAttemptCache::record_data(const std::string& key,
                                       long long now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    write(key, nlohmann::json{{"empty_streak", 0},
                              {"last_attempt_ms", now_ms},
                              {"last_day", utc_day(now_ms)}}
                   .dump());
}

}  // namespace MarketData
