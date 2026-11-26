#pragma once

#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "live_infra/persistent_pg_connection.h"
#include "market_making/dtypes/engine_signal.h"
#include "market_making/strategy.h"

namespace Live {

// Which strategies must keep their quotes pulled. A pull is state, not a
// command: the newest live pull_quotes / resume_quotes row per key decides
// that source, so a restarted engine comes back pulled. Never acknowledged.
class QuoteHolds {
   public:
    // The guard heartbeats every 5s.
    static constexpr long long kGuardStaleMs = 15000;
    static constexpr const char* kGuardInstance = "quote_guard";

    struct Row {
        std::string signal_type;
        nlohmann::json payload = nlohmann::json::object();
    };
    struct Snapshot {
        std::vector<Row> latest_per_key;
        std::optional<long long> guard_age_ms;
    };
    struct Pulls {
        bool all = false;
        std::unordered_set<int> strategy_ids;
    };

    static Pulls pulls(const std::vector<Row>& latest_per_key) {
        Pulls out;
        for (const auto& row : latest_per_key) {
            if (row.signal_type != Core::ENGINE_SIGNAL_PULL_QUOTES) continue;
            const auto it = row.payload.find("strategy_id");
            if (it == row.payload.end() || !it->is_number_integer()) {
                out.all = true;
            } else {
                out.strategy_ids.insert(it->get<int>());
            }
        }
        return out;
    }

    // nullptr: the strategy may quote. Otherwise the QuotesPullReason.
    static const char* pull_reason(const Pulls& pulls, int strategy_id,
                                   bool is_paper, bool guard_fresh,
                                   bool shutting_down) {
        if (shutting_down) return Core::QuotesPullReason::SHUTDOWN;
        if (pulls.all || pulls.strategy_ids.count(strategy_id) > 0) {
            return Core::QuotesPullReason::PULL_SIGNAL;
        }
        if (!is_paper && !guard_fresh) return Core::QuotesPullReason::GUARD_STALE;
        return nullptr;
    }

    QuoteHolds(std::string conninfo, std::string engine_instance)
        : db_(std::move(conninfo), "quote_holds"),
          engine_instance_(std::move(engine_instance)) {}

    // Throws when the DB cannot be read.
    Snapshot fetch() {
        Snapshot snap;
        const char* params[] = {engine_instance_.c_str(),
                                Core::ENGINE_SIGNAL_PULL_QUOTES,
                                Core::ENGINE_SIGNAL_RESUME_QUOTES};
        const auto rows = db_.exec(
            "SELECT DISTINCT ON (COALESCE(key, '')) signal_type, payload::text "
            "FROM engine_signal WHERE signal_type IN ($2, $3) "
            "AND expires_at > NOW() AND (target IS NULL OR target = $1) "
            "ORDER BY COALESCE(key, ''), id DESC",
            3, params, PGRES_TUPLES_OK);
        for (int i = 0; i < PQntuples(rows.get()); ++i) {
            snap.latest_per_key.push_back(
                {PQgetvalue(rows.get(), i, 0),
                 nlohmann::json::parse(PQgetvalue(rows.get(), i, 1))});
        }
        const char* guard[] = {kGuardInstance};
        const auto age = db_.exec(
            "SELECT (EXTRACT(EPOCH FROM NOW() - updated_at) * 1000)::bigint "
            "FROM engine_health WHERE engine_instance = $1",
            1, guard, PGRES_TUPLES_OK);
        if (PQntuples(age.get()) == 1 && !PQgetisnull(age.get(), 0, 0)) {
            snap.guard_age_ms = std::stoll(PQgetvalue(age.get(), 0, 0));
        }
        return snap;
    }

   private:
    PersistentPgConnection db_;
    std::string engine_instance_;
};

}  // namespace Live
