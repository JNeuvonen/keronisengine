#pragma once

#include <atomic>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <ankerl/unordered_dense.h>

#include "concurrent_queue_include_helper.h"
#include "market_data/bar_aggregator.h"
#include "market_data/market_event.h"
#include "market_data/sm_syncer.h"
#include "market_data/stream_health_monitor.h"
#include "market_data/stream_info.h"
#include "market_data/stream_status.h"
#include "market_making/dtypes/orderbook.h"

class WsStream;

using LogCallback = std::function<void(const std::string &)>;

namespace MarketData {

struct Store;

// --- Zero-allocation BBA pipeline ---

static constexpr uint32_t MAX_BBA_SLOTS = 16384;
static constexpr size_t BBA_PAYLOAD_SIZE = 512;

struct BbaSlot {
    std::atomic<uint64_t> seq{0};
    char payload[BBA_PAYLOAD_SIZE];
    size_t length{0};
};

struct BbaSlotMeta {
    std::string symbol;
    std::string exchange_name;
    std::string market_type;
};

// Transparent hash/eq for string_view lookups without allocation
struct BbaStringHash {
    using is_transparent = void;
    auto operator()(std::string_view sv) const noexcept -> uint64_t {
        return ankerl::unordered_dense::hash<std::string_view>{}(sv);
    }
};
struct BbaStringEq {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const noexcept {
        return a == b;
    }
};

// Symbol -> slot for one book-ticker connection. Immutable once the
// connection exists, so its handler reads it without a lock while other
// connections are opened.
using BbaSymbolSlots =
    ankerl::unordered_dense::map<std::string, uint32_t, BbaStringHash,
                                 BbaStringEq>;

// --- End BBA pipeline ---

// Book-builder state for one Lighter order_book channel (defined in the .cpp).
struct LighterBookStream;

struct MarketDataMsg {
    std::string exchange_name;
    std::string market_type;
    std::string msg;
    bool is_full_refresh = false;
    bool is_levels_only_refresh = false;
    std::string symbol;
    std::optional<Core::OrderBookLevels> full_refresh_orderbook;
};

class MarketDataStreams {
   public:
    static MarketDataStreams &get_instance();

    MarketDataStreams(const MarketDataStreams &) = delete;
    MarketDataStreams &operator=(const MarketDataStreams &) = delete;

    void set_is_ran_from_live_infra(bool is_ran_from_live_infra);
    // Off everywhere but the recorder, which serves the forming bars.
    void set_keep_forming_klines(bool keep);
    void set_live_event_callback(
        std::function<void(MarketData::MarketEvent &&)> callback);
    void add_ohlcv_config(const StreamsConfig &new_config);
    void add_lob_config(const StreamsConfig &new_config);
    void add_trade_config(const StreamsConfig &new_config);
    void add_book_ticker_config(const StreamsConfig &new_config);
    void add_ticker_config(const StreamsConfig &new_config);
    void add_polymarket_config(const std::string &ws_url);
    // Open or retire connections on a running process. A config identifies a
    // connection by its stream type and exact stream infos.
    void open_streams(const std::string &stream_type,
                      const std::vector<StreamsConfig> &configs);
    void retire_streams(const std::string &stream_type,
                        const std::vector<StreamsConfig> &configs);
    void send_polymarket_message(const std::string &msg);
    void send_polymarket_book_message(const std::string &msg);
    // Unsubscribe + subscribe on the open Lighter order_book connection that
    // carries `symbol`; the venue answers with a fresh snapshot.
    void resubscribe_lighter_order_book(const std::string &symbol,
                                        long long market_id);

    // Rebuilt once per health_check_loop pass (5s). Empty until the first
    // pass, which only runs after start_streams has built every stream.
    std::vector<StreamStatus> get_stream_statuses() const;

    void setup_aggregator_state();
    void set_log_callbacks(const LogCallback &error_log_callback,
                           const LogCallback &info_log_callback);
    void ohlcv_msg_consumer_thread();
    void lob_msg_consumer_thread();
    void trade_msg_consumer_thread();
    void book_ticker_msg_consumer_thread();
    void ticker_msg_consumer_thread();
    void polymarket_msg_consumer_thread();
    void check_completed_bars();

    void start_streams();
    void stop_streams();
    void health_check_loop();
    void run_refresh_orderbooks_check();
    void bars_utils_thread();
    void parse_polygon_ohlcv_message(const MarketDataMsg &ohlcv_msg,
                                     simdjson::dom::parser &parser);
    void parse_binance_ohlcv_message(const MarketDataMsg &ohlcv_msg,
                                     simdjson::dom::parser &parser);
    void parse_bybit_ohlcv_message(const MarketDataMsg &ohlcv_msg,
                                   simdjson::dom::parser &parser);
    void parse_bitget_ohlcv_message(const MarketDataMsg &ohlcv_msg,
                                    simdjson::dom::parser &parser);
    void parse_gateio_ohlcv_message(const MarketDataMsg &ohlcv_msg,
                                    simdjson::dom::parser &parser);
    std::optional<Core::OrderBookUpdate> parse_gateio_lob_message(
        const MarketDataMsg &lob_msg, simdjson::dom::parser &parser,
        std::unordered_map<std::string, Core::OrderBook> &orderbook_map);
    std::optional<Core::OrderBookUpdate> parse_binance_lob_message(
        const MarketDataMsg &lob_msg, simdjson::dom::parser &parser);
    void run_backup_fetch_check(
        const Store &store,
        std::unordered_map<std::string,
                           std::vector<std::chrono::steady_clock::time_point>>
            &backup_fetch_timestamps,
        std::unordered_map<std::string, std::unordered_map<long long, int>>
            &kline_attempt_counts);

    void backup_fetch_ohlcv_klines(
        const std::shared_ptr<OhlcvDataset> &dataset);

    void handle_ohlcv_message(const std::string &msg,
                              const std::string &exchange_name,
                              const std::string &market_type);
    void handle_lob_message(const std::string &msg,
                            const std::string &exchange_name,
                            const std::string &market_type);
    void handle_trade_message(const std::string &msg,
                              const std::string &exchange_name,
                              const std::string &market_type);
    void handle_book_ticker_message(const std::string &msg,
                                    const BbaSymbolSlots &slots);
    void handle_ticker_message(const std::string &msg,
                               const std::string &exchange_name,
                               const std::string &market_type);

    void handle_polymarket_message(const std::string &msg);
    void enqueue_full_refresh(const std::string &exchange_name,
                              const std::string &market_type,
                              const std::string &symbol,
                              const Core::OrderBookLevels &levels);
    void enqueue_levels_only_refresh(const std::string &exchange_name,
                                     const std::string &market_type,
                                     const std::string &symbol,
                                     const Core::OrderBookLevels &levels);

   private:
    moodycamel::BlockingConcurrentQueue<MarketDataMsg> ohlcv_queue;
    moodycamel::BlockingConcurrentQueue<MarketDataMsg> lob_queue;
    moodycamel::BlockingConcurrentQueue<MarketDataMsg> trade_queue;
    moodycamel::BlockingConcurrentQueue<MarketDataMsg> ticker_queue;
    moodycamel::BlockingConcurrentQueue<MarketDataMsg> polymarket_queue;

    // Zero-allocation BBA pipeline. Slot metadata is written once, before the
    // connection that fills the slot exists, and never moves.
    std::unique_ptr<BbaSlot[]> book_states_;
    std::unique_ptr<BbaSlotMeta[]> bba_slot_metadata_;
    moodycamel::BlockingConcurrentQueue<uint32_t> bba_notify_queue_;
    std::mutex bba_registry_mutex_;
    std::unordered_map<std::string, uint32_t> bba_slot_ids_;
    uint32_t next_bba_slot_id_{0};
    std::shared_ptr<const BbaSymbolSlots> assign_bba_slots(
        const std::vector<StreamInfo> &stream_infos);
    void open_stream_locked(const std::string &stream_type,
                            const StreamsConfig &config);
    void consume_lighter_lob_message(
        const MarketDataMsg &msg, simdjson::dom::parser &parser,
        std::unordered_map<long long, LighterBookStream> &books);
    void consume_lighter_trade_message(
        const MarketDataMsg &msg, simdjson::dom::element doc,
        std::unordered_map<long long, std::string> &symbols_by_market_id,
        std::unordered_map<long long, long long> &last_price_emit_ms);
    MarketDataStreams() noexcept;
    ~MarketDataStreams() noexcept;

    BarAggregator &bar_aggregator;
    std::vector<StreamsConfig> ohlcv_configs;
    std::vector<StreamsConfig> lob_configs;
    std::vector<StreamsConfig> trade_configs;
    std::vector<StreamsConfig> book_ticker_configs;
    std::vector<StreamsConfig> ticker_configs;
    // Guards streams and retired_streams. Retired streams stay alive: a
    // detached reconnect thread may still hold a pointer to one.
    mutable std::mutex streams_mutex_;
    std::vector<StreamMeta> streams;
    std::vector<StreamMeta> retired_streams;
    LogCallback error_log_callback;
    LogCallback info_log_callback;
    long long msgs_count{0};
    long long total_latency_to_parse_ms{0};
    long long total_latency_to_process_micros{0};

    std::atomic<bool> should_stop_reconnecting{false};

    std::chrono::steady_clock::time_point last_spawned_sync_to_disk_thread_time{
        std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point last_spawned_live_sync_thread_time{
        std::chrono::steady_clock::now()};

    std::chrono::steady_clock::time_point last_trade_msg_count_log_time{
        std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point last_logged_streams_status_time{
        std::chrono::steady_clock::now()};

    long long trade_msgs_count{0};
    long long trade_total_latency_to_parse_ms{0};
    long long trade_total_latency_to_process_micros{0};
    long long bar_emit_min_latency_ms{std::numeric_limits<long long>::max()};
    long long bar_emit_max_latency_ms{0};
    std::chrono::steady_clock::time_point last_trade_throughput_log_time{
        std::chrono::steady_clock::now()};

    bool is_ran_from_live_infra{false};
    std::atomic<bool> keep_forming_klines{false};
    std::function<void(MarketData::MarketEvent &&)> live_event_callback_;
    std::string polymarket_ws_url_;
    std::unique_ptr<StreamHealthMonitor> health_monitor_;
    std::unique_ptr<StateMachineSyncer> sm_syncer_;
    mutable std::mutex stream_statuses_mutex_;
    std::vector<StreamStatus> stream_statuses_;
};
};  // namespace MarketData