#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "market_data/stream_status.h"
#include "market_making/dtypes/order_res.h"

class WsStream;

namespace Live {

// Reason codes for PRIVATE_STREAM_DISCONNECTED.metadata.reason. Closed set —
// agents query by these to distinguish network blips from staleness from key
// expiry. `UNKNOWN` is the fallback when we notice `is_disconnected()` went
// true without having attributed the cause.
namespace PrivateStreamDisconnectReason {
constexpr const char* NETWORK_TIMEOUT = "NETWORK_TIMEOUT";
constexpr const char* STALENESS_WATCHDOG = "STALENESS_WATCHDOG";
constexpr const char* KEY_EXPIRED = "KEY_EXPIRED";
constexpr const char* VENUE_SENT_DISCONNECT = "VENUE_SENT_DISCONNECT";
constexpr const char* UNKNOWN = "UNKNOWN";
}  // namespace PrivateStreamDisconnectReason

// Stale if no message received for this many milliseconds while the stream
// reports connected. 10s matches the roadmap — long enough to tolerate venue
// quiet periods during low-volume windows, short enough to catch zombied
// sockets before orders back up.
constexpr long long PRIVATE_STREAM_STALE_THRESHOLD_MS = 10000;

struct AccountStreamConfig {
    std::string exchange_name;
    std::string market_type;
    long long account_id;

    // Stream key lifecycle — exchange-specific, provided at registration.
    // Optional: venues with subscription-auth WS (e.g. Hyperliquid) leave
    // these unset and supply on_connect_messages instead.
    std::function<std::string()> fetch_stream_key;
    std::function<void(const std::string&)> renew_stream_key;
    int key_refresh_interval_s = 25 * 60;  // default 25 min

    // URL construction from key (subscription-auth venues ignore the key)
    std::function<std::string(const std::string& key)> build_ws_url;

    // Messages sent on each (re)connect — subscription-auth venues use this to
    // subscribe to their user/order streams. Empty for listen-key venues.
    std::function<std::vector<std::string>()> on_connect_messages;

    // Message parsing — returns the order updates carried by this message
    // (empty for non-order messages; venues like Hyperliquid batch many
    // updates per message, so this returns a vector rather than a single).
    std::function<std::vector<Core::OrderResNormalized>(const std::string&)>
        parse_order_update;
};

class AccountStreamManager {
   public:
    static AccountStreamManager& getInstance() {
        static AccountStreamManager instance;
        return instance;
    }
    AccountStreamManager(const AccountStreamManager&) = delete;
    AccountStreamManager& operator=(const AccountStreamManager&) = delete;

    ~AccountStreamManager() {
        running_ = false;
        if (key_refresh_thread_.joinable()) {
            key_refresh_thread_.join();
        }
    }

    void add_account(AccountStreamConfig config);
    void start();

    void set_order_update_callback(
        std::function<void(const Core::OrderResNormalized&)> callback);

    // Called from StateUpdater::reconnect_streams()
    void reconnect_disconnected();

    // Called from the main loop to emit PRIVATE_STREAM_STALE when a stream
    // has been silent for too long. A stale stream is technically connected
    // but no longer delivering — the watchdog surfaces this so agents can
    // distinguish "venue is quiet" from "our connection is zombie."
    // Emits at most once per stream per stale window; resets on next message.
    void check_staleness();

    std::vector<MarketData::StreamStatus> get_stream_statuses();

   private:
    AccountStreamManager() = default;

    struct ManagedStream {
        AccountStreamConfig config;
        std::string current_key;
        std::unique_ptr<WsStream> ws;
        std::chrono::steady_clock::time_point last_key_refresh;
        // Tracks last inbound message; staleness check flags a stream that
        // hasn't received anything for STALE_THRESHOLD_MS while still
        // reporting connected. Initialized to construction time so a
        // never-received stream eventually stales instead of looking fresh.
        std::chrono::steady_clock::time_point last_message_at{
            std::chrono::steady_clock::now()};
        // When we detect a disconnect, capture the timestamp + reason. Cleared
        // on successful reconnect (after emitting CONNECTED with downtime_ms).
        std::optional<std::chrono::steady_clock::time_point> disconnected_since;
        std::string disconnect_reason;  // one of PrivateStreamDisconnectReason::*
        // Suppresses repeated STALE emits for the same stale window; reset on
        // message receipt.
        bool stale_already_emitted{false};
    };

    std::vector<ManagedStream> streams_;
    std::mutex streams_mutex_;
    std::function<void(const Core::OrderResNormalized&)> order_update_callback_;
    std::atomic<bool> running_{false};
    std::thread key_refresh_thread_;

    void connect_stream(ManagedStream& stream);
    void on_message(size_t stream_idx, const std::string& msg);
    void key_refresh_loop();
};

}  // namespace Live
