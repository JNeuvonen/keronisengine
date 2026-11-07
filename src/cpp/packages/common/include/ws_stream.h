#pragma once

#include <asio/ssl/context.hpp>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <websocketpp/client.hpp>
#include <websocketpp/config/asio_client.hpp>

#include "market_data/stream_info.h"

namespace WsStreamConstants {

constexpr const char* OHLCV_STREAM_TYPE = "klines";
constexpr const char* LOB_STREAM_TYPE = "lob";
constexpr const char* TRADE_STREAM_TYPE = "trade";
constexpr const char* BOOK_TICKER_STREAM_TYPE = "book_ticker";
constexpr const char* POLYMARKET_STREAM_TYPE = "polymarket";
constexpr const char* POLYMARKET_BOOK_STREAM_TYPE = "polymarket_book";

}  // namespace WsStreamConstants

using MessageCallback = std::function<void(const std::string&)>;
using LogCallback = std::function<void(const std::string&)>;
using WsClientTls = websocketpp::client<websocketpp::config::asio_tls_client>;
using WsClientPlain = websocketpp::client<websocketpp::config::asio_client>;
using WsClientVariant =
    std::variant<std::unique_ptr<WsClientTls>, std::unique_ptr<WsClientPlain>>;

class WsStream {
   public:
    std::string wsurl_;
    MessageCallback message_callback_;
    WsClientVariant ws_client_;
    websocketpp::connection_hdl ws_hdl_;
    std::thread ws_thread_;
    std::string bind_ip_;
    std::atomic<bool> is_disconnected_;
    std::atomic<bool> is_initialized_;
    std::atomic<std::chrono::steady_clock::time_point> last_ping_received_;
    std::atomic<std::chrono::steady_clock::time_point> last_ping_sent_;
    std::atomic<std::chrono::steady_clock::time_point> last_msg_received_ =
        std::chrono::steady_clock::now();
    std::atomic<long long> msg_count_{0};
    std::mutex ws_mutex;

    std::optional<std::string> exchange_name_ = std::nullopt;
    std::optional<std::string> market_type_ = std::nullopt;
    std::vector<MarketData::StreamInfo> stream_infos_;
    std::string stream_type_;
    std::vector<std::string> on_open_raw_messages_;

    WsStream(const std::string& wsurl, MessageCallback message_callback,
             const std::string& bind_ip = "",
             LogCallback info_callback = nullptr,
             LogCallback error_callback = nullptr);

    ~WsStream();

    void set_stream_infos(
        const std::string& exchange_name, const std::string& market_type,
        const std::string& stream_type,
        const std::vector<MarketData::StreamInfo>& stream_infos);

    // Raw messages sent on each (re)connect, before any stream-info subscribe.
    // Used by subscription-auth venues (e.g. Hyperliquid user streams).
    void set_on_open_messages(const std::vector<std::string>& messages);

    void connect(const std::string& url);
    void connect();
    void disconnect();
    void send_ping();
    void send_subscribe_messages();
    void send_ohlcv_subscribe_messages();
    void send_lob_subscribe_messages();
    void send_trade_subscribe_messages();
    void send_book_ticker_subscribe_messages();
    bool is_disconnected();
    std::chrono::steady_clock::time_point get_last_msg_received();
    // Hold the staleness clock while a venue is legitimately silent (e.g. US
    // equities outside extended hours) so the watchdog does not disconnect a
    // healthy stream, and so its grace window restarts when the venue reopens.
    void hold_staleness_clock();
    long long get_msg_count() const;
    bool send_message(const std::string& message);

   protected:
    LogCallback info_log_;
    LogCallback error_log_;

   private:
    void run();
    void setup_websocket();
    void setup_tls();
    bool is_wss() const;
};
