#pragma once

#include <libpq-fe.h>

#include <memory>
#include <stdexcept>
#include <string>

namespace Live {

using PgResult = std::unique_ptr<PGresult, decltype(&PQclear)>;

// One libpq connection kept open across calls, reconnected when it drops. Not
// thread-safe: the owner serializes calls.
class PersistentPgConnection {
   public:
    PersistentPgConnection(std::string conninfo, std::string label)
        : conninfo_(std::move(conninfo)), label_(std::move(label)) {}
    ~PersistentPgConnection() {
        if (conn_ != nullptr) PQfinish(conn_);
    }
    PersistentPgConnection(const PersistentPgConnection&) = delete;
    PersistentPgConnection& operator=(const PersistentPgConnection&) = delete;

    // Throws unless the statement ends in `expected`.
    PgResult exec(const char* sql, int n_params, const char* const* params,
                  ExecStatusType expected) {
        PgResult res(
            PQexecParams(connection(), sql, n_params, nullptr, params, nullptr,
                         nullptr, 0),
            &PQclear);
        if (PQresultStatus(res.get()) != expected) {
            const std::string error = PQresultErrorMessage(res.get());
            if (PQstatus(conn_) != CONNECTION_OK) {
                PQfinish(conn_);
                conn_ = nullptr;
            }
            throw std::runtime_error(label_ + " failed: " + error);
        }
        return res;
    }

   private:
    // A stuck DB costs at most the connect and statement timeouts.
    PGconn* connection() {
        if (conn_ != nullptr && PQstatus(conn_) == CONNECTION_OK) return conn_;
        if (conn_ != nullptr) {
            PQfinish(conn_);
            conn_ = nullptr;
        }
        const char* keys[] = {"dbname", "connect_timeout", nullptr};
        const char* values[] = {conninfo_.c_str(), "5", nullptr};
        PGconn* conn = PQconnectdbParams(keys, values, /*expand_dbname=*/1);
        if (PQstatus(conn) != CONNECTION_OK) {
            const std::string error = PQerrorMessage(conn);
            PQfinish(conn);
            throw std::runtime_error(label_ + " connect failed: " + error);
        }
        PQclear(PQexec(conn, "SET statement_timeout = 5000"));
        conn_ = conn;
        return conn_;
    }

    std::string conninfo_;
    std::string label_;
    PGconn* conn_ = nullptr;
};

}  // namespace Live
