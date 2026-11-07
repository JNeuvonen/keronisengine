#pragma once

#include <functional>
#include <nlohmann/json.hpp>
#include <string>

// =============================================================================
// Unified logging sink — Phase 3 of the engine logging overhaul.
//
// LOG_INFO / LOG_WARNING / LOG_ERROR are synchronous disk + sink writes. They
// belong OUTSIDE hot paths — startup, stream lifecycle, error recovery, DB
// write failures, NATS connect failures, etc. Low-frequency operational
// signal.
//
// HOT-PATH LOGGING (per-tick, per-event, per-message consumer threads) goes
// through `Engine::create_system_log()` instead — that path enqueues into a
// lock-free `moodycamel::ConcurrentQueue` and the periodic flush thread
// drains it in batches. For high-frequency events there, use
// `RateLimitedEmitter` (one emit per cooldown per key with suppressed-count)
// or sample every N-th call. Never do synchronous disk I/O on a tick thread.
//
// Two sinks, one call:
//   1. Rotating disk file — always on when DISK_LOGGING_ENABLED defined.
//      Local, per-process, rotates at 500 KiB. Human-scrollable debug.
//   2. External sink → structured row in trading_activity_log via the
//      engine. Opt-in per binary: set_external_sink() is called by
//      live_infra at startup. Offline tools (backtest_runner,
//      market_data_recorder) leave it null — no DB write, disk only.
//
// The macros take `event_type` as the first parameter — one of the closed-
// set constants from common_types/log_event_types.h. Agents query
// trading_activity_log by event_type; free-form strings break that.
//
// DEPRECATED 2-arg form exists for migration only. Existing
// `LOG_INFO(msg, json)` / `LOG_ERROR(msg, json)` call sites still compile
// (disk write only, no sink routing) so migration can proceed per-package.
// The 2-arg form is flagged by codebase_health and deleted once the last
// untyped call site migrates.
// =============================================================================

namespace DiskLogging {

// Callback signature for the external sink. Called after the disk write for
// every typed LOG_* invocation, when a sink is registered.
using ExternalSink =
    std::function<void(const char* level, const char* event_type,
                        const std::string& msg, const nlohmann::json& metadata)>;

// Register / clear the sink. Idempotent. Thread-safe wrt. the macro's
// fast-path (atomic load in the macro). Called ONCE at process startup by
// live_infra/main.cpp, and ONCE on graceful shutdown before engine teardown.
void set_external_sink(ExternalSink cb);
void clear_external_sink();

// Internal — invoked by the typed macros after the disk write. Silently
// skips when no sink is registered.
void route_to_external_sink(const char* level, const char* event_type,
                             const std::string& msg,
                             const nlohmann::json& metadata);

}  // namespace DiskLogging

#ifdef DISK_LOGGING_ENABLED

#include <chrono>
#include <iomanip>
#include <sstream>

#include "rotating_log.h"

inline RotatingLog& disk_logger() {
    static RotatingLog instance(0.5 * 1024 * 1024);  // 500 KiB
    return instance;
}

inline std::string current_utc_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto now_c = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
#if defined(_MSC_VER)
    gmtime_s(&tm_buf, &now_c);
#else
    gmtime_r(&now_c, &tm_buf);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

inline std::string pretty_or_blank(const nlohmann::json& j) {
    return (j.is_null() || j.empty()) ? "" : ("\n\n" + j.dump(4) + "\n\n");
}

// -----------------------------------------------------------------------------
// Typed macros — the only form. event_type must come from
// common_types/log_event_types.h. 2-arg calls are a compile error — the
// legacy dispatcher was retired at the end of Phase 3.
// -----------------------------------------------------------------------------
#define LOG_INFO(event_type, msg_, json_)                                     \
    do {                                                                      \
        const std::string _log_msg = (msg_);                                  \
        const nlohmann::json _log_md = (json_);                               \
        disk_logger().log("[" + current_utc_timestamp() + " UTC] [INFO]  " + \
                          _log_msg +                                          \
                          ((_log_md.is_null() || _log_md.empty())             \
                               ? ""                                           \
                               : ("\n\n" + _log_md.dump(4) + "\n\n")));       \
        ::DiskLogging::route_to_external_sink("INFO", (event_type), _log_msg, \
                                               _log_md);                      \
    } while (0)

#define LOG_WARNING(event_type, msg_, json_)                                  \
    do {                                                                      \
        const std::string _log_msg = (msg_);                                  \
        const nlohmann::json _log_md = (json_);                               \
        disk_logger().log("[" + current_utc_timestamp() +                     \
                          " UTC] [WARNING] " + _log_msg +                     \
                          ((_log_md.is_null() || _log_md.empty())             \
                               ? ""                                           \
                               : ("\n\n" + _log_md.dump(4) + "\n\n")));       \
        ::DiskLogging::route_to_external_sink("WARNING", (event_type),        \
                                               _log_msg, _log_md);            \
    } while (0)

#define LOG_ERROR(event_type, msg_, json_)                                    \
    do {                                                                      \
        const std::string _log_msg = (msg_);                                  \
        const nlohmann::json _log_md = (json_);                               \
        disk_logger().log_error("[" + current_utc_timestamp() +               \
                                " UTC] [ERROR] " + _log_msg +                 \
                                ((_log_md.is_null() || _log_md.empty())       \
                                     ? ""                                     \
                                     : ("\n\n" + _log_md.dump(4) + "\n\n"))); \
        ::DiskLogging::route_to_external_sink("ERROR", (event_type),          \
                                               _log_msg, _log_md);            \
    } while (0)

#else  // DISK_LOGGING_ENABLED  -----------------------------------------------

// Disk-logging compiled-out: macros become no-ops. The external sink
// mechanism still works (set_external_sink / route_to…) so tests can register
// a sink and observe calls even without disk backing.
#define LOG_INFO(...) ((void)0)
#define LOG_WARNING(...) ((void)0)
#define LOG_ERROR(...) ((void)0)

#endif  // DISK_LOGGING_ENABLED
