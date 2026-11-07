#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "live_infra/persistent_pg_connection.h"
#include "market_making/strategy_commands.h"

namespace Live {

// strategy_command_status: one row per command, created when an engine claims
// it. The row is the delivery ledger: a command is queued only by the engine
// that inserted its row, so a signal redelivered after a crash never runs
// twice.
class StrategyCommandStore {
   public:
    explicit StrategyCommandStore(std::string conninfo)
        : db_(std::move(conninfo), "strategy_command_status") {}

    // False when the command was already claimed.
    bool claim(long long command_id, int strategy_id, const std::string& status,
               const std::string& detail) {
        const std::string id = std::to_string(command_id);
        const std::string sid = std::to_string(strategy_id);
        std::lock_guard<std::mutex> lock(mutex_);
        const char* params[] = {id.c_str(), sid.c_str(), status.c_str(),
                                detail.c_str()};
        const auto res = db_.exec(
            "INSERT INTO strategy_command_status (signal_id, strategy_id, "
            "status, detail) VALUES ($1::bigint, $2::int, $3, NULLIF($4, '')) "
            "ON CONFLICT (signal_id) DO NOTHING RETURNING signal_id",
            4, params, PGRES_TUPLES_OK);
        return PQntuples(res.get()) == 1;
    }

    // A finished command keeps its outcome.
    void apply(const std::vector<Core::StrategyCommandReport>& reports) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& report : reports) {
            const std::string id = std::to_string(report.command_id);
            const char* params[] = {id.c_str(), report.status.c_str(),
                                    report.detail.c_str()};
            db_.exec(
                "UPDATE strategy_command_status SET status = $2, "
                "detail = NULLIF($3, ''), updated_at = NOW() "
                "WHERE signal_id = $1::bigint AND status "
                "NOT IN ('done', 'failed', 'cancelled', 'interrupted')",
                3, params, PGRES_COMMAND_OK);
        }
    }

    void link_orders(const std::vector<Core::StrategyCommandOrder>& links) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& link : links) {
            const std::string id = std::to_string(link.command_id);
            const std::string order = std::to_string(link.order_id);
            const char* params[] = {id.c_str(), order.c_str()};
            db_.exec(
                "INSERT INTO strategy_command_order (signal_id, order_id) "
                "VALUES ($1::bigint, $2::numeric) ON CONFLICT DO NOTHING",
                2, params, PGRES_COMMAND_OK);
        }
    }

    void save_books(const std::vector<std::pair<int, nlohmann::json>>& books) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [strategy_id, book] : books) {
            const std::string sid = std::to_string(strategy_id);
            const std::string state = book.dump();
            const char* params[] = {sid.c_str(), state.c_str()};
            db_.exec(
                "INSERT INTO strategy_book_state (strategy_id, state, updated_at) "
                "VALUES ($1::int, $2::jsonb, NOW()) ON CONFLICT (strategy_id) "
                "DO UPDATE SET state = EXCLUDED.state, updated_at = NOW()",
                2, params, PGRES_COMMAND_OK);
        }
    }

    // Commands of these strategies still open from an earlier engine run,
    // oldest first, stamped with the time they were sent.
    std::vector<Core::StrategyCommand> open_commands(const std::vector<int>& strategy_ids) {
        if (strategy_ids.empty()) return {};
        std::string ids = "{";
        for (std::size_t i = 0; i < strategy_ids.size(); ++i) {
            if (i > 0) ids += ",";
            ids += std::to_string(strategy_ids[i]);
        }
        ids += "}";
        std::lock_guard<std::mutex> lock(mutex_);
        const char* params[] = {ids.c_str()};
        const auto res = db_.exec(
            "SELECT s.id, c.strategy_id, s.payload::text, "
            "(EXTRACT(EPOCH FROM s.created_at) * 1000)::bigint "
            "FROM strategy_command_status c JOIN engine_signal s ON s.id = c.signal_id "
            "WHERE c.strategy_id = ANY($1::int[]) "
            "AND c.status IN ('accepted', 'working') ORDER BY s.id",
            1, params, PGRES_TUPLES_OK);
        std::vector<Core::StrategyCommand> commands;
        for (int row = 0; row < PQntuples(res.get()); ++row) {
            Core::StrategyCommand command;
            command.id = std::stoll(PQgetvalue(res.get(), row, 0));
            command.strategy_id = std::stoi(PQgetvalue(res.get(), row, 1));
            command.payload = nlohmann::json::parse(PQgetvalue(res.get(), row, 2), nullptr, false);
            command.ts_ms = std::stoll(PQgetvalue(res.get(), row, 3));
            command.resumed = true;
            if (!command.payload.is_object() || !command.payload.contains("op") ||
                !command.payload.at("op").is_string()) {
                continue;
            }
            command.op = command.payload.at("op").get<std::string>();
            commands.push_back(std::move(command));
        }
        return commands;
    }

   private:
    PersistentPgConnection db_;
    std::mutex mutex_;
};

}  // namespace Live
