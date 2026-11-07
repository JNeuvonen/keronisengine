#pragma once

#include <sqlite3.h>

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "market_data/stream_info.h"

namespace MarketData {

class StreamHealthMonitor {
   public:
    StreamHealthMonitor(const std::string& db_path,
                        std::chrono::steady_clock::time_point process_start);
    ~StreamHealthMonitor();

    void upsert_stream_status(const std::string& stream_key,
                              const std::string& stream_type,
                              const std::string& exchange_name,
                              const std::string& market_type,
                              const std::string& ws_url,
                              const std::vector<StreamInfo>& stream_infos,
                              bool is_connected, long long last_msg_age_ms,
                              long long total_msg_count,
                              double avg_msgs_per_sec, long long now_ms);

    std::chrono::steady_clock::time_point get_process_start() const {
        return process_start_;
    }

    void record_disconnect(const std::string& stream_key, long long now_ms);
    void record_reconnect(const std::string& stream_key, long long now_ms);
    void record_force_disconnect(const std::string& stream_key,
                                 long long now_ms,
                                 long long stale_duration_ms);

    void append_health_log(long long now_ms, int streams_alive,
                           int streams_total);

    struct EventCounts {
        int disconnects = 0;
        int reconnects = 0;
        int forced_disconnects = 0;
    };
    EventCounts get_event_counts(const std::string& stream_key);

    static std::string build_stream_key(
        const std::vector<StreamInfo>& stream_infos,
        const std::string& stream_type, int index);

   private:
    void init_tables();
    void exec_sql(const std::string& sql);

    std::string db_path_;
    sqlite3* db_ = nullptr;
    std::mutex mutex_;
    std::chrono::steady_clock::time_point process_start_;
    std::unordered_map<std::string, int> disconnect_counts_;
    std::unordered_map<std::string, int> reconnect_counts_;
    std::unordered_map<std::string, int> force_disconnect_counts_;
    std::unordered_set<std::string> was_disconnected_;
    int total_disconnects_ = 0;
};

}  // namespace MarketData
