#pragma once

#include <sqlite3.h>

#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

template <typename T>
class SqliteCache {
   public:
    explicit SqliteCache(const std::string& cache_path)
        : cache_path_(cache_path) {
        ensure_cache_dir();
        init_db();
    }

    ~SqliteCache() {
        if (db_) {
            sqlite3_close(db_);
        }
    }

    void insert(const std::string& key, const T& value,
                bool allow_cache = false) {
        std::string value_str = serialize_value(value);
        std::string sql =
            "INSERT OR REPLACE INTO cache (key, value, updated_at) "
            "VALUES (?, ?, CURRENT_TIMESTAMP)";

        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) ==
            SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, value_str.c_str(), -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            if (allow_cache) {
                cache_[key] = value;
            }
        }
    }

    std::optional<T> get(const std::string& key, bool allow_cache = false) {
        if (allow_cache && cache_.find(key) != cache_.end()) {
            return cache_[key];
        }
        std::string sql = "SELECT value FROM cache WHERE key = ?";
        sqlite3_stmt* stmt;

        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) ==
            SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_STATIC);

            if (sqlite3_step(stmt) == SQLITE_ROW) {
                std::string value_str(reinterpret_cast<const char*>(
                    sqlite3_column_text(stmt, 0)));
                T value = deserialize_value(value_str);
                if (allow_cache) {
                    cache_[key] = value;
                }
                sqlite3_finalize(stmt);
                return value;
            }
            sqlite3_finalize(stmt);
        }
        return std::nullopt;
    }

    void remove(const std::string& key) {
        std::string sql = "DELETE FROM cache WHERE key = ?";
        sqlite3_stmt* stmt;

        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) ==
            SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    void clear() {
        std::string sql = "DELETE FROM cache";
        sqlite3_stmt* stmt;

        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) ==
            SQLITE_OK) {
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    std::vector<std::pair<std::string, T>> query(const std::string& query) {
        std::vector<std::pair<std::string, T>> results;
        sqlite3_stmt* stmt;

        if (sqlite3_prepare_v2(db_, query.c_str(), -1, &stmt, nullptr) !=
            SQLITE_OK) {
            std::string error = sqlite3_errmsg(db_);
            throw std::runtime_error("Failed to prepare query: " + error);
        }

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string key(
                reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
            std::string value_str(
                reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
            results.push_back({key, deserialize_value(value_str)});
        }

        sqlite3_finalize(stmt);
        return results;
    }

    std::unordered_map<std::string, T> get_all() {
        std::unordered_map<std::string, T> result;
        std::string sql = "SELECT key, value FROM cache";
        sqlite3_stmt* stmt;

        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) ==
            SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                std::string key(reinterpret_cast<const char*>(
                    sqlite3_column_text(stmt, 0)));
                std::string value_str(reinterpret_cast<const char*>(
                    sqlite3_column_text(stmt, 1)));
                result[key] = deserialize_value(value_str);
            }
            sqlite3_finalize(stmt);
        }
        return result;
    }

   private:
    void ensure_cache_dir() {
        std::filesystem::create_directories(
            std::filesystem::path(cache_path_).parent_path());
    }

    void init_db() {
        if (sqlite3_open(cache_path_.c_str(), &db_) != SQLITE_OK) {
            throw std::runtime_error("Failed to open database");
        }

        sqlite3_busy_timeout(db_, 5000);
        sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr,
                     nullptr);

        std::string sql = R"(
            CREATE TABLE IF NOT EXISTS cache (
                key TEXT PRIMARY KEY,
                value TEXT NOT NULL,
                created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
                updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
            )
        )";

        char* err_msg = nullptr;
        if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err_msg) !=
            SQLITE_OK) {
            std::string error = err_msg;
            sqlite3_free(err_msg);
            throw std::runtime_error("Failed to create table: " + error);
        }
    }

    std::string serialize_value(const T& value) {
        return nlohmann::json(value).dump();
    }

    T deserialize_value(const std::string& value_str) {
        return nlohmann::json::parse(value_str).get<T>();
    }

    sqlite3* db_;
    std::string cache_path_;
    std::unordered_map<std::string, T> cache_;
};