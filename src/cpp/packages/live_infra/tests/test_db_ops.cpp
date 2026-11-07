#include <libpq-fe.h>

#include <catch2/catch_all.hpp>
#include <chrono>
#include <string>
#include <vector>

#include "live_infra/config.h"
#include "live_infra/engine_signal_store.h"
#include "live_infra/strategy_command_store.h"
#include "market_making/engine.h"
#include "postgre_utils.h"

TEST_CASE("batch_update_entries updates trade rows correctly",
          "[db_batch_update_entries]") {
    auto& config = Live::Config::getInstance();
    const char* conninfo = config.get_db_connection_string();

    // Use high unique_ids to avoid collision with real data
    const auto timestamp =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    const unsigned long long base_unique_id = 9000000000000ULL + timestamp;

    std::vector<long long> inserted_ids;

    // Insert test trades
    {
        std::vector<nlohmann::json> trades = {
            {{"unique_id", base_unique_id},
             {"symbol", "BTCUSDT"},
             {"strategy_id", 18},
             {"exchange_name", "binance"},
             {"market_type", "swap"},
             {"open_price", 50000.0},
             {"close_price", 51000.0},
             {"open_quantity", 0.1},
             {"closed_quantity", 0.0},
             {"pnl", 100.0},
             {"pnl_percentage", 2.0},
             {"fees", 10.0},
             {"is_long_trade", true},
             {"is_short_trade", false},
             {"is_live_trade", false},
             {"is_closed_trade", false},
             {"strategy_version", 2}},
            {{"unique_id", base_unique_id + 1},
             {"symbol", "ETHUSDT"},
             {"strategy_id", 18},
             {"exchange_name", "binance"},
             {"market_type", "swap"},
             {"open_price", 3000.0},
             {"close_price", 2950.0},
             {"open_quantity", 1.0},
             {"closed_quantity", 0.5},
             {"pnl", -50.0},
             {"pnl_percentage", -1.67},
             {"fees", 5.0},
             {"is_long_trade", false},
             {"is_short_trade", true},
             {"is_live_trade", false},
             {"is_closed_trade", false},
             {"strategy_version", 2}},
            {{"unique_id", base_unique_id + 2},
             {"symbol", "SOLUSDT"},
             {"strategy_id", 18},
             {"exchange_name", "binance"},
             {"market_type", "swap"},
             {"open_price", 100.0},
             {"close_price", 105.0},
             {"open_quantity", 10.0},
             {"closed_quantity", 10.0},
             {"pnl", 45.0},
             {"pnl_percentage", 4.5},
             {"fees", 5.0},
             {"is_long_trade", true},
             {"is_short_trade", false},
             {"is_live_trade", false},
             {"is_closed_trade", true},
             {"strategy_version", 2}},
        };

        REQUIRE(PQUtils::write_to_postgres(conninfo, "trade", trades));

        // Fetch inserted IDs
        const std::string query =
            "SELECT id FROM trade WHERE unique_id >= " +
            std::to_string(base_unique_id) +
            " AND unique_id <= " + std::to_string(base_unique_id + 2) +
            " ORDER BY unique_id";
        auto res = PQUtils::fetch_from_postgres(conninfo, query.c_str());
        REQUIRE(res.size() == 3);

        for (const auto& row : res) {
            inserted_ids.push_back(std::stoll(row["id"].get<std::string>()));
        }
    }

    // Perform batch update
    {
        std::vector<nlohmann::json> updates = {
            // First entry determines the superset of all updatable columns
            {{"id", inserted_ids[0]},
             {"pnl", 150.0},
             {"pnl_percentage", 3.0},
             {"closed_quantity", 0.1},
             {"is_closed_trade", true},
             {"close_price",
              51000.0}},  // Include all columns that any row might update
            // Update only pnl fields for second trade
            {{"id", inserted_ids[1]},
             {"pnl", -25.0},
             {"pnl_percentage", -0.83}},
            // Update only close price for third trade
            {{"id", inserted_ids[2]}, {"close_price", 106.0}},
        };

        REQUIRE(
            PQUtils::batch_update_entries(conninfo, "trade", updates, "id"));
    }

    // Verify updates
    {
        const std::string query =
            "SELECT id, symbol, pnl, pnl_percentage, "
            "closed_quantity, close_price, is_closed_trade "
            "FROM trade WHERE id IN (" +
            std::to_string(inserted_ids[0]) + "," +
            std::to_string(inserted_ids[1]) + "," +
            std::to_string(inserted_ids[2]) + ") ORDER BY id";
        auto res = PQUtils::fetch_from_postgres(conninfo, query.c_str());
        REQUIRE(res.size() == 3);

        // Trade 1: all updated fields should change
        {
            auto& r = res[0];
            REQUIRE(r["symbol"].get<std::string>() == "BTCUSDT");
            REQUIRE(r["pnl"].get<std::string>() == "150");
            REQUIRE(r["pnl_percentage"].get<std::string>() == "3");
            REQUIRE(r["closed_quantity"].get<std::string>() == "0.1");
            REQUIRE(r["is_closed_trade"].get<std::string>() == "t");
        }

        // Trade 2: only pnl fields updated
        {
            auto& r = res[1];
            REQUIRE(r["symbol"].get<std::string>() == "ETHUSDT");
            REQUIRE(r["pnl"].get<std::string>() == "-25");
            REQUIRE(r["pnl_percentage"].get<std::string>() == "-0.83");
            REQUIRE(r["closed_quantity"].get<std::string>() ==
                    "0.5");  // unchanged
        }

        // Trade 3: only close_price updated
        {
            auto& r = res[2];
            REQUIRE(r["symbol"].get<std::string>() == "SOLUSDT");
            REQUIRE(r["close_price"].get<std::string>() == "106");
            REQUIRE(r["pnl"].get<std::string>() == "45");  // unchanged
            REQUIRE(r["is_closed_trade"].get<std::string>() ==
                    "t");  // unchanged
        }
    }

    // Cleanup: delete test trades
    {
        std::string delete_query =
            "DELETE FROM trade WHERE unique_id >= " +
            std::to_string(base_unique_id) +
            " AND unique_id <= " + std::to_string(base_unique_id + 2);

        PGconn* conn = PQconnectdb(conninfo);
        REQUIRE(PQstatus(conn) == CONNECTION_OK);

        PGresult* res = PQexec(conn, delete_query.c_str());
        REQUIRE(PQresultStatus(res) == PGRES_COMMAND_OK);

        PQclear(res);
        PQfinish(conn);
    }
}

// Needs DB_CONNECTION_STR for a DB with migration 00215:
// `live_infra_tests "[engine-signals-db]"`.
TEST_CASE("engine_signal store filters, dispatches and acknowledges per engine",
          "[.][engine-signals-db]") {
    const char* conninfo_env = std::getenv("DB_CONNECTION_STR");
    if (conninfo_env == nullptr) SKIP("DB_CONNECTION_STR unset");
    const std::string conninfo = conninfo_env;
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    const std::string consumer = "test-consumer:" + std::to_string(now_ms);
    const std::string type = "test_store_" + std::to_string(now_ms);

    PGconn* conn = PQconnectdb(conninfo.c_str());
    REQUIRE(PQstatus(conn) == CONNECTION_OK);
    auto sql = [conn](const std::string& query) -> std::string {
        PGresult* res = PQexec(conn, query.c_str());
        const auto status = PQresultStatus(res);
        std::string value = (status == PGRES_TUPLES_OK && PQntuples(res) > 0)
                                ? PQgetvalue(res, 0, 0)
                                : "";
        const std::string error = PQresultErrorMessage(res);
        PQclear(res);
        INFO(query << " -> " << error);
        REQUIRE((status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK));
        return value;
    };
    auto insert = [&](const std::string& columns, const std::string& values) {
        return sql("insert into engine_signal (signal_type, created_by" +
                   columns + ") values ('" + type + "', 'test'" + values +
                   ") returning id");
    };
    const auto untargeted = insert(", key, payload", ", 'k1', '{\"a\": 1}'");
    insert(", target", ", 'some-other-engine'");
    insert(", expires_at", ", now() - interval '1 minute'");
    sql("insert into engine_signal (signal_type, created_by) values ('" + type +
        "_other', 'test')");
    const auto targeted = insert(", target", ", '" + consumer + "'");

    Live::EngineSignalStore store(conninfo, consumer);
    const auto fetched = store.fetch_unacked({type});
    REQUIRE(fetched.size() == 2);
    CHECK(std::to_string(fetched[0].id) == untargeted);
    CHECK(fetched[0].key == "k1");
    CHECK(fetched[0].payload.at("a") == 1);
    CHECK(std::to_string(fetched[1].id) == targeted);

    auto& engine = Core::Engine::getInstance();
    engine.register_engine_signal_handler(
        type, [](const Core::EngineSignal& signal) {
            return signal.key == "k1"
                       ? Core::EngineSignalOutcome{Core::EngineSignalStatus::APPLIED, ""}
                       : Core::EngineSignalOutcome{Core::EngineSignalStatus::FAILED,
                                                   "no key"};
        });
    const auto outcomes = engine.apply_engine_signals(fetched);
    for (std::size_t i = 0; i < fetched.size(); ++i) {
        store.ack(fetched[i], outcomes[i]);
    }

    CHECK(store.fetch_unacked({type}).empty());
    CHECK(sql("select status from engine_signal_ack where signal_id = " +
              untargeted + " and consumer_id = '" + consumer + "'") == "applied");
    CHECK(sql("select error from engine_signal_ack where signal_id = " +
              targeted + " and consumer_id = '" + consumer + "'") == "no key");

    // Acks are per engine: another engine still sees the untargeted row.
    Live::EngineSignalStore other(conninfo, consumer + "-b");
    CHECK(other.fetch_unacked({type}).size() == 1);

    sql("delete from engine_signal where signal_type in ('" + type + "', '" +
        type + "_other')");
    PQfinish(conn);
}

// Needs DB_CONNECTION_STR for a DB with migrations 00244 and 00249:
// `live_infra_tests "[strategy-commands-db]"`.
TEST_CASE("strategy command store claims once, keeps a finished outcome and resumes open commands",
          "[.][strategy-commands-db]") {
    const char* conninfo_env = std::getenv("DB_CONNECTION_STR");
    if (conninfo_env == nullptr) SKIP("DB_CONNECTION_STR unset");
    const std::string conninfo = conninfo_env;
    PGconn* conn = PQconnectdb(conninfo.c_str());
    REQUIRE(PQstatus(conn) == CONNECTION_OK);
    auto sql = [conn](const std::string& query) -> std::string {
        PGresult* res = PQexec(conn, query.c_str());
        const auto status = PQresultStatus(res);
        std::string value = (status == PGRES_TUPLES_OK && PQntuples(res) > 0)
                                ? PQgetvalue(res, 0, 0)
                                : "";
        const std::string error = PQresultErrorMessage(res);
        PQclear(res);
        INFO(query << " -> " << error);
        REQUIRE((status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK));
        return value;
    };
    auto insert_signal = [&]() {
        return std::stoll(sql(
            "insert into engine_signal (signal_type, created_by) values "
            "('strategy_command', 'test') returning id"));
    };
    auto status_of = [&](long long id) {
        return sql("select status || ':' || coalesce(detail, '') "
                   "from strategy_command_status where signal_id = " +
                   std::to_string(id));
    };
    constexpr int strategy_id = 999991;
    const auto finished = insert_signal();
    const auto open = insert_signal();
    sql("update engine_signal set payload = '{\"op\": \"open_trade\", "
        "\"strategy_id\": 999991}' where id = " + std::to_string(open));

    Live::StrategyCommandStore store(conninfo);
    CHECK(store.claim(finished, strategy_id, "accepted", ""));
    CHECK_FALSE(store.claim(finished, strategy_id, "accepted", ""));
    CHECK(store.claim(open, strategy_id, "accepted", ""));

    store.apply({{finished, strategy_id, Core::STRATEGY_COMMAND_DONE, ""},
                 {finished, strategy_id, Core::STRATEGY_COMMAND_WORKING, "late"}});
    CHECK(status_of(finished) == "done:");

    store.link_orders({{finished, 9223372036854775901ULL}, {finished, 9223372036854775901ULL}});
    CHECK(sql("select count(*) || ':' || max(order_id) from strategy_command_order "
              "where signal_id = " + std::to_string(finished)) ==
          "1:9223372036854775901");

    const auto resumed = store.open_commands({strategy_id});
    REQUIRE(resumed.size() == 1);
    CHECK(resumed[0].id == open);
    CHECK(resumed[0].op == "open_trade");
    CHECK(resumed[0].resumed);
    CHECK(resumed[0].ts_ms > 0);

    sql("delete from engine_signal where id in (" + std::to_string(finished) +
        ", " + std::to_string(open) + ")");
    PQfinish(conn);
}
