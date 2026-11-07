#pragma once

#include <atomic>
#include <string>
#include <thread>
#include <utility>

#include "httplib.h"

namespace ServerCommon {

// Common lifecycle for our cpp-httplib-based HTTP/WS servers
// (simulator_control_server, AsterHttp::Server, AsterHttp::WsServer,
// engine_control_server). Owns the listener thread + the httplib::Server
// instance. Subclasses register routes against `server_` from their own
// constructor.
//
// Thread is **joinable**, not detached — Step 1.6 lesson: detached
// threads racing during teardown caused SIGSEGV under stress when many
// Servers are constructed/destructed in tests. Joinable + stop()
// blocking on join() is the safer pattern. EngineControl::Server's
// previous detached pattern silently inherited the same risk in
// production live_infra; migrating to this base class fixes it.
class HttplibServerBase {
   public:
    HttplibServerBase(std::string bind_address, int port)
        : bind_address_(std::move(bind_address)), port_(port) {}

    virtual ~HttplibServerBase() {
        // Defensive: if a subclass forgets to stop() before destruction,
        // we still tear down cleanly. server_.stop() is a no-op if not
        // listening; thread join is a no-op if not joinable.
        server_.stop();
        if (thread_.joinable()) thread_.join();
    }

    HttplibServerBase(const HttplibServerBase&) = delete;
    HttplibServerBase& operator=(const HttplibServerBase&) = delete;
    HttplibServerBase(HttplibServerBase&&) = delete;
    HttplibServerBase& operator=(HttplibServerBase&&) = delete;

    void start() {
        listening_.store(true);
        thread_ = std::thread([this]() {
            server_.listen(bind_address_.c_str(), port_);
            listening_.store(false);
        });
    }

    void stop() {
        server_.stop();
        if (thread_.joinable()) thread_.join();
    }

   protected:
    httplib::Server server_;

   private:
    std::string bind_address_;
    int port_;
    std::thread thread_;
    std::atomic<bool> listening_{false};
};

}  // namespace ServerCommon
