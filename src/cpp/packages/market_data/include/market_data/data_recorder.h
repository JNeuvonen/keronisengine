#pragma once

#include <atomic>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "market_data/market_data_coverage.h"

#include "concurrent_queue_include_helper.h"

namespace MarketData {

class DataRecorder {
   public:
    static DataRecorder& getInstance();

    DataRecorder(const DataRecorder&) = delete;
    DataRecorder& operator=(const DataRecorder&) = delete;

    // Called from WS handler threads. No-op when disabled.
    void record(const std::string& stream_type,
                const std::string& exchange_name,
                const std::string& market_type, const std::string& raw_msg);

    // Fast path: caller already extracted the symbol, skip re-parsing JSON
    void record_with_symbol(const std::string& stream_type,
                            const std::string& exchange_name,
                            const std::string& market_type,
                            const std::string& symbol,
                            const std::string& raw_msg);

    // Strategy event recording — bypasses config filtering, always records
    // if enabled. dedup_key: unique string for this event; duplicate keys
    // are silently dropped. json_line: pre-serialized JSON string.
    void record_strategy_event(const std::string& strategy_name,
                               const std::string& dedup_key,
                               const std::string& json_line);

    // Which raw streams are written; replaced whole on a coverage reload.
    void set_recording_keys(const std::vector<ResolvedRecordingKey>& keys);

    bool is_enabled() const;
    void shutdown();

   private:
    DataRecorder();
    ~DataRecorder();

    struct RecordMsg {
        std::string stream_type;
        std::string exchange_name;
        std::string market_type;
        std::string symbol;
        std::string raw_msg;
        std::string dedup_key;
        long long timestamp_ms;
    };

    [[nodiscard]] bool is_recorded(const std::string& base_key,
                                   const std::string& symbol) const;
    void writer_thread_loop();
    std::string build_file_path(const std::string& stream_type,
                                const std::string& exchange_name,
                                const std::string& market_type,
                                const std::string& symbol,
                                long long timestamp_ms) const;

    bool enabled_{false};
    std::string data_dir_root_;

    mutable std::shared_mutex keys_mutex_;
    std::unordered_set<std::string>
        recording_keys_;  // "stream_type|exchange|market_type|symbol"
    std::unordered_set<std::string>
        wildcard_keys_;  // "stream_type|exchange|market_type" (for "*")

    std::string build_strategy_file_path(const std::string& strategy_name,
                                        long long timestamp_ms) const;

    moodycamel::ConcurrentQueue<RecordMsg> queue_;
    std::atomic<bool> should_stop_{false};
    std::thread writer_thread_;

    // Strategy event dedup — only accessed on writer thread, no mutex needed.
    std::unordered_set<std::string> strategy_dedup_set_;
    long long dedup_last_clear_ts_{0};
};

}  // namespace MarketData
