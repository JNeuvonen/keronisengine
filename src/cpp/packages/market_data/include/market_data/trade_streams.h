#pragma once

#include <functional>
#include <string>
#include <vector>

#include "bar_aggregator.h"
#include "concurrent_queue_include_helper.h"
#include "market_data/stream_info.h"

class WsStream;

using LogCallback = std::function<void(const std::string &)>;

namespace MarketData {

constexpr const char *TRADE_STREAM_TYPE = "trade";

struct TradeMsg {
    std::string exchange_name;
    std::string market_type;
    std::string msg;
};

class TradeStreams {
   public:
    static TradeStreams &get_instance();

    TradeStreams(const TradeStreams &) = delete;
    TradeStreams &operator=(const TradeStreams &) = delete;

    void add_config(const StreamsConfig &new_config);
    void setup_aggregator_state();
    void set_log_callbacks(const LogCallback &error_log_callback,
                           const LogCallback &info_log_callback);
    void trade_msg_consumer_thread();
    void start_streams();
    void stop_streams();
    void health_check_loop();
    void check_completed_bars();
    void handle_trade_message(const std::string &msg,
                              const std::string &exchange_name,
                              const std::string &market_type);

    bool get_is_synced_with_live(const std::string &symbol_exchange_market_key);

    std::chrono::steady_clock::time_point last_spawned_live_sync_thread_time;
    std::chrono::steady_clock::time_point last_spawned_sync_to_disk_thread_time;

   private:
    moodycamel::BlockingConcurrentQueue<TradeMsg> trade_queue;
    TradeStreams() noexcept;
    ~TradeStreams() noexcept;

    BarAggregator &bar_aggregator;
    std::vector<StreamsConfig> configs;
    std::vector<StreamMeta> streams;
    LogCallback error_log_callback;
    LogCallback info_log_callback;
    long long msgs_count{0};
    long long total_latency_to_parse_ms{0};
    long long total_latency_to_process_micros{0};
    std::atomic<bool> should_stop_reconnecting{false};

    std::chrono::steady_clock::time_point last_trade_msg_count_log_time{
        std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point last_logged_streams_status_time{
        std::chrono::steady_clock::now()};
};

}  // namespace MarketData
