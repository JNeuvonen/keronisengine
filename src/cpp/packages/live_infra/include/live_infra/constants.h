#pragma once

namespace Live {
namespace Constants {

constexpr const char* STREAM_EVENT_TYPE_ORDER_UPDATE = "order_update";
constexpr const char* STREAM_EVENT_TYPE_HEARTBEAT = "heartbeat";
constexpr const char* STREAM_EVENT_TYPE_POSITION_UPDATE = "position_update";
constexpr const char* STREAM_EVENT_TYPE_ORDER_CREATE_FAILED =
    "order_create_failed";
constexpr const char* STREAM_EVENT_TYPE_ORDER_CANCEL_FAILED =
    "order_cancel_failed";
constexpr const char* STREAM_EVENT_TYPE_FILL_STATUS_UPDATE =
    "fill_status_update";

constexpr long long PROGRAM_MAX_RUNTIME_MINUTES = 60 * 24;
constexpr long long PRE_SHUTDOWN_CANCEL_BUFFER_MINUTES = 3;

// Cron-driven restart cadence. Must match /etc/cron.d/trading_service.
// The standby-takeover logic in check_prev_program_uptime depends on this.
constexpr long long CRON_RESTART_INTERVAL_MINUTES = 30;

// A cron-spawned instance waits for the file lock at most this long before
// exiting. Must be > CRON_RESTART_INTERVAL_MINUTES so the standby that will
// inherit never times out before the current trader's main loop exits at
// PROGRAM_MAX_RUNTIME_MINUTES.
constexpr long long STANDBY_LOCK_WAIT_TIMEOUT_MINUTES =
    CRON_RESTART_INTERVAL_MINUTES + 5;

// Legacy fallback gate: if a new instance finds program_start_time in the
// cache but not program_end_time (older binary wrote the cache), it exits
// silently while the previous trader has been alive less than this long.
// Equivalent to the historical "23h" rule expressed as runtime − one cron
// interval.
constexpr long long STARTUP_TAKEOVER_GATE_MINUTES =
    PROGRAM_MAX_RUNTIME_MINUTES - CRON_RESTART_INTERVAL_MINUTES * 2;

// Heartbeat is presumed dead after this many minutes without a refresh.
constexpr long long HEARTBEAT_STALE_MINUTES = 60;

// Running instance refreshes its heartbeat at most this often.
constexpr long long HEARTBEAT_UPDATE_INTERVAL_MINUTES = 5;

// Cadence of the dedicated liveness thread: how often it writes the
// system-health file and sends venue session heartbeats. Polymarket's
// cancel-on-disconnect timeout is observed at ~5s, so 1s gives ~5x margin and
// tolerates a couple of slow/failed beats before orders are auto-cancelled.
constexpr long long LIVENESS_THREAD_INTERVAL_MS = 1000;

// Main-loop liveness gate: the dedicated thread only writes the health file and
// sends venue heartbeats when the main loop has ticked within this window. If
// the loop is wedged longer, both stop — external monitors then see a stale
// health file and the venue cancels our orders (correct dead-man behavior).
constexpr long long MAIN_LOOP_LIVENESS_STALE_MS = 5000;

// System-health watchdog file, written (timestamp) under $DATA_DIR_ROOT. A
// separate process can rsync it and abort open orders if it goes stale —
// multi-purpose (works for any venue, not just Polymarket).
constexpr const char* SYSTEM_HEALTH_FILE_RELPATH =
    "caches/live_infra/system_health";

// Watchdog thread fires abort() if the graceful main-loop exit at
// PROGRAM_MAX_RUNTIME_MINUTES doesn't happen. One-hour safety margin.
constexpr long long WATCHDOG_FORCE_ABORT_MINUTES =
    PROGRAM_MAX_RUNTIME_MINUTES + 60;

}  // namespace Constants
}  // namespace Live
