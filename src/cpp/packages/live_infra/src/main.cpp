#include <curl/curl.h>
#include <datetime_utils.h>
#include <execinfo.h>
#include <signal.h>
#include <sys/resource.h>

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <random>
#include <thread>

#include "cache_path_utils.h"
#include "common_types/log_event_types.h"
#include "datetime_utils.h"
#include "disk_logging/log.h"
#include "env_utils.h"
#include "historical_feature_gen/register_live_sms.h"
#include "engine_control_server/server.h"
#include "live_infra/constants.h"
#include "live_infra/state_updater.h"
#include "market_data/config.h"
#include "market_data/state_machine_dispatcher.h"
#include "market_making/dtypes/global_general_cache.h"
#include "market_making/engine.h"
#include "nats_publisher.h"
#include "sqlite_cache.h"

#ifndef DISABLE_VISUALIZATION
#include "realtime_dashboard/dashboard.h"
#endif

namespace {

std::function<void(const std::string &, const nlohmann::json &)> g_log_callback;

void sigterm_handler(int /*sig*/) {
    // A second SIGTERM, or one before the loop runs, exits at once.
    if (Live::StateUpdater::main_loop_running.load() &&
        !Live::StateUpdater::termination_requested.exchange(true)) {
        return;
    }
    std::_Exit(3);
}

void signal_handler(int sig) {
    if (g_log_callback) {
        g_log_callback("shutdown",
                       {{"event", "graceful_shutdown"}, {"signal", sig}});
    }
    std::_Exit(3);
}

void out_of_memory_handler() {
    if (g_log_callback) {
        g_log_callback("shutdown",
                       {{"event", "graceful_shutdown"}, {"reason", "oom"}});
    }
    std::_Exit(3);
}

void terminate_handler() {
    if (g_log_callback) {
        g_log_callback("shutdown", {{"event", "graceful_shutdown"},
                                    {"reason", "terminate"}});
    }
    std::_Exit(3);
}

void setup_signal_handlers(
    const std::function<void(const std::string &, const nlohmann::json &)>
        &log_callback) {
    g_log_callback = log_callback;

    std::set_terminate(terminate_handler);
    std::set_new_handler(out_of_memory_handler);
    signal(SIGSEGV, signal_handler);
    signal(SIGABRT, signal_handler);
    signal(SIGFPE, signal_handler);
    signal(SIGILL, signal_handler);
    signal(SIGTERM, sigterm_handler);
}

void check_prev_program_uptime() {
    SqliteCache<long long> program_start_time_cache(
        CommonUtils::CachePathUtils::get_sqlite_cache_path(
            "program_start_time.db"));
    SqliteCache<long long> heartbeat_cache(
        CommonUtils::CachePathUtils::get_sqlite_cache_path(
            "heartbeat_cache.db"));

    constexpr long long heartbeat_stale_ms =
        Live::Constants::HEARTBEAT_STALE_MINUTES * 60LL * 1000;
    constexpr long long cron_interval_ms =
        Live::Constants::CRON_RESTART_INTERVAL_MINUTES * 60LL * 1000;
    constexpr long long takeover_gate_ms =
        Live::Constants::STARTUP_TAKEOVER_GATE_MINUTES * 60LL * 1000;

    auto now_ms = DatetimeUtils::get_current_time_ms();
    auto heartbeat = heartbeat_cache.get("heartbeat");

    if (!heartbeat.has_value() ||
        (now_ms - heartbeat.value()) > heartbeat_stale_ms) {
        LOG_INFO(
            Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_STARTUP_PHASE,
            "Heartbeat absent or stale — previous instance presumed dead, "
            "proceeding to take over",
            nlohmann::json{});
        return;
    }

    // Preferred path: trader publishes its scheduled end time, standbys
    // decide based on it. Only the standby within one cron interval of the
    // handoff stays alive; earlier ones exit so a fresher cron tick can
    // produce the actual inheritor.
    auto end_time_ms = program_start_time_cache.get("program_end_time");
    if (end_time_ms.has_value()) {
        long long ms_until_handoff = end_time_ms.value() - now_ms;
        if (ms_until_handoff > cron_interval_ms) {
            LOG_INFO(
                Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_STARTUP_PHASE,
                "Current trader has more than one cron interval of runtime "
                "remaining; exiting so a fresher standby inherits",
                nlohmann::json{});
            std::_Exit(0);
        }
        return;
    }

    // Legacy fallback for caches written by an older binary that didn't
    // publish program_end_time.
    auto cached_start_time =
        program_start_time_cache.get("program_start_time");
    if (cached_start_time.has_value() &&
        (now_ms - cached_start_time.value()) < takeover_gate_ms) {
        LOG_INFO(
            Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_STARTUP_PHASE,
            "Previous trader has not yet reached the takeover gate, exiting "
            "(legacy cache path)",
            nlohmann::json{});
        std::_Exit(0);
    }
}

}  // anonymous namespace

void init_global_configs() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    MarketData::Config::getInstance();

    const bool nats_enabled =
        EnvUtils::env_or("ENABLE_NATS_PUBLISHER", "1") == "1";
    if (!nats_enabled) {
        NatsPublisher::getInstance().set_disabled(true);
        LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_STARTUP_PHASE,
                 "NATS publisher disabled via ENABLE_NATS_PUBLISHER=0",
                 nlohmann::json{});
        return;
    }

    const char *nats_token = std::getenv("NATS_TOKEN");
    if (nats_token == nullptr || std::strlen(nats_token) == 0) {
        std::cerr << "NATS_TOKEN env var is required" << std::endl;
        std::exit(1);
    }

    NatsPublisher::getInstance().connect(
        EnvUtils::env_or("NATS_URL", "nats://localhost:4222"),
        nats_token);
}

int main() {
    LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_STARTUP_BEGIN,
             "Starting live infra", nlohmann::json{});

    Core::GlobalGeneralCache::insert<long long>(
        "program_start_time_ms", DatetimeUtils::get_current_time_ms());
    init_global_configs();

    assert(std::atomic<std::chrono::steady_clock::time_point>{}.is_lock_free());
    assert(std::atomic<long long>{}.is_lock_free());
    assert(std::atomic<double>{}.is_lock_free());

    std::thread([]() {
        constexpr long long cutoff_ms =
            Live::Constants::WATCHDOG_FORCE_ABORT_MINUTES * 60LL * 1000;
        while (true) {
            auto start_ms = Core::GlobalGeneralCache::get<long long>(
                "program_start_time_ms");
            auto now_ms = DatetimeUtils::get_current_time_ms();
            if (start_ms.has_value()) {
                if (now_ms - start_ms.value() > cutoff_ms) {
                    Core::Engine::getInstance().create_system_log(
                        "", Core::LOG_LEVEL_CRASH,
                        "Program uptime exceeded watchdog cutoff, shutting "
                        "down",
                        Core::
                            TRADING_LOG_EVENT_TYPE_HUMAN_INTERVENTION_REQUIRED,
                        nlohmann::json::object(), -1);
                    LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_SHUTDOWN_REQUESTED,
                              "Program uptime exceeded watchdog cutoff, "
                              "shutting down",
                              nlohmann::json{});
                    abort();
                }
            }

            std::this_thread::sleep_for(std::chrono::seconds(30));
        }
    }).detach();

    const char *skip_checks_env = std::getenv("SKIP_DUPLICATE_PROGRAM_CHECKS");
    bool skip_checks =
        skip_checks_env != nullptr && std::strcmp(skip_checks_env, "1") == 0;

    if (skip_checks) {
        LOG_INFO(
            Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_STARTUP_PHASE,
            "SKIP_DUPLICATE_PROGRAM_CHECKS is set, skipping duplicate "
            "program checks",
            nlohmann::json{});
    } else {
        check_prev_program_uptime();
    }

    HistoricalFeatureGen::register_live_state_machines();

    // Optional HTTP control server. Off by default; opt in via env var.
    // Optional HTTP control and introspection server.
    std::unique_ptr<EngineControl::Server> control_server;
    if (const char *enable = std::getenv("ENABLE_ENGINE_CONTROL_SERVER");
        enable != nullptr && std::strcmp(enable, "1") == 0) {
        const char *port_env = std::getenv("ENGINE_CONTROL_SERVER_PORT");
        const char *bind_env = std::getenv("ENGINE_CONTROL_SERVER_BIND");
        const char *mut_env = std::getenv("ENABLE_ENGINE_CONTROL_MUTATIONS");
        int port = (port_env != nullptr) ? std::atoi(port_env) : 8090;
        std::string bind =
            (bind_env != nullptr) ? std::string(bind_env) : "127.0.0.1";
        bool mutations_enabled =
            mut_env != nullptr && std::strcmp(mut_env, "1") == 0;
        control_server = std::make_unique<EngineControl::Server>(
            bind, port, mutations_enabled,
            std::chrono::steady_clock::now());
        control_server->start();
        nlohmann::json log_meta = {
            {"bind", bind},
            {"port", port},
            {"mutations_enabled", mutations_enabled}};
        LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_STARTUP_PHASE,
                 "Engine control server listening", log_meta);
    }

#ifndef DISABLE_VISUALIZATION
    const char *dashboard_env = std::getenv("ENABLE_DASHBOARD");
    bool enable_dashboard =
        dashboard_env != nullptr && std::strcmp(dashboard_env, "1") == 0;

    if (enable_dashboard) {
        LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_STARTUP_PHASE,
                 "Dashboard enabled, initializing render window", nlohmann::json{});
        auto &view = Charting::Dashboard::getInstance();
        view.init(1280, 720, "Live Trading Dashboard");
        view.set_polling_mode(true, 1000);

        // Trade loop runs on background thread; render loop must stay on main thread (GLFW/OpenGL requirement)
        std::thread trade_thread([]() {
            Live::StateUpdater trading_system;
            trading_system.RunTradeLoop();
        });
        trade_thread.detach();

        while (true) {
            view.update();
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }

        view.shutdown();
    } else {
#endif
        Live::StateUpdater trading_system;
        trading_system.RunTradeLoop();
#ifndef DISABLE_VISUALIZATION
    }
#endif

    return 1;
}
