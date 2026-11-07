// External-sink storage + forwarding for the disk_logging macros.
// Declared in common/include/disk_logging/log.h; definition lives here so
// every TU that includes the header shares the same callback state.
//
// Thread-safety: registration is a one-shot at process startup (live_infra)
// and a one-shot clear during graceful shutdown, both on the main thread.
// Routing is called from arbitrary emit threads. An atomic<bool> guards
// reads-after-clear; the callback itself is protected by copy-at-init
// (std::function is reassignment-atomic only under a mutex, but we rely on
// the startup/shutdown pattern: no routing calls run while set/clear is in
// flight because the engine hasn't started / has shut down).
#include "disk_logging/log.h"

#include <atomic>

namespace DiskLogging {

namespace {
ExternalSink g_sink;
std::atomic<bool> g_sink_live{false};
}  // namespace

void set_external_sink(ExternalSink cb) {
    g_sink = std::move(cb);
    g_sink_live.store(true, std::memory_order_release);
}

void clear_external_sink() {
    g_sink_live.store(false, std::memory_order_release);
    g_sink = nullptr;
}

// Re-entry guard. If the sink callback itself emits a LOG_* (directly or via
// code it calls — e.g. a DB write that fails inside the engine's log-flush
// callback and tries to LOG_ERROR the failure), the second invocation would
// enqueue another log, which the next flush would try to write, which could
// fail and re-enqueue... unbounded queue growth. The guard short-circuits
// re-entry on the same thread and silently drops the inner emit.
//
// The dropped emit still reaches the rotating disk file (route_to_external_sink
// is called AFTER the disk write in the macro). Disk is the safety net for
// "couldn't write log row to DB" — grep the rotating file.
namespace {
thread_local bool t_routing_in_flight = false;

// RAII so the flag clears even if a future change re-throws from the sink
// callback or inserts a return mid-function. Correctness doesn't depend on
// the current catch(...) being perfect.
struct ScopedRoutingFlag {
    ScopedRoutingFlag() { t_routing_in_flight = true; }
    ~ScopedRoutingFlag() { t_routing_in_flight = false; }
};
}  // namespace

void route_to_external_sink(const char* level, const char* event_type,
                             const std::string& msg,
                             const nlohmann::json& metadata) {
    if (!g_sink_live.load(std::memory_order_acquire)) return;
    if (!g_sink) return;
    if (t_routing_in_flight) return;
    ScopedRoutingFlag guard;
    try {
        g_sink(level, event_type, msg, metadata);
    } catch (...) {
        // Never let a broken sink take down a log-emit path. Silently drop.
    }
}

}  // namespace DiskLogging
