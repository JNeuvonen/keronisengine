#include "live_infra/account_stream_manager.h"

#include "common_types/log_event_types.h"
#include "datetime_utils.h"
#include "disk_logging/log.h"
#include "market_making/engine.h"
#include "market_making/rate_limited_emitter.h"
#include "ws_stream.h"

// Hot-path guard for WsStream info/error callbacks. The ws library fires
// these per-message in the consumer thread; a misbehaving venue can push a
// kHz stream of error lines that would back-pressure the thread if every
// line sync-wrote disk. We route through Engine::create_system_log (lock-
// free queue) and rate-limit to one emit per 10s per (level, message-
// prefix) key. Mirrors the emit_trade_parse_error pattern in
// trade_streams.cpp + emit_parse_error in market_data_streams.cpp.
namespace {
Core::RateLimitedEmitter g_account_stream_info_emitter;
Core::RateLimitedEmitter g_account_stream_error_emitter;

// Prefix-keyed throttle — groups similar messages (e.g. retry counters that
// change every tick) into one emit stream while keeping distinct error
// categories separate. 40-char prefix is long enough to distinguish
// "connect timeout" from "auth failed" without leaking a new key per
// parameterized retry count in the tail of the message.
std::string prefix_key(const std::string& msg) {
    return msg.substr(0, std::min<size_t>(40, msg.size()));
}

void emit_account_stream_info(const std::string& msg) {
    g_account_stream_info_emitter.emit_if_allowed(
        prefix_key(msg), 10000, [&](uint64_t suppressed) {
            nlohmann::json md = {
                {"events_suppressed_since_last_emit", suppressed}};
            Core::Engine::getInstance().create_system_log(
                "", Core::LOG_LEVEL_INFO, std::string("[AccountStream] ") + msg,
                Core::TRADING_LOG_EVENT_TYPE_PRIVATE_STREAM_MSG, md, -1);
        });
}

void emit_account_stream_error(const std::string& msg) {
    g_account_stream_error_emitter.emit_if_allowed(
        prefix_key(msg), 10000, [&](uint64_t suppressed) {
            nlohmann::json md = {
                {"events_suppressed_since_last_emit", suppressed}};
            Core::Engine::getInstance().create_system_log(
                "", Core::LOG_LEVEL_ERROR, std::string("[AccountStream] ") + msg,
                Core::TRADING_LOG_EVENT_TYPE_PRIVATE_STREAM_PARSE_ERROR, md, -1);
        });
}
}  // namespace

namespace Live {

void AccountStreamManager::add_account(AccountStreamConfig config) {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    ManagedStream ms;
    ms.config = std::move(config);
    ms.last_key_refresh = std::chrono::steady_clock::now();
    streams_.push_back(std::move(ms));
}

void AccountStreamManager::set_order_update_callback(
    std::function<void(const Core::OrderResNormalized&)> callback) {
    order_update_callback_ = std::move(callback);
}

void AccountStreamManager::start() {
    // Idempotent: each venue's init_and_register_account_streams() calls this.
    // The first call connects its streams + spawns the refresh thread; a later
    // call connects only newly-registered (not-yet-connected) streams.
    const bool was_running = running_.exchange(true);

    for (size_t i = 0; i < streams_.size(); i++) {
        auto& stream = streams_[i];
        if (stream.ws) continue;  // already connected by a prior start()
        try {
            connect_stream(stream);
            LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_PRIVATE_STREAM_CONNECTED,
                     "[AccountStream] Connected: " +
                         stream.config.exchange_name + ":" +
                         stream.config.market_type +
                         " account=" + std::to_string(stream.config.account_id),
                     (nlohmann::json{{"exchange_name", stream.config.exchange_name},
                                     {"market_type", stream.config.market_type},
                                     {"account_id", stream.config.account_id}}));
        } catch (const std::exception& e) {
            LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_PRIVATE_STREAM_AUTH_FAILED,
                      "[AccountStream] Failed to connect " +
                          stream.config.exchange_name + ": " + e.what(),
                      (nlohmann::json{{"exchange_name", stream.config.exchange_name},
                                      {"market_type", stream.config.market_type},
                                      {"account_id", stream.config.account_id},
                                      {"what", e.what()}}));
        }
    }

    if (!was_running && !streams_.empty()) {
        key_refresh_thread_ =
            std::thread([this]() { this->key_refresh_loop(); });
    }
}

void AccountStreamManager::connect_stream(ManagedStream& stream) {
    // Listen-key venues fetch a key; subscription-auth venues (Hyperliquid)
    // leave fetch_stream_key unset and authenticate via on_connect_messages.
    if (stream.config.fetch_stream_key && stream.current_key.empty()) {
        stream.current_key = stream.config.fetch_stream_key();
        stream.last_key_refresh = std::chrono::steady_clock::now();
    }

    std::string url = stream.config.build_ws_url(stream.current_key);

    // Find this stream's index for the message callback
    size_t idx = 0;
    for (size_t i = 0; i < streams_.size(); i++) {
        if (&streams_[i] == &stream) {
            idx = i;
            break;
        }
    }

    stream.ws = std::make_unique<WsStream>(
        url,
        [this, idx](const std::string& msg) { this->on_message(idx, msg); },
        "",
        [](const std::string& msg) { emit_account_stream_info(msg); },
        [](const std::string& msg) { emit_account_stream_error(msg); });

    if (stream.config.on_connect_messages) {
        stream.ws->set_on_open_messages(stream.config.on_connect_messages());
    }

    stream.ws->connect();
}

void AccountStreamManager::on_message(size_t stream_idx,
                                       const std::string& msg) {
    std::vector<Core::OrderResNormalized> results;
    std::function<void(const Core::OrderResNormalized&)> callback;

    {
        std::lock_guard<std::mutex> lock(streams_mutex_);
        if (stream_idx >= streams_.size()) return;

        auto& stream = streams_[stream_idx];
        stream.last_message_at = std::chrono::steady_clock::now();
        stream.stale_already_emitted = false;
        results = stream.config.parse_order_update(msg);
        callback = order_update_callback_;
    }

    if (!callback) return;
    for (const auto& result : results) {
        callback(result);
    }
}

void AccountStreamManager::reconnect_disconnected() {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    for (auto& stream : streams_) {
        const bool is_disconnected =
            !stream.ws || stream.ws->is_disconnected();
        if (!is_disconnected) continue;

        // First loop iteration noticing this disconnect: capture the moment
        // and a reason, then emit PRIVATE_STREAM_DISCONNECTED so the reconnect
        // burst is traceable.
        if (!stream.disconnected_since.has_value()) {
            stream.disconnected_since = std::chrono::steady_clock::now();
            if (stream.disconnect_reason.empty()) {
                stream.disconnect_reason =
                    PrivateStreamDisconnectReason::UNKNOWN;
            }
            Core::Engine::getInstance().create_system_log(
                "", Core::LOG_LEVEL_WARNING, "private stream disconnected",
                Core::TRADING_LOG_EVENT_TYPE_PRIVATE_STREAM_DISCONNECTED,
                nlohmann::json{
                    {"exchange_name", stream.config.exchange_name},
                    {"market_type", stream.config.market_type},
                    {"account_id", stream.config.account_id},
                    {"reason", stream.disconnect_reason}},
                -1);
        }

        const auto now = std::chrono::steady_clock::now();
        if (stream.last_reconnect_attempt.has_value() &&
            now - *stream.last_reconnect_attempt < std::chrono::seconds(5)) {
            continue;
        }
        stream.last_reconnect_attempt = now;

        try {
            // Key may have expired — fetch a fresh one
            stream.current_key.clear();
            connect_stream(stream);
            const long long downtime_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() -
                    stream.disconnected_since.value())
                    .count();
            Core::Engine::getInstance().create_system_log(
                "", Core::LOG_LEVEL_INFO, "private stream reconnected",
                Core::TRADING_LOG_EVENT_TYPE_PRIVATE_STREAM_CONNECTED,
                nlohmann::json{
                    {"exchange_name", stream.config.exchange_name},
                    {"market_type", stream.config.market_type},
                    {"account_id", stream.config.account_id},
                    {"trigger", "disconnected_reconnect"},
                    {"reason_for_disconnect", stream.disconnect_reason},
                    {"downtime_ms", downtime_ms}},
                -1);
            stream.disconnected_since.reset();
            stream.disconnect_reason.clear();
            stream.last_message_at = std::chrono::steady_clock::now();
            stream.stale_already_emitted = false;
            stream.last_reconnect_attempt.reset();
            stream.outage_error_emitted = false;
        } catch (const std::exception& e) {
            const nlohmann::json md{
                {"exchange_name", stream.config.exchange_name},
                {"market_type", stream.config.market_type},
                {"account_id", stream.config.account_id},
                {"trigger", "disconnected_reconnect"},
                {"reason", stream.disconnect_reason},
                {"what", e.what()}};
            const std::string msg = "[AccountStream] Reconnect failed: " +
                                    stream.config.exchange_name + ": " +
                                    e.what();
            // One ERROR per outage, once it has lasted two minutes.
            if (!stream.outage_error_emitted &&
                now - stream.disconnected_since.value() >
                    std::chrono::minutes(2)) {
                stream.outage_error_emitted = true;
                LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_PRIVATE_STREAM_AUTH_FAILED,
                          msg, md);
            } else {
                LOG_WARNING(
                    Core::TRADING_LOG_EVENT_TYPE_PRIVATE_STREAM_AUTH_FAILED, msg,
                    md);
            }
        }
    }
}

void AccountStreamManager::check_staleness() {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(streams_mutex_);
    for (auto& stream : streams_) {
        if (!stream.ws || stream.ws->is_disconnected()) continue;
        if (stream.stale_already_emitted) continue;
        const long long silent_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - stream.last_message_at)
                .count();
        if (silent_ms < PRIVATE_STREAM_STALE_THRESHOLD_MS) continue;

        stream.stale_already_emitted = true;
        // Tag the expected upcoming disconnect with STALENESS_WATCHDOG so if
        // the stream goes actually-disconnected next (which is common when
        // the WS is zombied), the DISCONNECTED emit carries the right reason.
        stream.disconnect_reason =
            PrivateStreamDisconnectReason::STALENESS_WATCHDOG;
        Core::Engine::getInstance().create_system_log(
            "", Core::LOG_LEVEL_WARNING, "private stream stale",
            Core::TRADING_LOG_EVENT_TYPE_PRIVATE_STREAM_STALE,
            nlohmann::json{
                {"exchange_name", stream.config.exchange_name},
                {"market_type", stream.config.market_type},
                {"account_id", stream.config.account_id},
                {"silent_ms", silent_ms},
                {"stale_threshold_ms", PRIVATE_STREAM_STALE_THRESHOLD_MS}},
            -1);
    }
}

std::vector<MarketData::StreamStatus> AccountStreamManager::get_stream_statuses() {
    const auto now = std::chrono::steady_clock::now();
    const long long now_ms = DatetimeUtils::get_current_time_ms();
    auto ms_since = [&](std::chrono::steady_clock::time_point then) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(now - then)
            .count();
    };

    std::lock_guard<std::mutex> lock(streams_mutex_);
    std::vector<MarketData::StreamStatus> statuses;
    statuses.reserve(streams_.size());
    for (const auto& stream : streams_) {
        MarketData::StreamStatus s;
        s.key = "private:" + stream.config.exchange_name + ":" +
                stream.config.market_type + ":" +
                std::to_string(stream.config.account_id);
        s.is_private = true;
        s.stream_type = "account";
        s.exchange_name = stream.config.exchange_name;
        s.market_type = stream.config.market_type;
        s.account_id = stream.config.account_id;
        if (stream.ws) {
            s.host = MarketData::StreamStatusUtils::url_host(stream.ws->wsurl_);
            s.msg_count = stream.ws->get_msg_count();
            // Atomics, not is_disconnected(): that takes ws_mutex, which
            // connect() holds through a thread join, under streams_mutex_.
            s.is_connected = stream.ws->is_initialized_.load() &&
                             !stream.ws->is_disconnected_.load();
        }
        s.last_msg_age_ms = ms_since(stream.last_message_at);
        s.is_stale = s.is_connected &&
                     s.last_msg_age_ms >= PRIVATE_STREAM_STALE_THRESHOLD_MS;
        s.sampled_at_ms = now_ms;
        s.disconnect_reason = stream.disconnect_reason;
        if (stream.disconnected_since.has_value()) {
            s.disconnected_for_ms = ms_since(*stream.disconnected_since);
        }
        statuses.push_back(std::move(s));
    }
    return statuses;
}

void AccountStreamManager::key_refresh_loop() {
    struct RefreshWork {
        size_t stream_idx;
        std::string current_key;
        std::function<void(const std::string&)> renew_stream_key;
    };

    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(60));

        std::vector<RefreshWork> refresh_work;
        {
            const auto now = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> lock(streams_mutex_);
            refresh_work.reserve(streams_.size());
            for (size_t i = 0; i < streams_.size(); ++i) {
                const auto& stream = streams_[i];
                const auto elapsed =
                    std::chrono::duration_cast<std::chrono::seconds>(
                        now - stream.last_key_refresh)
                        .count();

                if (stream.config.renew_stream_key &&
                    elapsed >= stream.config.key_refresh_interval_s) {
                    refresh_work.push_back(RefreshWork{
                        i,
                        stream.current_key,
                        stream.config.renew_stream_key,
                    });
                }
            }
        }

        for (const auto& work : refresh_work) {
            try {
                work.renew_stream_key(work.current_key);

                std::lock_guard<std::mutex> lock(streams_mutex_);
                if (work.stream_idx >= streams_.size()) continue;
                streams_[work.stream_idx].last_key_refresh =
                    std::chrono::steady_clock::now();
            } catch (const std::exception& e) {
                std::string exchange_name;
                std::string market_type;
                long long account_id = 0;
                std::optional<ManagedStream> reconnect_stream;
                {
                    std::lock_guard<std::mutex> lock(streams_mutex_);
                    if (work.stream_idx >= streams_.size()) continue;
                    auto& stream = streams_[work.stream_idx];
                    stream.disconnect_reason =
                        PrivateStreamDisconnectReason::KEY_EXPIRED;
                    exchange_name = stream.config.exchange_name;
                    market_type = stream.config.market_type;
                    account_id = stream.config.account_id;
                    stream.current_key.clear();
                    reconnect_stream.emplace();
                    reconnect_stream->config = stream.config;
                    reconnect_stream->current_key = stream.current_key;
                    reconnect_stream->last_key_refresh = stream.last_key_refresh;
                    reconnect_stream->last_message_at = stream.last_message_at;
                    reconnect_stream->disconnected_since =
                        stream.disconnected_since;
                    reconnect_stream->disconnect_reason =
                        stream.disconnect_reason;
                    reconnect_stream->stale_already_emitted =
                        stream.stale_already_emitted;
                }

                LOG_ERROR(
                    Core::TRADING_LOG_EVENT_TYPE_PRIVATE_STREAM_AUTH_FAILED,
                    "[AccountStream] Key refresh failed for " + exchange_name +
                        ", fetching new key: " + e.what(),
                    (nlohmann::json{{"exchange_name", exchange_name},
                                    {"market_type", market_type},
                                    {"account_id", account_id},
                                    {"trigger", "key_refresh"},
                                    {"what", e.what()}}));
                try {
                    if (!reconnect_stream.has_value()) continue;
                    connect_stream(reconnect_stream.value());

                    std::lock_guard<std::mutex> lock(streams_mutex_);
                    if (work.stream_idx >= streams_.size()) continue;
                    auto& stream = streams_[work.stream_idx];
                    stream.current_key = reconnect_stream->current_key;
                    stream.ws = std::move(reconnect_stream->ws);
                    stream.last_key_refresh = std::chrono::steady_clock::now();
                    stream.disconnect_reason.clear();
                    stream.last_message_at = std::chrono::steady_clock::now();
                    stream.stale_already_emitted = false;
                } catch (const std::exception& e2) {
                    LOG_ERROR(
                        Core::TRADING_LOG_EVENT_TYPE_PRIVATE_STREAM_AUTH_FAILED,
                        "[AccountStream] Reconnect after key refresh failure "
                        "failed: " + std::string(e2.what()),
                        (nlohmann::json{{"exchange_name", exchange_name},
                                        {"market_type", market_type},
                                        {"account_id", account_id},
                                        {"trigger", "post_key_refresh_reconnect"},
                                        {"what", e2.what()}}));
                }
            }
        }
    }
}

}  // namespace Live
