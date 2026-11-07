#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "live_infra/persistent_pg_connection.h"
#include "market_making/dtypes/engine_signal.h"

namespace Live {

// engine_signal access for one engine instance over one connection kept open
// across polls. The engine never sees credentials: it gets fetch and ack
// callbacks that wrap this.
class EngineSignalStore {
   public:
    EngineSignalStore(std::string conninfo, std::string consumer_id)
        : db_(std::move(conninfo), "engine_signal"),
          consumer_id_(std::move(consumer_id)) {}

    const std::string& consumer_id() const { return consumer_id_; }

    std::vector<Core::EngineSignal> fetch_unacked(
        const std::vector<std::string>& signal_types) {
        if (signal_types.empty()) return {};
        std::string types = "{";
        for (std::size_t i = 0; i < signal_types.size(); ++i) {
            if (i > 0) types += ",";
            types += "\"";
            for (const char c : signal_types[i]) {
                if (c == '"' || c == '\\') types += '\\';
                types += c;
            }
            types += "\"";
        }
        types += "}";

        std::lock_guard<std::mutex> lock(mutex_);
        const char* params[] = {types.c_str(), consumer_id_.c_str()};
        const auto res = db_.exec(
            "SELECT s.id, s.signal_type, COALESCE(s.key, ''), s.payload::text "
            "FROM engine_signal s "
            "WHERE s.signal_type = ANY($1::text[]) AND s.expires_at > NOW() "
            "AND (s.target IS NULL OR s.target = $2) "
            "AND NOT EXISTS (SELECT 1 FROM engine_signal_ack a "
            "WHERE a.signal_id = s.id AND a.consumer_id = $2) "
            "ORDER BY s.id LIMIT 500",
            2, params, PGRES_TUPLES_OK);
        std::vector<Core::EngineSignal> signals;
        signals.reserve(PQntuples(res.get()));
        for (int row = 0; row < PQntuples(res.get()); ++row) {
            Core::EngineSignal signal;
            signal.id = std::stoll(PQgetvalue(res.get(), row, 0));
            signal.signal_type = PQgetvalue(res.get(), row, 1);
            signal.key = PQgetvalue(res.get(), row, 2);
            signal.payload = nlohmann::json::parse(PQgetvalue(res.get(), row, 3));
            signals.push_back(std::move(signal));
        }
        return signals;
    }

    void ack(const Core::EngineSignal& signal,
             const Core::EngineSignalOutcome& outcome) {
        const std::string id = std::to_string(signal.id);
        std::lock_guard<std::mutex> lock(mutex_);
        const char* params[] = {id.c_str(), consumer_id_.c_str(),
                                Core::engine_signal_status_name(outcome.status),
                                outcome.error.c_str()};
        db_.exec(
            "INSERT INTO engine_signal_ack (signal_id, consumer_id, status, "
            "error) VALUES ($1::bigint, $2, $3, NULLIF($4, '')) "
            "ON CONFLICT (signal_id, consumer_id) DO UPDATE SET status = "
            "EXCLUDED.status, error = EXCLUDED.error, applied_at = NOW()",
            4, params, PGRES_COMMAND_OK);
    }

   private:
    PersistentPgConnection db_;
    std::string consumer_id_;
    std::mutex mutex_;
};

}  // namespace Live
