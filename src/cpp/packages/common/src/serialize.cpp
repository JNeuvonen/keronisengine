#include <datetime_utils.h>
#include <serialize.h>

#include <filesystem>

std::string Serialize::escape_pg_literal(PGconn *conn,
                                         const std::string &input) {
    char *escaped = PQescapeLiteral(conn, input.c_str(), input.length());
    if (!escaped) {
        throw std::runtime_error("Error escaping string for PostgreSQL: " +
                                 std::string(PQerrorMessage(conn)));
    }
    std::string result(escaped);
    PQfreemem(escaped);
    return result;
}
