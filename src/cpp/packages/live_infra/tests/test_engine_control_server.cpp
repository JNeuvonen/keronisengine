#include <chrono>
#include <thread>

#include <catch2/catch_all.hpp>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include "engine_control_server/server.h"

using namespace std::chrono_literals;

namespace {

// Picks ports that are almost certainly free on a developer box. If a real
// service collides, tests will fail noisily — preferable to silent flakiness.
constexpr int kPortReadOnly = 18091;
constexpr int kPortMutationsOn = 18092;
constexpr int kPortMutationsDisabled403 = 18093;
constexpr int kPortTriggerSucceeds = 18094;
constexpr int kPortBbaRoundTrip = 18095;
constexpr int kPortMarketEventRoundTrip = 18096;
constexpr int kPortPolymarketRoundTrip = 18097;
constexpr int kPortStrategy404 = 18098;
constexpr int kPortLifecycleGating = 18099;
constexpr int kPortSmReact = 18100;

// Wait for the server to bind. Without this, a fast test can race the
// listener thread.
void wait_until_up(httplib::Client& client) {
    for (int i = 0; i < 50; ++i) {
        auto res = client.Get("/control/health");
        if (res && res->status == 200) return;
        std::this_thread::sleep_for(20ms);
    }
}

}  // namespace

// One TEST_CASE per server instance — Catch2 re-runs the case body for each
// SECTION, which would re-bind the port and race on socket release. Single
// pass with grouped REQUIREs avoids the flakiness.
TEST_CASE("EngineControlServer Phase A read endpoints",
          "[engine_control_server]") {
    EngineControl::Server server(
        "127.0.0.1", kPortReadOnly, /*mutations_enabled=*/false,
        std::chrono::steady_clock::now());
    server.start();

    httplib::Client client("127.0.0.1", kPortReadOnly);
    client.set_connection_timeout(2);
    client.set_read_timeout(2);
    wait_until_up(client);

    // /control/health
    {
        auto res = client.Get("/control/health");
        REQUIRE(res);
        REQUIRE(res->status == 200);
        auto body = nlohmann::json::parse(res->body);
        REQUIRE(body["status"] == "ok");
        REQUIRE(body.contains("initialized"));
        REQUIRE(body.contains("state_ready"));
        REQUIRE(body.contains("trading"));
        REQUIRE(body["mutations_enabled"] == false);
        REQUIRE(body.contains("uptime_ms"));
    }

    // /control/engine/orders
    {
        auto res = client.Get("/control/engine/orders");
        REQUIRE(res);
        REQUIRE(res->status == 200);
        REQUIRE(nlohmann::json::parse(res->body).is_array());
    }

    // /control/engine/trades
    {
        auto res = client.Get("/control/engine/trades");
        REQUIRE(res);
        REQUIRE(res->status == 200);
        REQUIRE(nlohmann::json::parse(res->body).is_array());
    }

    // /control/engine/bba without required query params → 400
    {
        auto res = client.Get("/control/engine/bba");
        REQUIRE(res);
        REQUIRE(res->status == 400);
        auto body = nlohmann::json::parse(res->body);
        REQUIRE(body.contains("error"));
    }

    // /control/engine/counters
    {
        auto res = client.Get("/control/engine/counters");
        REQUIRE(res);
        REQUIRE(res->status == 200);
        auto body = nlohmann::json::parse(res->body);
        REQUIRE(body.contains("num_market_events_processed"));
        REQUIRE(body.contains("num_backpressure_events"));
        REQUIRE(body.contains("num_dropped_trade_events"));
        REQUIRE(body.contains("num_dropped_orderbook_events"));
        REQUIRE(body.contains("latest_market_event_time_ms"));
    }

    // /control/engine/time
    {
        auto res = client.Get("/control/engine/time");
        REQUIRE(res);
        REQUIRE(res->status == 200);
        auto body = nlohmann::json::parse(res->body);
        REQUIRE(body.contains("engine_time_ms"));
        REQUIRE(body.contains("wall_time_ms"));
    }

    // unknown endpoint → 404
    {
        auto res = client.Get("/control/engine/does-not-exist");
        REQUIRE(res);
        REQUIRE(res->status == 404);
    }

    server.stop();
}

TEST_CASE("EngineControlServer mutations_enabled flag plumbs through",
          "[engine_control_server]") {
    EngineControl::Server server(
        "127.0.0.1", kPortMutationsOn, /*mutations_enabled=*/true,
        std::chrono::steady_clock::now());
    server.start();

    httplib::Client client("127.0.0.1", kPortMutationsOn);
    client.set_connection_timeout(2);
    client.set_read_timeout(2);
    wait_until_up(client);

    auto res = client.Get("/control/health");
    REQUIRE(res);
    REQUIRE(res->status == 200);
    auto body = nlohmann::json::parse(res->body);
    REQUIRE(body["mutations_enabled"] == true);

    server.stop();
}

TEST_CASE("EngineControlServer Phase B.1 mutations disabled returns 403",
          "[engine_control_server]") {
    EngineControl::Server server(
        "127.0.0.1", kPortMutationsDisabled403, /*mutations_enabled=*/false,
        std::chrono::steady_clock::now());
    server.start();

    httplib::Client client("127.0.0.1", kPortMutationsDisabled403);
    client.set_connection_timeout(2);
    client.set_read_timeout(2);
    wait_until_up(client);

    // A mutating order endpoint stub fires.
    {
        auto res = client.Post("/control/engine/orders/created-ack",
                               "{}", "application/json");
        REQUIRE(res);
        REQUIRE(res->status == 403);
        auto body = nlohmann::json::parse(res->body);
        REQUIRE(body.contains("error"));
    }

    // A trigger endpoint stub fires too — same gate covers both groups.
    {
        auto res = client.Post("/control/engine/triggers/run-periodic-calls",
                               "", "application/json");
        REQUIRE(res);
        REQUIRE(res->status == 403);
    }

    server.stop();
}

TEST_CASE("EngineControlServer Phase B.1 trigger endpoints succeed when "
          "mutations enabled",
          "[engine_control_server]") {
    EngineControl::Server server(
        "127.0.0.1", kPortTriggerSucceeds, /*mutations_enabled=*/true,
        std::chrono::steady_clock::now());
    server.start();

    httplib::Client client("127.0.0.1", kPortTriggerSucceeds);
    client.set_connection_timeout(2);
    client.set_read_timeout(2);
    wait_until_up(client);

    // flush_trading_logs is the safest trigger to fire against an engine
    // with no strategies loaded — pure logging side effect.
    {
        auto res = client.Post(
            "/control/engine/triggers/flush-trading-logs?force=true",
            "", "application/json");
        REQUIRE(res);
        REQUIRE(res->status == 200);
        auto body = nlohmann::json::parse(res->body);
        REQUIRE(body["ok"] == true);
        REQUIRE(body["force"] == true);
    }

    // emit_state_snapshot with bypass_guard — also no-op-ish on an empty engine.
    {
        auto res = client.Post(
            "/control/engine/triggers/emit-state-snapshot?bypass_guard=true",
            "", "application/json");
        REQUIRE(res);
        REQUIRE(res->status == 200);
        auto body = nlohmann::json::parse(res->body);
        REQUIRE(body["ok"] == true);
        REQUIRE(body["bypass_guard"] == true);
    }

    server.stop();
}

TEST_CASE("EngineControlServer Phase B.1 BBA write-then-read round-trip",
          "[engine_control_server]") {
    EngineControl::Server server(
        "127.0.0.1", kPortBbaRoundTrip, /*mutations_enabled=*/true,
        std::chrono::steady_clock::now());
    server.start();

    httplib::Client client("127.0.0.1", kPortBbaRoundTrip);
    client.set_connection_timeout(2);
    client.set_read_timeout(2);
    wait_until_up(client);

    // Write a BBA snapshot for a synthetic key the prod engine will never use.
    nlohmann::json payload = {
        {"exchange", "test_exchange"},
        {"market_type", "test_mt"},
        {"symbol", "TEST_SYMBOL_BBA_RTT"},
        {"bid_price", 99.5},
        {"bid_qty", 1.25},
        {"ask_price", 100.5},
        {"ask_qty", 2.0},
        {"timestamp_ms", 1700000000000LL},
    };
    {
        auto res = client.Post("/control/engine/bba", payload.dump(),
                               "application/json");
        REQUIRE(res);
        REQUIRE(res->status == 200);
    }

    // Read it back via Phase A endpoint — the one invariant Phase B needs to
    // prove: writes go through to engine state.
    {
        auto res = client.Get(
            "/control/engine/bba?exchange=test_exchange&market_type=test_mt&"
            "symbol=TEST_SYMBOL_BBA_RTT");
        REQUIRE(res);
        REQUIRE(res->status == 200);
        auto body = nlohmann::json::parse(res->body);
        REQUIRE(body["best_bid_price"] == 99.5);
        REQUIRE(body["best_bid_qty"] == 1.25);
        REQUIRE(body["best_ask_price"] == 100.5);
        REQUIRE(body["best_ask_qty"] == 2.0);
        REQUIRE(body["timestamp_ms"] == 1700000000000LL);
    }

    server.stop();
}

TEST_CASE("EngineControlServer Phase B.2 MarketEvent PRICE_UPDATE round-trip",
          "[engine_control_server]") {
    EngineControl::Server server(
        "127.0.0.1", kPortMarketEventRoundTrip, /*mutations_enabled=*/true,
        std::chrono::steady_clock::now());
    server.start();

    httplib::Client client("127.0.0.1", kPortMarketEventRoundTrip);
    client.set_connection_timeout(2);
    client.set_read_timeout(2);
    wait_until_up(client);

    // PRICE_UPDATE = 5 in MarketEventType enum (enums.h:31).
    // Inject a price via MarketEvent then read it back via the existing
    // Phase A /control/engine/price endpoint.
    nlohmann::json payload = {
        {"type", 5},
        {"event_time", 1700000000000LL},
        {"symbol", "TEST_SYMBOL_ME_PRICE"},
        {"exchange_name", "test_exchange"},
        {"market_type", "test_mt"},
        {"price", 12345.67},
    };
    {
        auto res = client.Post("/control/engine/market-event",
                               payload.dump(), "application/json");
        REQUIRE(res);
        REQUIRE(res->status == 200);
        auto body = nlohmann::json::parse(res->body);
        REQUIRE(body["ok"] == true);
        REQUIRE(body["mode"] == "process");
    }
    {
        auto res = client.Get(
            "/control/engine/price?exchange=test_exchange&market_type=test_mt"
            "&symbol=TEST_SYMBOL_ME_PRICE");
        REQUIRE(res);
        REQUIRE(res->status == 200);
        auto body = nlohmann::json::parse(res->body);
        REQUIRE(body["price"] == 12345.67);
    }

    server.stop();
}

TEST_CASE("EngineControlServer Phase B.2 polymarket event add + bba round-trip",
          "[engine_control_server]") {
    EngineControl::Server server(
        "127.0.0.1", kPortPolymarketRoundTrip, /*mutations_enabled=*/true,
        std::chrono::steady_clock::now());
    server.start();

    httplib::Client client("127.0.0.1", kPortPolymarketRoundTrip);
    client.set_connection_timeout(2);
    client.set_read_timeout(2);
    wait_until_up(client);

    const std::string condition_id = "test_condition_id_b2_pmrt";

    // Add a polymarket event with 2 outcomes (Yes / No). Atomics start at 0.
    {
        nlohmann::json payload = {
            {"condition_id", condition_id},
            {"series_slug", "test-series"},
            {"slug", "test-event"},
            {"event_start_time_ms", 1700000000000LL},
            {"event_end_time_ms", 1700000999999LL},
            {"num_outcomes", 2},
            {"outcome_names", {"Yes", "No"}},
            {"clob_token_ids", {"token_yes", "token_no"}},
        };
        auto res = client.Post("/control/engine/polymarket/event",
                               payload.dump(), "application/json");
        REQUIRE(res);
        REQUIRE(res->status == 200);
    }

    // Read it back — atomics should be 0 since we haven't pushed BBA yet.
    {
        auto res = client.Get(
            ("/control/engine/polymarket/event?condition_id=" + condition_id)
                .c_str());
        REQUIRE(res);
        REQUIRE(res->status == 200);
        auto body = nlohmann::json::parse(res->body);
        REQUIRE(body["condition_id"] == condition_id);
        REQUIRE(body["num_outcomes"] == 2);
        REQUIRE(body["outcomes"].size() == 2);
        REQUIRE(body["outcomes"][0]["name"] == "Yes");
        REQUIRE(body["outcomes"][0]["best_bid"] == 0.0);
        REQUIRE(body["outcomes"][0]["best_ask"] == 0.0);
    }

    // Push BBA on outcome 0 and verify atomic flips.
    {
        nlohmann::json payload = {
            {"condition_id", condition_id},
            {"outcome_index", 0},
            {"best_bid", 0.42},
            {"best_ask", 0.45},
            {"timestamp_ms", 1700000111111LL},
        };
        auto res = client.Post("/control/engine/polymarket/bba",
                               payload.dump(), "application/json");
        REQUIRE(res);
        REQUIRE(res->status == 200);
    }
    {
        auto res = client.Get(
            ("/control/engine/polymarket/event?condition_id=" + condition_id)
                .c_str());
        REQUIRE(res);
        REQUIRE(res->status == 200);
        auto body = nlohmann::json::parse(res->body);
        REQUIRE(body["outcomes"][0]["best_bid"] == 0.42);
        REQUIRE(body["outcomes"][0]["best_ask"] == 0.45);
        REQUIRE(body["outcomes"][0]["bba_timestamp_ms"] == 1700000111111LL);
        // outcome 1 still untouched.
        REQUIRE(body["outcomes"][1]["best_bid"] == 0.0);
    }

    // Cleanup so re-runs of the test suite don't accumulate events.
    {
        nlohmann::json payload = {{"condition_id", condition_id}};
        auto res = client.Post("/control/engine/polymarket/event/remove",
                               payload.dump(), "application/json");
        REQUIRE(res);
        REQUIRE(res->status == 200);
    }

    server.stop();
}

TEST_CASE("EngineControlServer Phase C strategy lookups 404 on missing id",
          "[engine_control_server]") {
    EngineControl::Server server(
        "127.0.0.1", kPortStrategy404, /*mutations_enabled=*/false,
        std::chrono::steady_clock::now());
    server.start();

    httplib::Client client("127.0.0.1", kPortStrategy404);
    client.set_connection_timeout(2);
    client.set_read_timeout(2);
    wait_until_up(client);

    // No strategies are loaded in the test fixture; all per-strategy
    // endpoints should hit the require_strategy 404 path.
    const std::vector<std::string> paths = {
        "/control/strategy/orders?id=999",
        "/control/strategy/trades?id=999",
        "/control/strategy/virtual-balance?id=999",
        "/control/strategy/recent-orders?id=999",
        "/control/strategy/sm/sync-progress?id=999",
        "/control/strategy/sm/features?id=999",
    };
    for (const auto& path : paths) {
        auto res = client.Get(path.c_str());
        REQUIRE(res);
        REQUIRE(res->status == 404);
        auto body = nlohmann::json::parse(res->body);
        REQUIRE(body.contains("error"));
    }

    // Missing id returns 400, not 404.
    {
        auto res = client.Get("/control/strategy/orders");
        REQUIRE(res);
        REQUIRE(res->status == 400);
    }
    // Non-int id returns 400.
    {
        auto res = client.Get("/control/strategy/orders?id=abc");
        REQUIRE(res);
        REQUIRE(res->status == 400);
    }

    server.stop();
}

TEST_CASE("EngineControlServer Phase C lifecycle endpoints respect gating",
          "[engine_control_server]") {
    // First sub-server: mutations off, all 3 lifecycle endpoints return 403.
    {
        EngineControl::Server server(
            "127.0.0.1", kPortLifecycleGating,
            /*mutations_enabled=*/false,
            std::chrono::steady_clock::now());
        server.start();

        httplib::Client client("127.0.0.1", kPortLifecycleGating);
        client.set_connection_timeout(2);
        client.set_read_timeout(2);
        wait_until_up(client);

        for (const std::string& path :
             {"/control/engine/state-ready",
              "/control/engine/stop-trading",
              "/control/engine/gracefully-stop"}) {
            auto res = client.Post(path.c_str(), "", "application/json");
            REQUIRE(res);
            REQUIRE(res->status == 403);
        }

        server.stop();
    }

    // Second sub-server: mutations on. state-ready then stop-trading flip
    // /control/health.state_ready. Skip gracefully-stop — it kills the
    // engine event loop and would interfere with subsequent test cases
    // sharing the engine singleton.
    {
        EngineControl::Server server(
            "127.0.0.1", kPortLifecycleGating + 100,
            /*mutations_enabled=*/true,
            std::chrono::steady_clock::now());
        server.start();

        httplib::Client client("127.0.0.1", kPortLifecycleGating + 100);
        client.set_connection_timeout(2);
        client.set_read_timeout(2);
        wait_until_up(client);

        {
            auto res = client.Post("/control/engine/state-ready", "",
                                   "application/json");
            REQUIRE(res);
            REQUIRE(res->status == 200);
        }
        {
            auto res = client.Get("/control/health");
            REQUIRE(res);
            REQUIRE(res->status == 200);
            auto body = nlohmann::json::parse(res->body);
            REQUIRE(body["state_ready"] == true);
        }
        {
            auto res = client.Post("/control/engine/stop-trading", "",
                                   "application/json");
            REQUIRE(res);
            REQUIRE(res->status == 200);
        }
        {
            auto res = client.Get("/control/health");
            REQUIRE(res);
            REQUIRE(res->status == 200);
            auto body = nlohmann::json::parse(res->body);
            REQUIRE(body["state_ready"] == false);
        }

        server.stop();
    }
}

TEST_CASE("EngineControlServer Phase C SM react gates + parses MarketEvent",
          "[engine_control_server]") {
    EngineControl::Server server(
        "127.0.0.1", kPortSmReact, /*mutations_enabled=*/true,
        std::chrono::steady_clock::now());
    server.start();

    httplib::Client client("127.0.0.1", kPortSmReact);
    client.set_connection_timeout(2);
    client.set_read_timeout(2);
    wait_until_up(client);

    // Valid TRADE-shaped MarketData::MarketEvent body.
    nlohmann::json payload = {
        {"type", 0},  // MarketData::MarketEvent::TRADE
        {"trade",
         {
             {"symbol", "TEST_SYMBOL_C_REACT"},
             {"exchange_name", "test_exchange"},
             {"market_type", "test_mt"},
             {"price", 100.0},
             {"qty", 0.5},
             {"is_buyer_maker", false},
             {"timestamp_ms", 1700000000000LL},
         }},
    };

    // No strategy id 999 — should 404 *after* parsing succeeds. This proves
    // the route is wired through gating + reaches the require_strategy path.
    auto res = client.Post("/control/strategy/sm/react?id=999",
                           payload.dump(), "application/json");
    REQUIRE(res);
    REQUIRE(res->status == 404);
    auto body = nlohmann::json::parse(res->body);
    REQUIRE(body.contains("error"));

    server.stop();
}
