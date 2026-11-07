// src/cpp/packages/common/include/nats_publisher.h
#pragma once

#include <nats/nats.h>

#include <atomic>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>

#include "common_types/trading_log.h"

class NatsPublisher {
   public:
    static NatsPublisher& getInstance();

    NatsPublisher(const NatsPublisher&) = delete;
    NatsPublisher& operator=(const NatsPublisher&) = delete;

    bool connect(const std::string& url, const std::string& token = "");
    void disconnect();
    bool is_connected() const;

    bool publish(const std::string& subject, const std::string& message);
    bool publish_trading_logs(const std::vector<TradingLog>& logs);
    bool publish_trading_log(const TradingLog& log);
    bool publish_logs(const std::vector<TradingLog>& logs);

    // When disabled, all publish*() calls return true with zero I/O. Used
    // when ENABLE_NATS_PUBLISHER=0 — caller treats publish as a no-op
    // instead of paying the rate-limited "not_connected" error path.
    void set_disabled(bool disabled);

   private:
    NatsPublisher();
    ~NatsPublisher();

    natsConnection* conn_ = nullptr;
    natsOptions* opts_ = nullptr;
    std::mutex conn_mutex_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> disabled_{false};
    std::string url_;

    static void disconnected_cb(natsConnection* nc, void* closure);
    static void reconnected_cb(natsConnection* nc, void* closure);
    static void error_cb(natsConnection* nc, natsSubscription* sub,
                         natsStatus err, void* closure);

    // Rate-limited emit path for NATS_PUBLISH_FAILED. Called from publish()
    // on both the not-connected-yet and venue-returned-error branches.
    void emit_publish_failure(const std::string& subject, size_t payload_bytes,
                              const std::string& what);
};