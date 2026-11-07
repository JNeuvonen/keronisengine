// src/cpp/packages/common/src/nats_publisher.cpp
#include "nats_publisher.h"

#include <atomic>
#include <chrono>

#include "common_types/log_event_types.h"
#include "disk_logging/log.h"

namespace {
// NATS publish fires once per emitted log row + once per strategy telemetry
// tick + once per realtime-state broadcast — order 100s/sec under normal load.
// When the server is flapping, every publish fails. An unthrottled LOG_ERROR
// at that rate would flood the rotating log. 10s cooldown with a suppressed
// counter is the RateLimitedEmitter pattern, rebuilt inline here because
// `common/` can't depend on `trading_engine/` (where RateLimitedEmitter lives).
std::atomic<int64_t> g_nats_publish_fail_last_emit_ms{0};
std::atomic<uint64_t> g_nats_publish_fail_suppressed{0};
constexpr int64_t NATS_PUBLISH_FAIL_COOLDOWN_MS = 10000;

int64_t now_epoch_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
}  // namespace

NatsPublisher& NatsPublisher::getInstance() {
    static NatsPublisher instance;
    return instance;
}

NatsPublisher::NatsPublisher() = default;

NatsPublisher::~NatsPublisher() { disconnect(); }

bool NatsPublisher::connect(const std::string& url, const std::string& token) {
    std::lock_guard<std::mutex> lock(conn_mutex_);

    if (connected_) {
        return true;
    }

    url_ = url;
    natsStatus s = natsOptions_Create(&opts_);
    if (s != NATS_OK) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_NATS_CONNECT_FAILED,
                  "Failed to create NATS options",
                  (nlohmann::json{{"nats_status", natsStatus_GetText(s)}}));
        return false;
    }

    natsOptions_SetURL(opts_, url.c_str());
    if (!token.empty()) {
        natsOptions_SetToken(opts_, token.c_str());
    }
    natsOptions_SetMaxReconnect(opts_, -1);
    natsOptions_SetReconnectWait(opts_, 2000);
    natsOptions_SetDisconnectedCB(opts_, disconnected_cb, this);
    natsOptions_SetReconnectedCB(opts_, reconnected_cb, this);
    natsOptions_SetErrorHandler(opts_, error_cb, this);

    s = natsConnection_Connect(&conn_, opts_);
    if (s != NATS_OK) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_NATS_CONNECT_FAILED,
                  "Failed to connect to NATS: " +
                      std::string(natsStatus_GetText(s)),
                  (nlohmann::json{{"nats_url", url},
                                  {"nats_status", natsStatus_GetText(s)}}));
        natsOptions_Destroy(opts_);
        opts_ = nullptr;
        return false;
    }

    connected_ = true;
    LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_NATS_CONNECTED,
             "Connected to NATS at " + url,
             (nlohmann::json{{"nats_url", url}}));
    return true;
}

void NatsPublisher::disconnect() {
    std::lock_guard<std::mutex> lock(conn_mutex_);

    if (conn_) {
        natsConnection_Destroy(conn_);
        conn_ = nullptr;
    }
    if (opts_) {
        natsOptions_Destroy(opts_);
        opts_ = nullptr;
    }
    connected_ = false;
}

bool NatsPublisher::is_connected() const { return connected_.load(); }

void NatsPublisher::set_disabled(bool disabled) {
    disabled_.store(disabled, std::memory_order_relaxed);
}

bool NatsPublisher::publish(const std::string& subject,
                            const std::string& message) {
    if (disabled_.load(std::memory_order_relaxed)) {
        // ENABLE_NATS_PUBLISHER=0 — silent no-op, no error spam.
        return true;
    }
    if (!connected_ || !conn_) {
        emit_publish_failure(subject, message.size(), "not_connected");
        return false;
    }

    natsStatus s =
        natsConnection_PublishString(conn_, subject.c_str(), message.c_str());
    if (s != NATS_OK) {
        emit_publish_failure(subject, message.size(), natsStatus_GetText(s));
        return false;
    }
    return true;
}

void NatsPublisher::emit_publish_failure(const std::string& subject,
                                         size_t payload_bytes,
                                         const std::string& what) {
    const int64_t now_ms = now_epoch_ms();
    const int64_t last_ms = g_nats_publish_fail_last_emit_ms.load(
        std::memory_order_acquire);
    if (now_ms - last_ms < NATS_PUBLISH_FAIL_COOLDOWN_MS) {
        g_nats_publish_fail_suppressed.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // CAS guards against racy double-emits; loser increments suppressed.
    int64_t expected = last_ms;
    if (!g_nats_publish_fail_last_emit_ms.compare_exchange_strong(
            expected, now_ms, std::memory_order_acq_rel)) {
        g_nats_publish_fail_suppressed.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint64_t suppressed = g_nats_publish_fail_suppressed.exchange(
        0, std::memory_order_acq_rel);
    LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_NATS_PUBLISH_FAILED,
              "nats publish failed",
              (nlohmann::json{
                  {"subject", subject},
                  {"payload_bytes", payload_bytes},
                  {"what", what},
                  {"events_suppressed_since_last_emit", suppressed}}));
}

bool NatsPublisher::publish_trading_logs(const std::vector<TradingLog>& logs) {
    if (logs.empty()) {
        return true;
    }

    nlohmann::json logs_array = nlohmann::json::array();
    for (const auto& log : logs) {
        logs_array.push_back(log.to_json());
    }

    return publish("trading_activity.logs", logs_array.dump());
}

void NatsPublisher::disconnected_cb(natsConnection* nc, void* closure) {
    auto* self = static_cast<NatsPublisher*>(closure);
    self->connected_ = false;
    LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_NATS_CONNECTION_LOST,
              "Disconnected from NATS",
              (nlohmann::json{{"nats_url", self->url_}}));
}

void NatsPublisher::reconnected_cb(natsConnection* nc, void* closure) {
    auto* self = static_cast<NatsPublisher*>(closure);
    self->connected_ = true;
    LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_NATS_RECONNECTED,
             "Reconnected to NATS",
             (nlohmann::json{{"nats_url", self->url_}}));
}

void NatsPublisher::error_cb(natsConnection* nc, natsSubscription* sub,
                             natsStatus err, void* closure) {
    auto* self = static_cast<NatsPublisher*>(closure);
    LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_NATS_ERROR,
              "NATS error: " + std::string(natsStatus_GetText(err)),
              (nlohmann::json{{"nats_url", self->url_},
                              {"nats_status", natsStatus_GetText(err)}}));
}

bool NatsPublisher::publish_trading_log(const TradingLog& log) {
    return publish("trading_activity.logs", log.to_json().dump());
}

bool NatsPublisher::publish_logs(const std::vector<TradingLog>& logs) {
    if (logs.empty()) {
        return true;
    }

    nlohmann::json logs_array = nlohmann::json::array();
    for (const auto& log : logs) {
        logs_array.push_back(log.to_json());
    }

    return publish("logs", logs_array.dump());
}