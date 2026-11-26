#include <generic_utils.h>
#include <postgre_utils.h>
#include <serialize.h>

#include <optional>

#include "common_types/log_event_types.h"
#include "common_types/strategy.h"
#include "db_types/dataset.h"
#include "disk_logging/log.h"
#include "market_making/constants.h"
#include "nats_publisher.h"

namespace {
std::string remove_json_null_escapes(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        if (i + 5 < s.size() && s[i] == '\\' && s[i + 1] == 'u' &&
            s[i + 2] == '0' && s[i + 3] == '0' && s[i + 4] == '0' &&
            s[i + 5] == '0') {
            i += 6;
        } else if (s[i] == '\0') {
            ++i;
        } else {
            out += s[i];
            ++i;
        }
    }
    return out;
}

// fetch_from_postgres renders every column as text and NULL as "". A NULL
// number, or a JSON column holding anything but an array (old rows hold {}),
// is left out so the config default applies; a NULL bool or text reads as
// false / "", as the engine has always read them.
void copy_bool(const json &row, json &config, const char *column) {
    if (const auto it = row.find(column); it != row.end()) {
        const auto &value = it->get_ref<const std::string &>();
        config[column] = value == "t" || value == "true" || value == "1";
    }
}

void copy_text(const json &row, json &config, const char *column) {
    if (const auto it = row.find(column); it != row.end()) {
        config[column] = it->get<std::string>();
    }
}

void copy_integer(const json &row, json &config, const char *column) {
    if (const auto it = row.find(column); it != row.end()) {
        const auto &value = it->get_ref<const std::string &>();
        if (!value.empty()) config[column] = std::stoll(value);
    }
}

void copy_double(const json &row, json &config, const char *column) {
    if (const auto it = row.find(column); it != row.end()) {
        const auto &value = it->get_ref<const std::string &>();
        if (!value.empty()) config[column] = std::stod(value);
    }
}

void copy_string_array(const json &row, json &config, const char *column) {
    if (const auto it = row.find(column); it != row.end()) {
        const auto &value = it->get_ref<const std::string &>();
        if (value.empty()) return;
        json parsed = json::parse(value);
        if (parsed.is_array()) config[column] = std::move(parsed);
    }
}

json ticker_row_to_config(const json &row) {
    json config = json::object();
    copy_integer(row, config, "id");
    for (const char *column :
         {"symbol", "base_asset", "quote_asset", "slug", "condition_id"}) {
        copy_text(row, config, column);
    }
    for (const char *column : {"is_active", "is_closed", "is_accepting_orders",
                               "is_order_book_enabled", "is_neg_risk"}) {
        copy_bool(row, config, column);
    }
    for (const char *column : {"order_price_min_tick_size", "order_min_size"}) {
        copy_double(row, config, column);
    }
    for (const char *column :
         {"event_start_time_ms", "event_end_time_ms",
          "accepting_orders_timestamp_ms", "num_outcomes"}) {
        copy_integer(row, config, column);
    }
    for (const char *column : {"outcome_names", "clob_token_ids"}) {
        copy_string_array(row, config, column);
    }
    return config;
}
}  // namespace

json PQUtils::strategy_rows_to_config(const json &strategy_row,
                                      const std::vector<json> &ticker_rows,
                                      const std::vector<json> &model_rows) {
    const std::string id = strategy_row.value("id", std::string{});
    const std::string format =
        strategy_row.value("backtest_json_format_version", std::string{});
    if (format != "3.0") {
        throw std::runtime_error("strategy " + id +
                                 " has backtest_json_format_version '" +
                                 format + "'; only 3.0 is supported");
    }

    json config = json::object();
    for (const char *column :
         {"id", "candle_interval_ms", "num_max_orders_10_sec",
          "num_max_orders_1_min", "num_max_orders_1_hour"}) {
        copy_integer(strategy_row, config, column);
    }
    for (const char *column :
         {"is_disabled", "is_using_all_available_symbols",
          "is_using_percentage_max_position", "is_using_nominal_max_position",
          "is_in_reduce_only", "uses_hedges", "allow_co_strategy_compounding",
          "needs_to_run_realtime", "is_trading_on_prediction_market",
          "is_market_making_strategy", "is_paper_trading",
          "discretionary_multi_account"}) {
        copy_bool(strategy_row, config, column);
    }
    for (const char *column :
         {"percentage_max_position", "nominal_max_position",
          "max_directional_exposure", "max_sizing_per_symbol",
          "virtual_balance"}) {
        copy_double(strategy_row, config, column);
    }
    for (const char *column :
         {"strategy_name", "strategy_type", "account_mode", "series_slug",
          "main_exchange", "model_schedule_path"}) {
        copy_text(strategy_row, config, column);
    }
    copy_string_array(strategy_row, config, "series_slugs");

    // exchanges_list holds a JSON string whose content is the JSON array.
    // Old rows carry keys the engine never read, such as "symbols".
    if (const auto it = strategy_row.find("exchanges_list");
        it != strategy_row.end() && !it->get<std::string>().empty()) {
        json exchanges = json::parse(it->get<std::string>());
        if (exchanges.is_string()) {
            exchanges = json::parse(exchanges.get<std::string>());
        }
        config["exchanges_list"] = json::array();
        for (const auto &exchange : exchanges) {
            json entry = json::object();
            for (const char *key : {"exchange_name", "market_type", "account_id",
                                    "paper_fill_latency_ms"}) {
                if (exchange.contains(key)) entry[key] = exchange.at(key);
            }
            config["exchanges_list"].push_back(entry);
        }
    }

    config["trade_tickers"] = json::array();
    for (const auto &row : ticker_rows) {
        config["trade_tickers"].push_back(ticker_row_to_config(row));
    }
    config["models"] = json::array();
    for (const auto &row : model_rows) {
        json model = json::object();
        copy_text(row, model, "name");
        copy_text(row, model, "path_to_model");
        config["models"].push_back(model);
    }
    return config;
}

std::vector<json> PQUtils::fetch_strategy_configs(
    const char *conninfo, const std::string &version,
    std::optional<int> strategy_id) {
    const std::string query =
        "SELECT * FROM strategy WHERE version = '" + version + "'" +
        (strategy_id ? " AND id = " + std::to_string(*strategy_id) : "") + ";";
    std::vector<json> configs;
    for (const auto &row :
         PQUtils::fetch_from_postgres(conninfo, query.c_str())) {
        const std::string id = row.at("id").get<std::string>();
        const std::string tickers_query =
            "SELECT * FROM trade_ticker where strategy_id = " + id + ";";
        const std::string models_query =
            "SELECT m.name, m.path_to_model FROM strategy_model sm JOIN model "
            "m ON sm.model_id = m.id WHERE sm.strategy_id = " +
            id + ";";
        configs.push_back(PQUtils::strategy_rows_to_config(
            row, PQUtils::fetch_from_postgres(conninfo, tickers_query.c_str()),
            PQUtils::fetch_from_postgres(conninfo, models_query.c_str())));
    }
    return configs;
}

std::vector<StrategyConfig> PQUtils::fetch_strategies(
    const char *conninfo, const std::string &version,
    std::optional<int> strategy_id) {
    std::vector<StrategyConfig> strategies;
    for (const auto &config :
         PQUtils::fetch_strategy_configs(conninfo, version, strategy_id)) {
        strategies.push_back(StrategyConfig::from_json(config));
    }
    return strategies;
}

std::vector<json> PQUtils::fetch_from_postgres(const char *conninfo,
                                               const char *query) {
    std::vector<json> result_vector;

    PGconn *conn = PQconnectdb(conninfo);
    if (PQstatus(conn) != CONNECTION_OK) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_DB_CONNECT_FAILED,
                  "db connect failed",
                  (nlohmann::json{{"site", "fetch_from_postgres"},
                                  {"pq_error", PQerrorMessage(conn)}}));
        PQfinish(conn);
        return result_vector;
    }

    PGresult *res = PQexec(conn, query);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        PQclear(res);
        PQfinish(conn);
        return result_vector;
    }

    int nrows = PQntuples(res);
    int nfields = PQnfields(res);

    for (int i = 0; i < nrows; i++) {
        json row;
        for (int j = 0; j < nfields; j++) {
            std::string col_name = PQfname(res, j);
            const char *value = PQgetvalue(res, i, j);

            // Store everything as strings
            row[col_name] = value ? std::string(value) : "";
        }
        result_vector.push_back(row);
    }

    PQclear(res);
    PQfinish(conn);

    return result_vector;
}

bool PQUtils::write_to_postgres(const char *conninfo,
                                const std::string &table_name,
                                const std::vector<json> &entries,
                                bool print_errors) {
    if (entries.empty()) {
        return true;
    }

    PGconn *conn = PQconnectdb(conninfo);
    if (PQstatus(conn) != CONNECTION_OK) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_DB_CONNECT_FAILED,
                  std::string("Connection to database failed: ") +
                      PQerrorMessage(conn),
                  (nlohmann::json{{"pq_error", PQerrorMessage(conn)}}));
        PQfinish(conn);
        return false;
    }

    std::string query = "INSERT INTO " + table_name + " (";

    const json &first_entry = entries[0];
    std::vector<std::string> columns;
    for (auto it = first_entry.begin(); it != first_entry.end(); ++it) {
        columns.push_back(it.key());
    }

    for (size_t i = 0; i < columns.size(); ++i) {
        query += columns[i];
        if (i < columns.size() - 1) {
            query += ", ";
        }
    }
    query += ") VALUES ";

    for (size_t i = 0; i < entries.size(); ++i) {
        const json &entry = entries[i];
        query += "(";

        for (size_t j = 0; j < columns.size(); ++j) {
            const std::string &col = columns[j];
            std::string value;

            if (entry.contains(col)) {
                if (entry[col].is_string()) {
                    const char *escaped_value = PQescapeLiteral(
                        conn, entry[col].get<std::string>().c_str(),
                        entry[col].get<std::string>().size());
                    if (escaped_value) {
                        value = std::string(escaped_value);
                        PQfreemem((void *)escaped_value);
                    } else {
                        LOG_WARNING(
                            Core::TRADING_LOG_EVENT_TYPE_DB_WRITE_FAILED,
                            std::string("Error escaping value for column: ") +
                                col,
                            (nlohmann::json{{"column", col}}));
                        value = "NULL";
                    }
                } else if (entry[col].is_boolean()) {
                    value = entry[col].get<bool>() ? "true" : "false";
                } else if (entry[col].is_number()) {
                    value = entry[col].dump();
                } else {
                    value = "NULL";
                }
            } else {
                value = "NULL";
            }

            query += value;

            if (j < columns.size() - 1) {
                query += ", ";
            }
        }
        query += ")";

        if (i < entries.size() - 1) {
            query += ", ";
        }
    }
    query += ";";

    PGresult *res = PQexec(conn, query.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        if (print_errors) {
            LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_DB_WRITE_FAILED,
                      std::string("Query execution failed: ") +
                          PQresultErrorMessage(res),
                      (nlohmann::json{{"pq_error", PQresultErrorMessage(res)},
                                      {"failed_query", query}}));
        }
        PQclear(res);
        PQfinish(conn);
        return false;
    }

    PQclear(res);
    PQfinish(conn);
    return true;
}


bool PQUtils::batch_update_entries(const char *conninfo,
                                   const std::string &table_name,
                                   const std::vector<json> &entries,
                                   const std::string &id_column) {
    if (entries.empty()) {
        return true;
    }

    PGconn *conn = PQconnectdb(conninfo);
    if (PQstatus(conn) != CONNECTION_OK) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_DB_CONNECT_FAILED,
                  std::string("Connection to database failed: ") +
                      PQerrorMessage(conn),
                  (nlohmann::json{{"pq_error", PQerrorMessage(conn)}}));
        PQfinish(conn);
        return false;
    }

    const json &first_entry = entries[0];
    std::vector<std::string> columns;
    for (auto it = first_entry.begin(); it != first_entry.end(); ++it) {
        if (it.key() != id_column) {
            columns.push_back(it.key());
        }
    }

    if (columns.empty()) {
        PQfinish(conn);
        return false;
    }

    std::string query = "UPDATE " + table_name + " SET ";
    std::vector<std::string> case_clauses;

    for (const auto &col : columns) {
        std::string case_clause = col + " = CASE ";

        for (const auto &entry : entries) {
            if (!entry.contains(id_column)) {
                LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_DB_WRITE_FAILED,
                          std::string("Entry missing id_column: ") + id_column,
                          (nlohmann::json{{"id_column", id_column}}));
                PQfinish(conn);
                return false;
            }

            std::string id_value;
            if (entry[id_column].is_string()) {
                const char *escaped_id = PQescapeLiteral(
                    conn, entry[id_column].get<std::string>().c_str(),
                    entry[id_column].get<std::string>().size());
                if (escaped_id) {
                    id_value = std::string(escaped_id);
                    PQfreemem((void *)escaped_id);
                } else {
                    id_value = "NULL";
                }
            } else if (entry[id_column].is_number()) {
                id_value = entry[id_column].dump();
            } else {
                id_value = entry[id_column].dump();
            }

            case_clause += "WHEN " + id_column + " = " + id_value + " THEN ";

            if (entry.contains(col)) {
                std::string value;
                if (entry[col].is_string()) {
                    const char *escaped_value = PQescapeLiteral(
                        conn, entry[col].get<std::string>().c_str(),
                        entry[col].get<std::string>().size());
                    if (escaped_value) {
                        value = std::string(escaped_value);
                        PQfreemem((void *)escaped_value);
                    } else {
                        value = "NULL";
                    }
                } else if (entry[col].is_boolean()) {
                    value = entry[col].get<bool>() ? "true" : "false";
                } else if (entry[col].is_number()) {
                    value = entry[col].dump();
                } else if (entry[col].is_null()) {
                    value = "NULL";
                } else {
                    value = "NULL";
                }
                case_clause += value + " ";
            } else {
                case_clause += col + " ";
            }
        }

        case_clause += "ELSE " + col + " END";
        case_clauses.push_back(case_clause);
    }

    query += CommonUtils::join(case_clauses, ", ");

    query += " WHERE " + id_column + " IN (";
    std::vector<std::string> id_values;
    for (const auto &entry : entries) {
        if (entry.contains(id_column)) {
            std::string id_value;
            if (entry[id_column].is_string()) {
                const char *escaped_id = PQescapeLiteral(
                    conn, entry[id_column].get<std::string>().c_str(),
                    entry[id_column].get<std::string>().size());
                if (escaped_id) {
                    id_value = std::string(escaped_id);
                    PQfreemem((void *)escaped_id);
                } else {
                    id_value = "NULL";
                }
            } else if (entry[id_column].is_number()) {
                id_value = entry[id_column].dump();
            } else {
                id_value = entry[id_column].dump();
            }
            id_values.push_back(id_value);
        }
    }
    query += CommonUtils::join(id_values, ", ") + ");";

    PGresult *res = PQexec(conn, query.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_DB_WRITE_FAILED,
                  std::string("Batch update failed: ") +
                      PQresultErrorMessage(res),
                  (nlohmann::json{{"pq_error", PQresultErrorMessage(res)},
                                  {"failed_query", query}}));
        PQclear(res);
        PQfinish(conn);
        return false;
    }

    PQclear(res);
    PQfinish(conn);
    return true;
}


void PQUtils::update_trade_tickers_processed_kline(
    const char *conninfo, const std::vector<std::pair<int, double>> &ticker_ids,
    long long processed_kline) {
    if (ticker_ids.empty()) {
        return;
    }

    PGconn *conn = PQconnectdb(conninfo);
    if (PQstatus(conn) != CONNECTION_OK) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_DB_CONNECT_FAILED,
                  std::string("Connection to database failed: ") +
                      PQerrorMessage(conn),
                  (nlohmann::json{{"pq_error", PQerrorMessage(conn)}}));
        PQfinish(conn);
        return;
    }

    // Build CASE statement for prediction updates
    std::string prediction_case = "prediction = CASE ";
    for (size_t i = 0; i < ticker_ids.size(); i++) {
        prediction_case += "WHEN id = $" + std::to_string(i + 2) + " THEN $" +
                           std::to_string(i + ticker_ids.size() + 2) + " ";
    }
    prediction_case += "ELSE prediction END";

    std::string query =
        "UPDATE trade_ticker SET processed_kline_open_time_ms = $1, " +
        prediction_case + " WHERE id IN (";

    std::vector<std::string> id_placeholders;
    for (size_t i = 0; i < ticker_ids.size(); ++i) {
        id_placeholders.push_back("$" + std::to_string(i + 2));
    }
    query += CommonUtils::join(id_placeholders, ",") + ");";

    std::vector<std::string> paramStrings;
    paramStrings.push_back(std::to_string(processed_kline));

    // Add IDs first
    for (const auto &pair : ticker_ids) {
        paramStrings.push_back(std::to_string(pair.first));
    }
    // Add predictions next
    for (const auto &pair : ticker_ids) {
        paramStrings.push_back(std::to_string(pair.second));
    }

    std::vector<const char *> paramValues;
    for (const auto &str : paramStrings) {
        paramValues.push_back(str.c_str());
    }

    int paramCount = paramValues.size();
    int paramFormats[paramCount];
    memset(paramFormats, 0, sizeof(paramFormats));

    PGresult *res = PQexecParams(conn, query.c_str(), paramCount, nullptr,
                                 paramValues.data(), nullptr, paramFormats, 0);

    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_DB_WRITE_FAILED,
                  std::string("Batch update failed: ") +
                      PQresultErrorMessage(res),
                  (nlohmann::json{{"pq_error", PQresultErrorMessage(res)},
                                  {"failed_query", query}}));
    }

    PQclear(res);
    PQfinish(conn);
}


std::optional<long> PQUtils::create_entry_return_id(
    const char *conninfo, const std::string &table_name, const json &entry,
    const std::string &id_column) {
    if (entry.empty()) {
        return std::nullopt;
    }

    PGconn *conn = PQconnectdb(conninfo);
    if (PQstatus(conn) != CONNECTION_OK) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_DB_CONNECT_FAILED,
                  std::string("Connection to database failed: ") +
                      PQerrorMessage(conn),
                  (nlohmann::json{{"pq_error", PQerrorMessage(conn)}}));
        PQfinish(conn);
        return std::nullopt;
    }

    std::string query = "INSERT INTO " + table_name + " (";
    std::vector<std::string> columns;
    std::vector<std::string> values;

    for (auto it = entry.begin(); it != entry.end(); ++it) {
        columns.push_back(it.key());
        std::string value;

        if (it.value().is_string()) {
            const char *escaped_value =
                PQescapeLiteral(conn, it.value().get<std::string>().c_str(),
                                it.value().get<std::string>().size());
            if (escaped_value) {
                value = std::string(escaped_value);
                PQfreemem((void *)escaped_value);
            } else {
                LOG_WARNING(Core::TRADING_LOG_EVENT_TYPE_DB_WRITE_FAILED,
                            std::string("Error escaping value for column: ") +
                                it.key(),
                            (nlohmann::json{{"column", it.key()}}));
                value = "NULL";
            }
        } else if (it.value().is_boolean()) {
            value = it.value().get<bool>() ? "true" : "false";
        } else if (it.value().is_number()) {
            value = it.value().dump();
        } else {
            value = "NULL";
        }

        values.push_back(value);
    }

    query += CommonUtils::join(columns, ", ") + ") VALUES (" +
             CommonUtils::join(values, ", ") + ") RETURNING " + id_column + ";";

    PGresult *res = PQexec(conn, query.c_str());

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_DB_WRITE_FAILED,
                  std::string("Query execution failed: ") +
                      PQresultErrorMessage(res),
                  (nlohmann::json{{"pq_error", PQresultErrorMessage(res)},
                                  {"failed_query", query}}));
        PQclear(res);
        PQfinish(conn);
        return std::nullopt;
    }

    int returned_id = std::stoi(PQgetvalue(res, 0, 0));

    PQclear(res);
    PQfinish(conn);
    return returned_id;
}


std::vector<Db::Dataset> PQUtils::fetch_datasets(const char *conninfo,
                                                 bool fetch_only_ohlcv) {
    std::string query;

    if (fetch_only_ohlcv) {
        query =
            "SELECT * FROM dataset where is_ohlcv_dataset = true and version = "
            "1;";
    } else {
        query = "SELECT * FROM dataset where version = 1;";
    }
    std::vector<json> table_data =
        PQUtils::fetch_from_postgres(conninfo, query.c_str());
    std::vector<Db::Dataset> datasets;

    for (const auto &item : table_data) {
        Db::Dataset dataset;

        dataset.id = Serialize::get_value_from_json<int>(item, "id");
        dataset.relative_path =
            Serialize::get_value_from_json<std::string>(item, "relative_path");
        dataset.dataset_type =
            Serialize::get_value_from_json<std::string>(item, "dataset_type");

        dataset.is_mexc_dataset = Serialize::get_value_from_json<bool>(
            item, "is_mexc_dataset", false);
        dataset.is_binance_dataset = Serialize::get_value_from_json<bool>(
            item, "is_binance_dataset", false);
        dataset.is_crypto_dataset = Serialize::get_value_from_json<bool>(
            item, "is_crypto_dataset", false);
        dataset.is_alternative_dataset = Serialize::get_value_from_json<bool>(
            item, "is_alternative_dataset", false);
        dataset.is_solana_defi_dataset = Serialize::get_value_from_json<bool>(
            item, "is_solana_defi_dataset", false);
        dataset.is_concatenated_dataset = Serialize::get_value_from_json<bool>(
            item, "is_concatenated_dataset", false);
        dataset.is_yfinance_dataset = Serialize::get_value_from_json<bool>(
            item, "is_yfinance_dataset", false);
        dataset.is_stock_market_dataset = dataset.is_yfinance_dataset;

        // Parse dates
        if (item.contains("start_date") && !item["start_date"].is_null()) {
            dataset.start_date = Serialize::parse_timestamp(item["start_date"]);
        }
        if (item.contains("end_date") && !item["end_date"].is_null()) {
            dataset.end_date = Serialize::parse_timestamp(item["end_date"]);
        }

        // Parse metadata fields
        dataset.num_rows =
            Serialize::get_value_from_json<int>(item, "num_rows", 0);
        dataset.interval =
            Serialize::get_value_from_json<std::string>(item, "interval");

        // Parse column names
        dataset.price_column =
            Serialize::get_value_from_json<std::string>(item, "price_column");
        dataset.open_price_column = Serialize::get_value_from_json<std::string>(
            item, "open_price_column");
        dataset.high_price_column = Serialize::get_value_from_json<std::string>(
            item, "high_price_column");
        dataset.low_price_column = Serialize::get_value_from_json<std::string>(
            item, "low_price_column");
        dataset.close_price_column =
            Serialize::get_value_from_json<std::string>(item,
                                                        "close_price_column");
        dataset.timeseries_column = Serialize::get_value_from_json<std::string>(
            item, "timeseries_column");
        dataset.target_column =
            Serialize::get_value_from_json<std::string>(item, "target_column");
        dataset.volume_column =
            Serialize::get_value_from_json<std::string>(item, "volume_column");
        dataset.funding_column =
            Serialize::get_value_from_json<std::string>(item, "funding_column");
        dataset.funding_time_column =
            Serialize::get_value_from_json<std::string>(item,
                                                        "funding_time_column");

        // Parse asset information
        dataset.symbol =
            Serialize::get_value_from_json<std::string>(item, "symbol");
        dataset.quote_asset =
            Serialize::get_value_from_json<std::string>(item, "quote_asset");
        dataset.base_asset =
            Serialize::get_value_from_json<std::string>(item, "base_asset");

        // Parse timestamps
        dataset.created_at = Serialize::parse_timestamp(item["created_at"]);
        dataset.updated_at = Serialize::parse_timestamp(item["updated_at"]);

        datasets.push_back(dataset);
    }

    return datasets;
}

long long PQUtils::write_to_postgres_get_id(const char *conninfo,
                                            const std::string &table_name,
                                            const json &entry) {
    PGconn *conn = PQconnectdb(conninfo);
    if (PQstatus(conn) != CONNECTION_OK) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_DB_CONNECT_FAILED,
                  std::string("Connection to database failed: ") +
                      PQerrorMessage(conn),
                  (nlohmann::json{{"pq_error", PQerrorMessage(conn)}}));
        PQfinish(conn);
        return -1;
    }

    std::string query = "INSERT INTO " + table_name + " (";
    std::vector<std::string> columns;
    for (auto it = entry.begin(); it != entry.end(); ++it) {
        columns.push_back(it.key());
    }

    for (size_t i = 0; i < columns.size(); ++i) {
        query += columns[i];
        if (i < columns.size() - 1) {
            query += ", ";
        }
    }
    query += ") VALUES (";

    for (size_t j = 0; j < columns.size(); ++j) {
        const std::string &col = columns[j];
        std::string value;

        if (entry.contains(col)) {
            if (entry[col].is_string()) {
                const char *escaped_value =
                    PQescapeLiteral(conn, entry[col].get<std::string>().c_str(),
                                    entry[col].get<std::string>().size());
                if (escaped_value) {
                    value = std::string(escaped_value);
                    PQfreemem((void *)escaped_value);
                } else {
                    LOG_WARNING(
                        Core::TRADING_LOG_EVENT_TYPE_DB_WRITE_FAILED,
                        std::string("Error escaping value for column: ") + col,
                        (nlohmann::json{{"column", col}}));
                    value = "NULL";
                }
            } else if (entry[col].is_boolean()) {
                value = entry[col].get<bool>() ? "true" : "false";
            } else if (entry[col].is_number()) {
                value = entry[col].dump();
            } else {
                value = "NULL";
            }
        } else {
            value = "NULL";
        }

        query += value;
        if (j < columns.size() - 1) {
            query += ", ";
        }
    }
    query += ") RETURNING id;";

    PGresult *res = PQexec(conn, query.c_str());
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_DB_WRITE_FAILED,
                  std::string("Query execution failed: ") +
                      PQresultErrorMessage(res),
                  (nlohmann::json{{"pq_error", PQresultErrorMessage(res)},
                                  {"failed_query", query}}));
        PQclear(res);
        PQfinish(conn);
        return -1;
    }

    long long id = -1;
    if (PQntuples(res) > 0) {
        id = std::stoll(PQgetvalue(res, 0, 0));
    }

    PQclear(res);
    PQfinish(conn);
    return id;
}

bool PQUtils::delete_by_id(const char *conninfo, const std::string &table_name,
                           int id) {
    PGconn *conn = PQconnectdb(conninfo);
    if (PQstatus(conn) != CONNECTION_OK) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_DB_CONNECT_FAILED,
                  std::string("Connection to database failed: ") +
                      PQerrorMessage(conn),
                  (nlohmann::json{{"pq_error", PQerrorMessage(conn)}}));
        PQfinish(conn);
        return false;
    }

    std::string query = "DELETE FROM " + table_name + " WHERE id = $1;";

    const char *paramValues[1];
    std::string id_str = std::to_string(id);
    paramValues[0] = id_str.c_str();

    PGresult *res = PQexecParams(conn, query.c_str(), 1, nullptr, paramValues,
                                 nullptr, nullptr, 0);

    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_DB_WRITE_FAILED,
                  std::string("Delete operation failed: ") +
                      PQresultErrorMessage(res),
                  (nlohmann::json{{"pq_error", PQresultErrorMessage(res)},
                                  {"failed_query", query}}));
        PQclear(res);
        PQfinish(conn);
        return false;
    }

    PQclear(res);
    PQfinish(conn);
    return true;
}

bool PQUtils::delete_multiple_by_id(const char *conninfo,
                                    const std::string &table_name,
                                    const std::vector<int> &ids) {
    if (ids.empty()) {
        return false;
    }

    PGconn *conn = PQconnectdb(conninfo);
    if (PQstatus(conn) != CONNECTION_OK) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_DB_CONNECT_FAILED,
                  std::string("Connection to database failed: ") +
                      PQerrorMessage(conn),
                  (nlohmann::json{{"pq_error", PQerrorMessage(conn)}}));
        PQfinish(conn);
        return false;
    }

    std::string query = "DELETE FROM " + table_name + " WHERE id IN (";
    std::vector<std::string> id_placeholders;
    std::vector<const char *> paramValues;
    std::vector<std::string> id_strings;

    for (size_t i = 0; i < ids.size(); ++i) {
        id_strings.push_back(std::to_string(ids[i]));
        id_placeholders.push_back("$" + std::to_string(i + 1));
    }

    query += CommonUtils::join(id_placeholders, ", ") + ");";

    for (const auto &str : id_strings) {
        paramValues.push_back(str.c_str());
    }

    PGresult *res =
        PQexecParams(conn, query.c_str(), paramValues.size(), nullptr,
                     paramValues.data(), nullptr, nullptr, 0);

    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_DB_WRITE_FAILED,
                  std::string("Batch delete operation failed: ") +
                      PQresultErrorMessage(res),
                  (nlohmann::json{{"pq_error", PQresultErrorMessage(res)},
                                  {"failed_query", query}}));
        PQclear(res);
        PQfinish(conn);
        return false;
    }

    PQclear(res);
    PQfinish(conn);
    return true;
}

std::optional<std::string> PQUtils::fetch_limit_order_filled_qty_by_id(
    const char *conninfo, long long order_id, int active_trade_id) {
    std::string query =
        "SELECT filled_qty FROM active_trade_limit_order WHERE order_id = " +
        std::to_string(order_id) +
        " AND active_trade_id = " + std::to_string(active_trade_id) + ";";

    std::vector<json> table_data =
        PQUtils::fetch_from_postgres(conninfo, query.c_str());

    if (table_data.empty()) {
        return std::nullopt;
    }

    const json &item = table_data[0];
    return Serialize::get_value_from_json<std::string>(item, "filled_qty",
                                                       "0.0");
}

bool PQUtils::update_entry(const char *conninfo, const std::string &table_name,
                           const json &updates, int id) {
    if (updates.empty()) {
        return false;
    }

    PGconn *conn = PQconnectdb(conninfo);
    if (PQstatus(conn) != CONNECTION_OK) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_DB_CONNECT_FAILED,
                  std::string("Connection to database failed: ") +
                      PQerrorMessage(conn),
                  (nlohmann::json{{"pq_error", PQerrorMessage(conn)}}));
        PQfinish(conn);
        return false;
    }

    std::string query = "UPDATE " + table_name + " SET ";
    std::vector<std::string> set_clauses;

    for (auto it = updates.begin(); it != updates.end(); ++it) {
        std::string value;
        if (it.value().is_string()) {
            const char *escaped_value =
                PQescapeLiteral(conn, it.value().get<std::string>().c_str(),
                                it.value().get<std::string>().size());
            if (escaped_value) {
                value = std::string(escaped_value);
                PQfreemem((void *)escaped_value);
            } else {
                value = "NULL";
            }
        } else if (it.value().is_boolean()) {
            value = it.value().get<bool>() ? "true" : "false";
        } else if (it.value().is_number()) {
            value = it.value().dump();
        } else if (it.value().is_object() || it.value().is_array()) {
            const char *escaped_value = PQescapeLiteral(
                conn, it.value().dump().c_str(), it.value().dump().size());
            if (escaped_value) {
                value = std::string(escaped_value);
                PQfreemem((void *)escaped_value);
            } else {
                value = "NULL";
            }
        } else if (it.value().is_null()) {
            value = "NULL";
        } else {
            value = "NULL";
        }

        set_clauses.push_back(it.key() + " = " + value);
    }

    query += CommonUtils::join(set_clauses, ", ");
    query += " WHERE id = " + std::to_string(id) + ";";

    PGresult *res = PQexec(conn, query.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_DB_WRITE_FAILED,
                  std::string("Update failed: ") + PQresultErrorMessage(res),
                  (nlohmann::json{{"pq_error", PQresultErrorMessage(res)},
                                  {"failed_query", query}}));
        PQclear(res);
        PQfinish(conn);
        return false;
    }

    PQclear(res);
    PQfinish(conn);
    return true;
}

bool PQUtils::update_entry_by_field(const char *conninfo,
                                    const std::string &table_name,
                                    const json &updates,
                                    const std::string &field_name,
                                    const std::string &field_value) {
    if (updates.empty()) {
        return false;
    }

    PGconn *conn = PQconnectdb(conninfo);
    if (PQstatus(conn) != CONNECTION_OK) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_DB_CONNECT_FAILED,
                  std::string("Connection to database failed: ") +
                      PQerrorMessage(conn),
                  (nlohmann::json{{"pq_error", PQerrorMessage(conn)}}));
        PQfinish(conn);
        return false;
    }

    std::string query = "UPDATE " + table_name + " SET ";
    std::vector<std::string> set_clauses;

    for (auto it = updates.begin(); it != updates.end(); ++it) {
        std::string value;
        if (it.value().is_string()) {
            const char *escaped_value =
                PQescapeLiteral(conn, it.value().get<std::string>().c_str(),
                                it.value().get<std::string>().size());
            if (escaped_value) {
                value = std::string(escaped_value);
                PQfreemem((void *)escaped_value);
            } else {
                value = "NULL";
            }
        } else if (it.value().is_boolean()) {
            value = it.value().get<bool>() ? "true" : "false";
        } else if (it.value().is_number()) {
            value = it.value().dump();
        } else if (it.value().is_object() || it.value().is_array()) {
            const char *escaped_value = PQescapeLiteral(
                conn, it.value().dump().c_str(), it.value().dump().size());
            if (escaped_value) {
                value = std::string(escaped_value);
                PQfreemem((void *)escaped_value);
            } else {
                value = "NULL";
            }
        } else if (it.value().is_null()) {
            value = "NULL";
        } else {
            value = "NULL";
        }

        set_clauses.push_back(it.key() + " = " + value);
    }

    const char *escaped_field_value =
        PQescapeLiteral(conn, field_value.c_str(), field_value.size());
    std::string where_clause;
    if (escaped_field_value) {
        where_clause = field_name + " = " + std::string(escaped_field_value);
        PQfreemem((void *)escaped_field_value);
    } else {
        PQfinish(conn);
        return false;
    }

    query += CommonUtils::join(set_clauses, ", ");
    query += " WHERE " + where_clause + ";";

    PGresult *res = PQexec(conn, query.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_DB_WRITE_FAILED,
                  std::string("Update failed: ") + PQresultErrorMessage(res),
                  (nlohmann::json{{"pq_error", PQresultErrorMessage(res)},
                                  {"failed_query", query}}));
        PQclear(res);
        PQfinish(conn);
        return false;
    }

    PQclear(res);
    PQfinish(conn);
    return true;
}



bool PQUtils::write_trading_log(const char *conninfo, const TradingLog &log) {
    std::unique_ptr<PGconn, decltype(&PQfinish)> conn(PQconnectdb(conninfo),
                                                      &PQfinish);

    if (PQstatus(conn.get()) != CONNECTION_OK) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_DB_CONNECT_FAILED,
                  std::string("Connection to database failed: ") +
                      PQerrorMessage(conn.get()),
                  (nlohmann::json{{"pq_error", PQerrorMessage(conn.get())}}));
        return false;
    }

    const char *query =
        "INSERT INTO trading_log "
        "(symbol, strategy_id, log_level, message, event_type, metadata, "
        "source, event_link_id, timestamp) "
        "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)";

    // Convert values to strings for PostgreSQL
    std::string strategy_id_str = std::to_string(log.strategy_id);
    std::string event_link_id_str = std::to_string(log.event_link_id);
    std::string metadata_str = log.metadata.dump();
    std::string timestamp_str = std::to_string(log.timestamp);

    const char *paramValues[9] = {
        log.symbol.c_str(),  strategy_id_str.c_str(),   log.log_level.c_str(),
        log.message.c_str(), log.event_type.c_str(),    metadata_str.c_str(),
        log.source.c_str(),  event_link_id_str.c_str(), timestamp_str.c_str()};

    int paramLengths[9] = {static_cast<int>(log.symbol.length()),
                           static_cast<int>(strategy_id_str.length()),
                           static_cast<int>(log.log_level.length()),
                           static_cast<int>(log.message.length()),
                           static_cast<int>(log.event_type.length()),
                           static_cast<int>(metadata_str.length()),
                           static_cast<int>(log.source.length()),
                           static_cast<int>(event_link_id_str.length()),
                           static_cast<int>(timestamp_str.length())};

    int paramFormats[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};

    PGresult *res = PQexecParams(conn.get(), query, 9, nullptr, paramValues,
                                 paramLengths, paramFormats, 0);

    bool success = PQresultStatus(res) == PGRES_COMMAND_OK;

    if (!success) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_DB_WRITE_FAILED,
                  std::string("Failed to insert trading log: ") +
                      PQerrorMessage(conn.get()),
                  (nlohmann::json{{"pq_error", PQerrorMessage(conn.get())},
                                  {"target_table", "trading_activity_log"}}));
    }

    PQclear(res);
    return success;
}

bool PQUtils::write_trading_logs(const char *conninfo,
                                 const std::vector<TradingLog> &logs,
                                 long long program_start_time_ms) {
    if (logs.empty()) return true;

    std::unique_ptr<PGconn, decltype(&PQfinish)> conn(PQconnectdb(conninfo),
                                                      &PQfinish);
    if (PQstatus(conn.get()) != CONNECTION_OK) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_DB_CONNECT_FAILED,
                  std::string("Connection to database failed: ") +
                      PQerrorMessage(conn.get()),
                  (nlohmann::json{{"pq_error", PQerrorMessage(conn.get())}}));
        return false;
    }

    auto escape = [](const std::string &s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            switch (c) {
                case '\0':
                    break;
                case '\\':
                    out += "\\\\";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                default:
                    out += c;
            }
        }
        return out;
    };

    struct SignalData {
        int32_t strategy_id;
        int64_t timestamp;
        double signal;
        double price;
        std::string symbol;
        double sizing_coeff;
    };
    std::vector<SignalData> signals;
    signals.reserve(logs.size());

    std::vector<TradingLog> trading_activity_logs;
    for (const auto &log : logs) {
        if (log.is_trading_activity_log) {
            trading_activity_logs.push_back(log);
        }
    }

    NatsPublisher::getInstance().publish_trading_logs(trading_activity_logs);
    NatsPublisher::getInstance().publish_logs(logs);

    auto write_logs_to_table =
        [&conn, &escape, program_start_time_ms](
            const std::string &table_name,
            const std::vector<TradingLog> &logs_to_write) -> bool {
        if (logs_to_write.empty()) {
            return true;
        }

        std::string copy_cmd =
            "COPY " + table_name +
            " (symbol, strategy_id, log_level, message, event_type, "
            "metadata, source, event_link_id, timestamp, "
            "program_start_time_ms, "
            "exchange_name, market_type) FROM STDIN";

        PGresult *res = PQexec(conn.get(), copy_cmd.c_str());
        if (PQresultStatus(res) != PGRES_COPY_IN) {
            LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_DB_WRITE_FAILED,
                      std::string("COPY ") + table_name +
                          " failed: " + PQerrorMessage(conn.get()),
                      (nlohmann::json{{"pq_error", PQerrorMessage(conn.get())},
                                      {"target_table", table_name},
                                      {"copy_phase", "start"}}));
            PQclear(res);
            return false;
        }
        PQclear(res);

        for (const auto &log : logs_to_write) {
            std::ostringstream line;
            line << escape(log.symbol) << '\t' << log.strategy_id << '\t'
                 << escape(log.log_level) << '\t' << escape(log.message) << '\t'
                 << escape(log.event_type) << '\t'
                 << escape(remove_json_null_escapes(log.metadata.dump()))
                 << '\t' << escape(log.source) << '\t' << log.event_link_id
                 << '\t' << log.timestamp << '\t' << program_start_time_ms
                 << '\t' << escape(log.exchange_name) << '\t'
                 << escape(log.market_type) << '\n';

            std::string row = line.str();
            if (PQputCopyData(conn.get(), row.c_str(), row.length()) != 1) {
                LOG_ERROR(
                    Core::TRADING_LOG_EVENT_TYPE_DB_WRITE_FAILED,
                    std::string("PQputCopyData ") + table_name +
                        " failed: " + PQerrorMessage(conn.get()),
                    (nlohmann::json{{"pq_error", PQerrorMessage(conn.get())},
                                    {"target_table", table_name},
                                    {"copy_phase", "put_data"}}));
                PQputCopyEnd(conn.get(), "error");
                return false;
            }
        }

        if (PQputCopyEnd(conn.get(), nullptr) != 1) {
            LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_DB_WRITE_FAILED,
                      std::string("PQputCopyEnd ") + table_name +
                          " failed: " + PQerrorMessage(conn.get()),
                      (nlohmann::json{{"pq_error", PQerrorMessage(conn.get())},
                                      {"target_table", table_name},
                                      {"copy_phase", "end"}}));
            return false;
        }

        res = PQgetResult(conn.get());
        if (PQresultStatus(res) != PGRES_COMMAND_OK) {
            LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_DB_WRITE_FAILED,
                      std::string("COPY ") + table_name +
                          " failed: " + PQerrorMessage(conn.get()),
                      (nlohmann::json{{"pq_error", PQerrorMessage(conn.get())},
                                      {"target_table", table_name},
                                      {"copy_phase", "result"}}));
            PQclear(res);
            return false;
        }
        PQclear(res);
        return true;
    };

    for (const auto &log : logs) {
        if (log.event_type == Core::TRADING_LOG_EVENT_TYPE_SIGNAL &&
            log.metadata.contains("prediction") &&
            log.metadata.contains("price") &&
            log.metadata.contains("event_time_ms") &&
            log.metadata.contains("signal_strength")) {
            signals.push_back({log.strategy_id,
                               log.metadata["event_time_ms"].get<int64_t>(),
                               log.metadata["prediction"].get<double>(),
                               log.metadata["price"].get<double>(), log.symbol,
                               log.metadata["signal_strength"].get<double>()});
        }
    }

    if (!signals.empty()) {
        PGresult *res = PQexec(conn.get(),
                               "COPY signal (strategy_id, timestamp, signal, "
                               "price, symbol, sizing_coeff) FROM STDIN");
        if (PQresultStatus(res) != PGRES_COPY_IN) {
            LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_DB_WRITE_FAILED,
                      std::string("COPY signal failed: ") +
                          PQerrorMessage(conn.get()),
                      (nlohmann::json{{"pq_error", PQerrorMessage(conn.get())},
                                      {"target_table", "signal"},
                                      {"copy_phase", "start"}}));
            PQclear(res);
            return false;
        }
        PQclear(res);

        for (const auto &sig : signals) {
            std::ostringstream line;
            line << sig.strategy_id << '\t' << sig.timestamp << '\t'
                 << sig.signal << '\t' << sig.price << '\t'
                 << escape(sig.symbol) << '\t' << sig.sizing_coeff << '\n';
            std::string row = line.str();
            if (PQputCopyData(conn.get(), row.c_str(), row.length()) != 1) {
                LOG_ERROR(
                    Core::TRADING_LOG_EVENT_TYPE_DB_WRITE_FAILED,
                    std::string("PQputCopyData signal failed: ") +
                        PQerrorMessage(conn.get()),
                    (nlohmann::json{{"pq_error", PQerrorMessage(conn.get())},
                                    {"target_table", "signal"},
                                    {"copy_phase", "put_data"}}));
                PQputCopyEnd(conn.get(), "error");
                return false;
            }
        }

        if (PQputCopyEnd(conn.get(), nullptr) != 1) {
            LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_DB_WRITE_FAILED,
                      std::string("PQputCopyEnd signal failed: ") +
                          PQerrorMessage(conn.get()),
                      (nlohmann::json{{"pq_error", PQerrorMessage(conn.get())},
                                      {"target_table", "signal"},
                                      {"copy_phase", "end"}}));
            return false;
        }

        res = PQgetResult(conn.get());
        if (PQresultStatus(res) != PGRES_COMMAND_OK) {
            LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_DB_WRITE_FAILED,
                      std::string("COPY signal failed: ") +
                          PQerrorMessage(conn.get()),
                      (nlohmann::json{{"pq_error", PQerrorMessage(conn.get())},
                                      {"target_table", "signal"},
                                      {"copy_phase", "result"}}));
            PQclear(res);
            return false;
        }
        PQclear(res);
    }

    write_logs_to_table("trading_activity_log", trading_activity_logs);
    return write_logs_to_table("trading_log", logs);
}

bool PQUtils::write_engine_state_snapshot(
    const char *conninfo, const Core::EngineStateSnapshot &snapshot) {
    std::unique_ptr<PGconn, decltype(&PQfinish)> conn(PQconnectdb(conninfo),
                                                      &PQfinish);

    if (PQstatus(conn.get()) != CONNECTION_OK) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_DB_CONNECT_FAILED,
                  std::string("Connection to database failed: ") +
                      PQerrorMessage(conn.get()),
                  (nlohmann::json{{"pq_error", PQerrorMessage(conn.get())}}));
        return false;
    }

    // Begin transaction
    PGresult *res = PQexec(conn.get(), "BEGIN");
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        PQclear(res);
        return false;
    }
    PQclear(res);

    const char *query =
        "INSERT INTO engine_state_snapshot "
        "(timestamp_ms, snapshot) "
        "VALUES ($1, $2)";

    std::string timestamp_str = std::to_string(snapshot.timestamp_ms);
    std::string snapshot_json =
        remove_json_null_escapes(snapshot.to_json().dump());

    const char *paramValues[2] = {timestamp_str.c_str(), snapshot_json.c_str()};

    int paramLengths[2] = {static_cast<int>(timestamp_str.length()),
                           static_cast<int>(snapshot_json.length())};

    int paramFormats[2] = {0, 0};  // Use text format

    res = PQexecParams(conn.get(), query, 2, nullptr, paramValues, paramLengths,
                       paramFormats, 0);

    bool success = true;
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_DB_WRITE_FAILED,
                  std::string("Failed to insert engine state snapshot: ") +
                      PQerrorMessage(conn.get()),
                  (nlohmann::json{{"pq_error", PQerrorMessage(conn.get())},
                                  {"target_table", "engine_state_snapshot"}}));
        success = false;
    }
    PQclear(res);

    // Commit or rollback transaction
    if (success) {
        res = PQexec(conn.get(), "COMMIT");
    } else {
        res = PQexec(conn.get(), "ROLLBACK");
    }
    PQclear(res);

    return success;
}
