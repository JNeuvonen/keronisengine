#include "market_data/stream_health_monitor.h"

#include <filesystem>

#include "common_types/log_event_types.h"
#include "datetime_utils.h"
#include "disk_logging/log.h"
#include "nlohmann/json.hpp"

namespace MarketData {

StreamHealthMonitor::StreamHealthMonitor(
    const std::string& db_path,
    std::chrono::steady_clock::time_point process_start)
    : db_path_(db_path), process_start_(process_start) {
    std::filesystem::create_directories(
        std::filesystem::path(db_path_).parent_path());

    int rc = sqlite3_open(db_path_.c_str(), &db_);
    if (rc != SQLITE_OK) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_DB_CONNECT_FAILED,
                  "StreamHealthMonitor: failed to open db: " + db_path_,
                  (nlohmann::json{{"target_db", "stream_health_monitor_sqlite"},
                                  {"db_path", db_path_}}));
        db_ = nullptr;
        return;
    }
    exec_sql("PRAGMA journal_mode=WAL;");
    exec_sql("PRAGMA busy_timeout=5000;");
    init_tables();
}

StreamHealthMonitor::~StreamHealthMonitor() {
    if (db_) {
        sqlite3_close(db_);
    }
}

void StreamHealthMonitor::init_tables() {
    exec_sql(R"(
        CREATE TABLE IF NOT EXISTS stream_status (
            stream_key TEXT PRIMARY KEY,
            stream_type TEXT NOT NULL,
            exchange_name TEXT,
            market_type TEXT,
            ws_url TEXT,
            num_symbols INTEGER,
            symbols_json TEXT,
            is_connected INTEGER NOT NULL,
            last_msg_age_ms INTEGER,
            disconnect_count INTEGER DEFAULT 0,
            reconnect_count INTEGER DEFAULT 0,
            last_disconnect_ts_ms INTEGER,
            last_reconnect_ts_ms INTEGER,
            total_msg_count INTEGER DEFAULT 0,
            avg_msgs_per_sec REAL DEFAULT 0.0,
            force_disconnect_count INTEGER DEFAULT 0,
            last_force_disconnect_ts_ms INTEGER,
            updated_at_ms INTEGER NOT NULL
        );
    )");
    exec_sql(R"(
        CREATE TABLE IF NOT EXISTS health_log (
            timestamp_ms INTEGER NOT NULL,
            streams_alive INTEGER NOT NULL,
            streams_total INTEGER NOT NULL,
            total_disconnects INTEGER NOT NULL,
            process_uptime_seconds INTEGER NOT NULL
        );
    )");
}

void StreamHealthMonitor::exec_sql(const std::string& sql) {
    if (!db_) return;
    char* err = nullptr;
    sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err);
    if (err) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_DB_WRITE_FAILED,
                  "StreamHealthMonitor SQL error: " + std::string(err),
                  (nlohmann::json{{"target_db", "stream_health_monitor_sqlite"},
                                  {"sqlite_error", std::string(err)},
                                  {"failed_sql", sql}}));
        sqlite3_free(err);
    }
}

std::string StreamHealthMonitor::build_stream_key(
    const std::vector<StreamInfo>& stream_infos,
    const std::string& stream_type, int index) {
    if (stream_infos.empty()) {
        return "polymarket::" + stream_type + ":" + std::to_string(index);
    }
    return stream_infos[0].exchange_name + ":" +
           stream_infos[0].market_type + ":" + stream_type + ":" +
           std::to_string(index);
}

void StreamHealthMonitor::upsert_stream_status(
    const std::string& stream_key, const std::string& stream_type,
    const std::string& exchange_name, const std::string& market_type,
    const std::string& ws_url, const std::vector<StreamInfo>& stream_infos,
    bool is_connected, long long last_msg_age_ms,
    long long total_msg_count, double avg_msgs_per_sec, long long now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return;

    // Track reconnects: was disconnected last cycle, now connected
    if (is_connected && was_disconnected_.count(stream_key)) {
        was_disconnected_.erase(stream_key);
        reconnect_counts_[stream_key]++;
    }
    if (!is_connected) {
        was_disconnected_.insert(stream_key);
    }

    nlohmann::json symbols = nlohmann::json::array();
    for (const auto& info : stream_infos) {
        symbols.push_back(info.symbol);
    }

    int dc = disconnect_counts_[stream_key];
    int rc = reconnect_counts_[stream_key];

    const std::string sql = R"(
        INSERT INTO stream_status (stream_key, stream_type, exchange_name,
            market_type, ws_url, num_symbols, symbols_json, is_connected,
            last_msg_age_ms, disconnect_count, reconnect_count,
            last_disconnect_ts_ms, last_reconnect_ts_ms,
            total_msg_count, avg_msgs_per_sec, updated_at_ms)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?,
            (SELECT last_disconnect_ts_ms FROM stream_status WHERE stream_key = ?),
            (SELECT last_reconnect_ts_ms FROM stream_status WHERE stream_key = ?),
            ?, ?, ?)
        ON CONFLICT(stream_key) DO UPDATE SET
            is_connected = excluded.is_connected,
            last_msg_age_ms = excluded.last_msg_age_ms,
            disconnect_count = excluded.disconnect_count,
            reconnect_count = excluded.reconnect_count,
            total_msg_count = excluded.total_msg_count,
            avg_msgs_per_sec = excluded.avg_msgs_per_sec,
            updated_at_ms = excluded.updated_at_ms;
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) !=
        SQLITE_OK) {
        return;
    }

    std::string symbols_str = symbols.dump();
    sqlite3_bind_text(stmt, 1, stream_key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, stream_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, exchange_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, market_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, ws_url.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 6, static_cast<int>(stream_infos.size()));
    sqlite3_bind_text(stmt, 7, symbols_str.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 8, is_connected ? 1 : 0);
    sqlite3_bind_int64(stmt, 9, last_msg_age_ms);
    sqlite3_bind_int(stmt, 10, dc);
    sqlite3_bind_int(stmt, 11, rc);
    sqlite3_bind_text(stmt, 12, stream_key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 13, stream_key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 14, total_msg_count);
    sqlite3_bind_double(stmt, 15, avg_msgs_per_sec);
    sqlite3_bind_int64(stmt, 16, now_ms);

    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void StreamHealthMonitor::record_disconnect(const std::string& stream_key,
                                            long long now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    disconnect_counts_[stream_key]++;
    total_disconnects_++;
    was_disconnected_.insert(stream_key);

    if (!db_) return;
    const std::string sql =
        "UPDATE stream_status SET last_disconnect_ts_ms = " +
        std::to_string(now_ms) + ", disconnect_count = " +
        std::to_string(disconnect_counts_[stream_key]) +
        " WHERE stream_key = '" + stream_key + "';";
    exec_sql(sql);
}

void StreamHealthMonitor::record_reconnect(const std::string& stream_key,
                                           long long now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    reconnect_counts_[stream_key]++;

    if (!db_) return;
    const std::string sql =
        "UPDATE stream_status SET last_reconnect_ts_ms = " +
        std::to_string(now_ms) + ", reconnect_count = " +
        std::to_string(reconnect_counts_[stream_key]) +
        " WHERE stream_key = '" + stream_key + "';";
    exec_sql(sql);
}

void StreamHealthMonitor::record_force_disconnect(
    const std::string& stream_key, long long now_ms,
    long long stale_duration_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    force_disconnect_counts_[stream_key]++;

    if (!db_) return;
    const std::string sql =
        "UPDATE stream_status SET force_disconnect_count = " +
        std::to_string(force_disconnect_counts_[stream_key]) +
        ", last_force_disconnect_ts_ms = " + std::to_string(now_ms) +
        " WHERE stream_key = '" + stream_key + "';";
    exec_sql(sql);
}

StreamHealthMonitor::EventCounts StreamHealthMonitor::get_event_counts(
    const std::string& stream_key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto count_of = [&](const std::unordered_map<std::string, int>& counts) {
        auto it = counts.find(stream_key);
        return it == counts.end() ? 0 : it->second;
    };
    return {count_of(disconnect_counts_), count_of(reconnect_counts_),
            count_of(force_disconnect_counts_)};
}

void StreamHealthMonitor::append_health_log(long long now_ms,
                                            int streams_alive,
                                            int streams_total) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return;

    auto uptime_s = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now() - process_start_)
                        .count();

    const std::string sql =
        "INSERT INTO health_log (timestamp_ms, streams_alive, streams_total, "
        "total_disconnects, process_uptime_seconds) VALUES (" +
        std::to_string(now_ms) + ", " + std::to_string(streams_alive) + ", " +
        std::to_string(streams_total) + ", " +
        std::to_string(total_disconnects_) + ", " +
        std::to_string(uptime_s) + ");";
    exec_sql(sql);
}

}  // namespace MarketData
