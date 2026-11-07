#pragma once

#include <chrono>
#include <functional>
#include <string>

#include "httplib.h"
#include "server_common/httplib_server_base.h"

namespace EngineControl {

// HTTP server that exposes Core::Engine for tests + diagnostics. Off by
// default — opt in via ENABLE_ENGINE_CONTROL_SERVER=1.
//
// Phase A (today) ships read-only endpoints under /control/* — see
// read_handlers.h.
//
// Phase B mutating + trigger endpoints (write_handlers.h / trigger_handlers.h)
// are gated separately by ENABLE_ENGINE_CONTROL_MUTATIONS=1. The
// register_mutating() helper inspects the flag once at registration time and
// either wires the real handler or a 403 stub — no per-request branching.
//
// All handlers hit Core::Engine::getInstance() directly (singleton, lives at
// least as long as this server).
class Server : public ServerCommon::HttplibServerBase {
   public:
    Server(std::string bind_address, int port, bool mutations_enabled,
           std::chrono::steady_clock::time_point start_time);

   private:
    void register_handlers();
    // Routes a POST through mutations gating. When mutations_enabled_ is
    // false, registers a 403 stub instead of `real`.
    void register_mutating(
        const std::string& path,
        std::function<void(const httplib::Request&, httplib::Response&)> real);

    bool mutations_enabled_;
    std::chrono::steady_clock::time_point start_time_;
};

}  // namespace EngineControl
