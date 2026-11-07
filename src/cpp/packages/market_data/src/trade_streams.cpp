#include "market_data/trade_streams.h"

#include <chrono>
#include <thread>
#include <unordered_set>

#include "common_types/log_event_types.h"
#include "disk_logging/log.h"
#include "market_data/bar_aggregator.h"
#include "market_data/data_recorder.h"
#include "market_data/dataset_manager.h"
#include "market_data/normalized_trade.h"
#include "market_data/utils.h"
#include "market_making/engine.h"
#include "market_making/rate_limited_emitter.h"
#include "market_making/utils.h"
#include "nlohmann/json.hpp"
#include "perf_measure.h"
#include "simdjson.h"
#include "ws_stream.h"

// Hot-path guard: trade_msg_consumer_thread() processes every message pushed
// to trade_queue. A bad-data burst on the venue could fire the catch branch
// at kHz rate; a sync disk write per message would stall the thread and
// back-pressure the queue. We route through Engine::create_system_log
// (lock-free trading_logs queue) and rate-limit to one emit per 10s per
// (exchange, market, error_class) key. Suppressed-count preserved in
// metadata. Mirrors the pattern in market_data_streams.cpp.
namespace {
Core::RateLimitedEmitter g_trade_parse_error_emitter;

void emit_trade_parse_error(const std::string& exchange_name,
                             const std::string& market_type,
                             const std::string& what,
                             const std::string& msg_head) {
    g_trade_parse_error_emitter.emit_if_allowed(
        "trade_parse|" + exchange_name + "|" + market_type, 10000,
        [&](uint64_t suppressed) {
            nlohmann::json md = {
                {"exchange_name", exchange_name},
                {"market_type", market_type},
                {"what", what},
                {"msg_head", msg_head},
                {"events_suppressed_since_last_emit", suppressed},
            };
            Core::Engine::getInstance().create_system_log(
                "", Core::LOG_LEVEL_ERROR,
                std::string("Trade parse error: ") + what,
                Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_STREAM_PARSE_ERROR,
                md, -1, exchange_name, market_type);
        });
}
}  // namespace

MarketData::TradeStreams::TradeStreams() noexcept
    : bar_aggregator(BarAggregator::getInstance()) {
    std::thread([this]() { health_check_loop(); }).detach();
    std::thread([this]() { check_completed_bars(); }).detach();
    std::thread([this]() { trade_msg_consumer_thread(); }).detach();
}

MarketData::TradeStreams::~TradeStreams() noexcept {
    // Destroy streams
}

void MarketData::TradeStreams::add_config(const StreamsConfig &new_config) {
    configs.push_back(new_config);
}

void MarketData::TradeStreams::handle_trade_message(
    const std::string &msg, const std::string &exchange_name,
    const std::string &market_type) {
    DataRecorder::getInstance().record(
        WsStreamConstants::TRADE_STREAM_TYPE, exchange_name, market_type, msg);
    TradeMsg trade_msg;
    trade_msg.exchange_name = exchange_name;
    trade_msg.market_type = market_type;
    trade_msg.msg = msg;
    trade_queue.enqueue(trade_msg);
}

void MarketData::TradeStreams::trade_msg_consumer_thread() {
    thread_local simdjson::dom::parser parser;
    while (true) {
        TradeMsg trade_msg;
        if (trade_queue.wait_dequeue_timed(
                trade_msg, std::chrono::milliseconds(100))) {
            auto start_time = std::chrono::steady_clock::now();
            try {
                simdjson::padded_string padded(trade_msg.msg);
                auto doc = parser.parse(padded);
                const auto &trade =
                    MarketData::NormalizedParsedTrade::from_simdjson(
                        doc["data"], trade_msg.exchange_name,
                        trade_msg.market_type);
                BarAggregator::getInstance().add_trade(trade, true);
                ++msgs_count;
                total_latency_to_parse_ms += trade.latency_ms;
                auto end_time = std::chrono::steady_clock::now();
                total_latency_to_process_micros +=
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        end_time - start_time)
                        .count();
            } catch (const std::exception &e) {
                emit_trade_parse_error(
                    trade_msg.exchange_name, trade_msg.market_type,
                    e.what(),
                    trade_msg.msg.substr(
                        0, std::min<size_t>(200, trade_msg.msg.size())));
            }
        }
    }
}

void MarketData::TradeStreams::start_streams() {
    // Start streams
    for (const auto &config : configs) {
        if (config.streams.size() == 0) {
            continue;
        }

        const std::string exchange_name = config.streams[0].exchange_name;
        const std::string market_type = config.streams[0].market_type;

        const auto &stream_infos = MarketData::get_stream_infos(
            exchange_name, market_type, MarketData::TRADE_STREAM_TYPE,
            config.streams);

        const std::string stream_url = MarketData::get_stream_url(
            exchange_name, market_type, MarketData::TRADE_STREAM_TYPE,
            std::nullopt, stream_infos);
        auto stream_ptr = std::make_unique<WsStream>(
            stream_url,
            [this, exchange_name, market_type](const std::string &msg) {
                handle_trade_message(msg, exchange_name, market_type);
            },
            "", info_log_callback, error_log_callback);
        stream_ptr->connect();
        streams.push_back(StreamMeta{stream_infos, std::move(stream_ptr)});
        LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_STREAM_CONNECTED,
                 "Connected to stream: " + stream_url,
                 (nlohmann::json{{"stream_url", stream_url}}));
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void MarketData::TradeStreams::stop_streams() {
    LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_STREAM_STOPPING,
             "Stopping streams", nlohmann::json{});
    for (const auto &stream : this->streams) {
        stream.stream->disconnect();
    }
    this->should_stop_reconnecting = true;
}

MarketData::TradeStreams &MarketData::TradeStreams::get_instance() {
    static TradeStreams instance;
    return instance;
}

void MarketData::TradeStreams::set_log_callbacks(
    const LogCallback &error_log_callback,
    const LogCallback &info_log_callback) {
    this->error_log_callback = error_log_callback;
    this->info_log_callback = info_log_callback;
}

void MarketData::TradeStreams::health_check_loop() {
    while (!this->should_stop_reconnecting) {
        long long streams_alive = 0;
        for (const auto &stream : this->streams) {
            stream.stream->send_ping();

            if (stream.stream->is_disconnected()) {
                if (stream.stream_infos.size() > 0) {
                    this->error_log_callback(
                        "Reconnecting to stream: " +
                        stream.stream_infos[0].exchange_name + ":" +
                        stream.stream_infos[0].market_type);
                }
                for (const auto &stream_info : stream.stream_infos) {
                    BarAggregator::getInstance()
                        .live_trade_stream_disconnected_reset(
                            stream_info.exchange_name, stream_info.market_type,
                            stream_info.symbol);
                }
                stream.stream->connect();
            } else {
                ++streams_alive;
            }

            const auto &last_msg_received =
                stream.stream->get_last_msg_received();

            if (std::chrono::steady_clock::now() - last_msg_received >
                std::chrono::seconds(60 * 5)) {
                this->error_log_callback(
                    "Stream " + stream.stream_infos[0].exchange_name + ":" +
                    stream.stream_infos[0].market_type +
                    " is not receiving messages, its status is: " +
                    (stream.stream->is_disconnected() ? "disconnected"
                                                      : "connected"));
            }
        }
        if (std::chrono::steady_clock::now() -
                this->last_logged_streams_status_time >
            std::chrono::seconds(60 * 5)) {
            this->info_log_callback(
                "Streams alive: " + std::to_string(streams_alive) + "/" +
                std::to_string(this->streams.size()));
            this->last_logged_streams_status_time =
                std::chrono::steady_clock::now();
        }
        std::this_thread::sleep_for(std::chrono::seconds(5));
        BarAggregator::getInstance().reread_klines_from_disk();
    }
}

void MarketData::TradeStreams::check_completed_bars() {
    while (true) {
        const auto &completed_bars =
            BarAggregator::getInstance().emit_closed_bars();

        if (completed_bars.size() > 0) {
            LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_BAR_SYNC_PROGRESS,
                     "Completed bars: " +
                         std::to_string(completed_bars.size()),
                     (nlohmann::json{{"phase", "bars_completed"},
                                     {"num_bars", completed_bars.size()}}));
        }
        for (const auto &bar : completed_bars) {
            MarketData::DatasetsManager::get_instance().add_ohlcv_tick(bar);
        }
        if (this->last_spawned_live_sync_thread_time +
                std::chrono::seconds(10) <
            std::chrono::steady_clock::now()) {
            this->last_spawned_live_sync_thread_time =
                std::chrono::steady_clock::now();
            std::thread([this]() {
                this->bar_aggregator.sync_live_streamed_bars();
            }).detach();
        }

        if (this->last_spawned_sync_to_disk_thread_time +
                std::chrono::seconds(10) <
            std::chrono::steady_clock::now()) {
            this->last_spawned_sync_to_disk_thread_time =
                std::chrono::steady_clock::now();
            std::thread([this]() {
                MarketData::DatasetsManager::get_instance()
                    .sync_ohlcv_datasets_to_disk();
            }).detach();
        }

        if (this->last_trade_msg_count_log_time + std::chrono::seconds(15) <
            std::chrono::steady_clock::now()) {
            auto now = std::chrono::steady_clock::now();
            auto duration_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - this->last_trade_msg_count_log_time)
                    .count();
            double messages_per_second =
                duration_ms > 0
                    ? (static_cast<double>(msgs_count) * 1000.0) / duration_ms
                    : 0.0;
            double avg_latency_ms =
                msgs_count > 0
                    ? (static_cast<double>(total_latency_to_parse_ms) /
                       msgs_count)
                    : 0.0;
            double avg_latency_to_process_micros =
                msgs_count > 0
                    ? (static_cast<double>(total_latency_to_process_micros) /
                       msgs_count)
                    : 0.0;

            this->last_trade_msg_count_log_time = now;
            LOG_INFO(
                Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_STREAM_THROUGHPUT,
                "Throughput: " +
                    std::to_string(static_cast<int>(messages_per_second)) +
                    " msgs/sec | avg event→parsed: " +
                    std::to_string(avg_latency_ms) +
                    " ms | avg parse→process: " +
                    std::to_string(avg_latency_to_process_micros) + " μs",
                (nlohmann::json{
                    {"messages_per_second",
                     static_cast<int>(messages_per_second)},
                    {"avg_latency_to_parse_ms", avg_latency_ms},
                    {"avg_latency_to_process_micros",
                     avg_latency_to_process_micros},
                    {"window_duration_ms", duration_ms},
                    {"msg_count", msgs_count}}));

            msgs_count = 0;
            total_latency_to_parse_ms = 0;
            total_latency_to_process_micros = 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
