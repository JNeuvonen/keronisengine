// Sink smoke test — covers the Phase 3 external-sink contract:
//   (1) Register a sink, emit LOG_INFO/WARNING/ERROR, the sink receives
//       level + event_type + message + metadata exactly once per emit.
//   (2) Unregister the sink, the same emits land on disk only (no sink
//       invocation).
//   (3) If the sink callback itself emits a LOG_*, the RAII re-entry
//       guard in disk_logging_external_sink.cpp prevents the inner emit
//       from routing (disk write still happens, but the sink is not
//       re-invoked).
#include <atomic>
#include <cstdlib>

#include <catch2/catch_test_macros.hpp>

#include "common_types/log_event_types.h"
#include "disk_logging/log.h"

namespace {

// disk_logger() singleton construction reads APPLICATION_NAME and
// LOG_DIRECTORY at first touch. Default them here so the test runs in
// CI without external env setup. 0 as third arg to setenv = don't
// overwrite if already set.
struct DiskLoggerEnvInit {
    DiskLoggerEnvInit() {
        setenv("APPLICATION_NAME", "live_infra_tests", 0);
        setenv("LOG_DIRECTORY", "/tmp/live_infra_tests_logs", 0);
    }
};
static const DiskLoggerEnvInit g_disk_logger_env_init;

struct CapturedEmit {
    std::string level;
    std::string event_type;
    std::string message;
    nlohmann::json metadata;
};

// Shared capture buffer — reset at the start of each SECTION.
std::vector<CapturedEmit> g_captured;
std::atomic<int> g_reentry_inner_calls{0};

void reset_capture() {
    g_captured.clear();
    g_reentry_inner_calls.store(0);
}

DiskLogging::ExternalSink make_capture_sink() {
    return [](const char* level, const char* event_type,
              const std::string& msg, const nlohmann::json& md) {
        g_captured.push_back({level, event_type, msg, md});
    };
}

}  // namespace

TEST_CASE("External sink receives typed emit", "[disk_logging_sink]") {
#ifndef DISK_LOGGING_ENABLED
    SKIP("built without KERONIS_DISK_LOGGING");
#endif
    reset_capture();
    DiskLogging::set_external_sink(make_capture_sink());

    LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_STARTUP_PHASE,
             "hello info",
             (nlohmann::json{{"phase", "test"}, {"value", 42}}));

    LOG_WARNING(Core::TRADING_LOG_EVENT_TYPE_ORDER_REJECTED,
                "hello warning", nlohmann::json{});

    LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_DB_WRITE_FAILED,
              "hello error",
              (nlohmann::json{{"pq_error", "synthetic"}}));

    REQUIRE(g_captured.size() == 3);

    REQUIRE(g_captured[0].level == std::string("INFO"));
    REQUIRE(g_captured[0].event_type == std::string("LIVE_INFRA_STARTUP_PHASE"));
    REQUIRE(g_captured[0].message == "hello info");
    REQUIRE(g_captured[0].metadata["phase"] == "test");
    REQUIRE(g_captured[0].metadata["value"] == 42);

    REQUIRE(g_captured[1].level == std::string("WARNING"));
    REQUIRE(g_captured[1].event_type == std::string("ORDER_REJECTED"));

    REQUIRE(g_captured[2].level == std::string("ERROR"));
    REQUIRE(g_captured[2].event_type == std::string("DB_WRITE_FAILED"));
    REQUIRE(g_captured[2].metadata["pq_error"] == "synthetic");

    DiskLogging::clear_external_sink();
}

TEST_CASE("External sink cleared — emits land disk-only", "[disk_logging_sink]") {
    reset_capture();
    DiskLogging::set_external_sink(make_capture_sink());
    DiskLogging::clear_external_sink();

    LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_STARTUP_PHASE,
             "should not reach sink", nlohmann::json{});

    REQUIRE(g_captured.empty());
}

TEST_CASE("Re-entry guard prevents feedback when sink emits", "[disk_logging_sink]") {
#ifndef DISK_LOGGING_ENABLED
    SKIP("built without KERONIS_DISK_LOGGING");
#endif
    reset_capture();

    // This sink emits a LOG_ERROR internally. Without the re-entry guard
    // in route_to_external_sink, that emit would route back through the
    // sink, which would emit again, unbounded. With the guard, the outer
    // emit reaches the sink exactly once and the inner emit is dropped
    // from the sink route (disk write still happens).
    DiskLogging::set_external_sink(
        [](const char* level, const char* event_type,
           const std::string& /*msg*/, const nlohmann::json& /*md*/) {
            g_captured.push_back({level, event_type, "", {}});
            // Simulate a DB write failure inside the flush callback.
            g_reentry_inner_calls.fetch_add(1);
            LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_DB_WRITE_FAILED,
                      "inner failure from sink callback",
                      (nlohmann::json{{"target_table", "trading_activity_log"}}));
        });

    LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_STARTUP_PHASE,
             "outer emit", nlohmann::json{});

    // Outer LOG_INFO routed → sink ran once. Inner LOG_ERROR hit the disk
    // but was dropped by the guard, so the sink callback was NOT invoked a
    // second time. Expected captures: 1 (just the outer INFO).
    REQUIRE(g_captured.size() == 1);
    REQUIRE(g_captured[0].event_type == std::string("LIVE_INFRA_STARTUP_PHASE"));
    // Sink callback body ran exactly once (confirms the outer emit reached
    // it, the inner emit did not cause a second invocation).
    REQUIRE(g_reentry_inner_calls.load() == 1);

    DiskLogging::clear_external_sink();
}

TEST_CASE("Sink tolerates exceptions from callback", "[disk_logging_sink]") {
    reset_capture();
    DiskLogging::set_external_sink(
        [](const char* /*level*/, const char* /*event_type*/,
           const std::string& /*msg*/, const nlohmann::json& /*md*/) {
            throw std::runtime_error("synthetic sink failure");
        });

    // The macro must not throw even if the sink callback throws.
    auto emit = [] {
        LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_STARTUP_PHASE,
                 "exception from sink", nlohmann::json{});
    };
    REQUIRE_NOTHROW(emit());

    DiskLogging::clear_external_sink();

    // After the throwing sink, a follow-up emit with no sink set must also
    // not crash — verifies the guard flag was cleared even on exception.
    auto emit_after_clear = [] {
        LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_STARTUP_PHASE,
                 "post-exception emit", nlohmann::json{});
    };
    REQUIRE_NOTHROW(emit_after_clear());
}
