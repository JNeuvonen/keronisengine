#include <generic_utils.h>
#include <polymarket_event_parser.h>
#include <sqlite3.h>
#include <sqlite_datasets.h>

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "aggr_trade_dataset_tick.h"
#include "nlohmann/json.hpp"

const DbColumnsVector SqliteDatasets::read_table_into_vector(
    const std::string &db_path, const std::string &table_name,
    const std::string &key_column, std::optional<long long> start_ts_ms,
    std::optional<long long> end_ts_ms,
    std::optional<std::vector<std::string>> used_columns) {
    sqlite3 *db = nullptr;
    sqlite3_stmt *stmt = nullptr;
    DbColumnsVector result;

    std::string sql = "SELECT ";
    if (used_columns && !used_columns->empty()) {
        sql += key_column + ", " + CommonUtils::join(*used_columns, ", ");
    } else {
        sql += "*";
    }
    sql += " FROM " + table_name;

    if (start_ts_ms || end_ts_ms) {
        sql += " WHERE ";
        if (start_ts_ms && end_ts_ms) {
            sql += key_column + " BETWEEN ? AND ?";
        } else if (start_ts_ms) {
            sql += key_column + " >= ?";
        } else if (end_ts_ms) {
            sql += key_column + " <= ?";
        }
    }

    sql += " ORDER BY kline_open_time_ms ASC;";

    if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) {
        std::cerr << "Error opening database: " << sqlite3_errmsg(db)
                  << std::endl;
        return result;
    }

    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Error preparing statement: " << sqlite3_errmsg(db)
                  << std::endl;
        sqlite3_close(db);
        return result;
    }

    int bind_index = 1;
    std::string debug_sql = sql;

    if (start_ts_ms) {
        sqlite3_bind_int64(stmt, bind_index++, *start_ts_ms);
        size_t pos = debug_sql.find('?');
        if (pos != std::string::npos) {
            debug_sql.replace(pos, 1, std::to_string(*start_ts_ms));
        }
    }
    if (end_ts_ms) {
        sqlite3_bind_int64(stmt, bind_index++, *end_ts_ms);
        size_t pos = debug_sql.find('?');
        if (pos != std::string::npos) {
            debug_sql.replace(pos, 1, std::to_string(*end_ts_ms));
        }
    }

    std::cout << "SQL with bound parameters: " << debug_sql << std::endl;

    int key_column_index = -1;
    int num_columns = sqlite3_column_count(stmt);
    for (int i = 0; i < num_columns; ++i) {
        const char *col_name = sqlite3_column_name(stmt, i);
        if (col_name && key_column == col_name) {
            key_column_index = i;
            break;
        }
    }

    if (key_column_index == -1) {
        std::cerr << "Key column '" << key_column
                  << "' not found in table: " << table_name << std::endl;
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return result;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ColToValueMap row_map;

        for (int col_idx = 0; col_idx < num_columns; ++col_idx) {
            const char *column_name = sqlite3_column_name(stmt, col_idx);
            int column_type = sqlite3_column_type(stmt, col_idx);

            if (column_name) {
                switch (column_type) {
                    case SQLITE_INTEGER: {
                        int64_t val = sqlite3_column_int64(stmt, col_idx);
                        row_map[column_name] = val;
                        break;
                    }
                    case SQLITE_FLOAT: {
                        double val = sqlite3_column_double(stmt, col_idx);
                        row_map[column_name] = val;
                        break;
                    }
                    case SQLITE_TEXT: {
                        const char *text_val = reinterpret_cast<const char *>(
                            sqlite3_column_text(stmt, col_idx));
                        if (text_val) {
                            try {
                                double value_as_double = std::stod(text_val);
                                if (value_as_double ==
                                    static_cast<int64_t>(value_as_double)) {
                                    row_map[column_name] =
                                        static_cast<int64_t>(value_as_double);
                                } else {
                                    row_map[column_name] = value_as_double;
                                }
                            } catch (...) {
                                row_map[column_name] = 0.0;
                            }
                        } else {
                            row_map[column_name] = 0.0;
                        }
                        break;
                    }
                    case SQLITE_NULL:
                        row_map[column_name] = 0.0;
                        break;
                    default:
                        row_map[column_name] = 0.0;
                        break;
                }
            }
        }

        result.push_back(std::move(row_map));
    }

    // Clean up
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    return result;
}

std::array<StrategyDecisionTick, BACKTEST_CHUNK_SIZE>
SqliteDatasets::load_dataset_ticks(const std::string &path,
                                   long long start_date_ms,
                                   long long timestep) {
    std::array<StrategyDecisionTick, BACKTEST_CHUNK_SIZE> result{};

    sqlite3 *db;
    sqlite3_stmt *stmt;

    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
        return result;
    }

    std::string query =
        std::string(
            "SELECT sizing_coeff, kline_open_time_ms "
            "FROM data "
            "WHERE kline_open_time_ms >= ? AND kline_open_time_ms <= ? "
            "ORDER BY kline_open_time_ms "
            "LIMIT ") +
        std::to_string(BACKTEST_CHUNK_SIZE);

    if (sqlite3_prepare_v2(db, query.c_str(), -1, &stmt, nullptr) ==
        SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, start_date_ms);
        sqlite3_bind_int64(stmt, 2,
                           start_date_ms + timestep * BACKTEST_CHUNK_SIZE);

        bool has_row = (sqlite3_step(stmt) == SQLITE_ROW);
        long long prev_ts = start_date_ms - timestep;

        for (size_t i = 0; i < BACKTEST_CHUNK_SIZE; ++i) {
            long long expected_ts = start_date_ms + i * timestep;
            result[i].timestamp_ms = expected_ts;

            if (has_row) {
                long long row_ts = sqlite3_column_int64(stmt, 1);
                if (row_ts - prev_ts > timestep) {
                    memcpy(result[i].sizing_coefficient, " 0.00", 5);
                    prev_ts = expected_ts;
                } else if (row_ts - prev_ts == timestep) {
                    const char *coeff = reinterpret_cast<const char *>(
                        sqlite3_column_text(stmt, 0));

                    if (coeff && strlen(coeff) <= 5) {
                        memcpy(result[i].sizing_coefficient, coeff, 5);
                    } else {
                        memcpy(result[i].sizing_coefficient, " 0.00", 5);
                    }

                    prev_ts = row_ts;
                    has_row = (sqlite3_step(stmt) == SQLITE_ROW);
                }
            } else {
                memcpy(result[i].sizing_coefficient, " 0.00", 5);
                prev_ts = expected_ts;
            }
        }
    } else {
        for (size_t i = 0; i < BACKTEST_CHUNK_SIZE; ++i) {
            result[i].timestamp_ms = start_date_ms + i * timestep;
            memcpy(result[i].sizing_coefficient, " 0.00", 5);
        }
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    return result;
}

std::vector<PredictionTick> SqliteDatasets::load_predictions_ticks_vec(
    const std::string &path, long long start_date_ms) {
    std::vector<PredictionTick> result;

    sqlite3 *db;
    sqlite3_stmt *stmt;

    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
        return result;
    }

    std::string query =
        "SELECT prediction, close_price, kline_open_time_ms "
        "FROM data "
        "WHERE kline_open_time_ms >= ? "
        "ORDER BY kline_open_time_ms";

    if (sqlite3_prepare_v2(db, query.c_str(), -1, &stmt, nullptr) ==
        SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, start_date_ms);

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            PredictionTick tick;
            tick.timestamp_ms = sqlite3_column_int64(stmt, 2);
            tick.close_price = sqlite3_column_double(stmt, 1);
            tick.prediction = sqlite3_column_double(stmt, 0);
            result.push_back(tick);
        }
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    return result;
}

std::array<MarketData::OhlcvDatasetTick, BACKTEST_CHUNK_SIZE>
SqliteDatasets::load_price_dataset_ticks(const std::string &path,
                                         long long start_date_ms,
                                         long long timestep) {
    std::array<MarketData::OhlcvDatasetTick, BACKTEST_CHUNK_SIZE> result{};

    try {
        sqlite3 *db;
        sqlite3_stmt *stmt;

        if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
            std::cout << "Failed to open database: " << sqlite3_errmsg(db)
                      << std::endl;
            throw std::runtime_error("Failed to open database");
        }

        std::string query =
            "SELECT kline_open_time_ms, close_price, "
            "open_price, high_price, "
            "low_price, volume, "
            "quote_asset_volume, number_of_trades, "
            "taker_buy_base_asset_volume, "
            "taker_buy_quote_asset_volume "
            "FROM data "
            "WHERE kline_open_time_ms >= ? AND kline_open_time_ms <= ? "
            "ORDER BY kline_open_time_ms "
            "LIMIT " +
            std::to_string(BACKTEST_CHUNK_SIZE);

        if (sqlite3_prepare_v2(db, query.c_str(), -1, &stmt, nullptr) !=
            SQLITE_OK) {
            std::cout << "Failed to prepare statement: " << sqlite3_errmsg(db)
                      << std::endl;
            std::cout << "Query: " << query << std::endl;
            sqlite3_close(db);
            throw std::runtime_error("Failed to prepare statement");
        }

        long long end_date_ms = start_date_ms + timestep * BACKTEST_CHUNK_SIZE;
        sqlite3_bind_int64(stmt, 1, start_date_ms);
        sqlite3_bind_int64(stmt, 2, end_date_ms);

        bool has_row = (sqlite3_step(stmt) == SQLITE_ROW);
        long long prev_ts = start_date_ms - timestep;
        size_t rows_loaded = 0;
        size_t gaps_filled = 0;

        for (size_t i = 0; i < BACKTEST_CHUNK_SIZE; ++i) {
            long long expected_ts = start_date_ms + i * timestep;
            result[i].kline_open_time = expected_ts;

            if (has_row) {
                long long row_ts = sqlite3_column_int64(stmt, 0);

                if (row_ts - prev_ts > timestep) {
                    result[i] = MarketData::OhlcvDatasetTick{};
                    result[i].kline_open_time = expected_ts;
                    prev_ts = expected_ts;
                    gaps_filled++;
                } else if (row_ts - prev_ts == timestep) {
                    result[i].close_price = sqlite3_column_double(stmt, 1);
                    result[i].open_price = sqlite3_column_double(stmt, 2);
                    result[i].high_price = sqlite3_column_double(stmt, 3);
                    result[i].low_price = sqlite3_column_double(stmt, 4);
                    result[i].volume = sqlite3_column_double(stmt, 5);
                    result[i].quote_asset_volume =
                        sqlite3_column_double(stmt, 6);
                    result[i].number_of_trades = sqlite3_column_int64(stmt, 7);
                    result[i].taker_buy_base_volume =
                        sqlite3_column_double(stmt, 8);
                    result[i].taker_buy_quote_volume =
                        sqlite3_column_double(stmt, 9);

                    prev_ts = row_ts;
                    has_row = (sqlite3_step(stmt) == SQLITE_ROW);
                    rows_loaded++;
                }
            } else {
                result[i] = MarketData::OhlcvDatasetTick{};
                result[i].kline_open_time = expected_ts;
                prev_ts = expected_ts;
                gaps_filled++;
            }
        }

        sqlite3_finalize(stmt);
        sqlite3_close(db);

    } catch (const std::exception &e) {
        std::cout << "Error loading price dataset ticks: " << path << " "
                  << e.what() << std::endl;
        for (size_t i = 0; i < BACKTEST_CHUNK_SIZE; ++i) {
            result[i] = MarketData::OhlcvDatasetTick{};
            result[i].kline_open_time = start_date_ms + i * timestep;
        }
    }

    return result;
}

SqliteDatasets::OhlcvDatasetsResult SqliteDatasets::load_ohlcv_dataset(
    const std::string &path, const std::string &table_name,
    std::optional<long long> start_ts_ms, std::optional<long long> end_ts_ms) {
    SqliteDatasets::OhlcvDatasetsResult result;
    sqlite3 *db = nullptr;
    sqlite3_stmt *stmt = nullptr;

    // std::string sql =
    //     "SELECT kline_open_time_ms, binance_spot_close_price, "
    //     "binance_spot_open_price, binance_spot_high_price, "
    //     "binance_spot_low_price, binance_spot_volume, "
    //     "binance_spot_quote_asset_volume, binance_spot_number_of_trades, "
    //     "binance_spot_taker_buy_base_asset_volume, "
    //     "binance_spot_taker_buy_quote_asset_volume "
    //     "FROM " +
    //     table_name;

    std::string sql =
        "SELECT kline_open_time_ms, close_price, "
        "open_price, high_price, "
        "low_price, volume, "
        "quote_asset_volume, number_of_trades, "
        "taker_buy_base_asset_volume, "
        "taker_buy_quote_asset_volume "
        "FROM " +
        table_name;

    if (start_ts_ms || end_ts_ms) {
        sql += " WHERE ";
        if (start_ts_ms && end_ts_ms) {
            sql += "kline_open_time_ms BETWEEN ? AND ?";
        } else if (start_ts_ms) {
            sql += "kline_open_time_ms >= ?";
        } else {
            sql += "kline_open_time_ms <= ?";
        }
    }

    sql += " ORDER BY kline_open_time_ms ASC;";

    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
        std::cerr << "Error opening database: " << sqlite3_errmsg(db)
                  << std::endl;
        return result;
    }

    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Error preparing statement: " << sqlite3_errmsg(db)
                  << std::endl;
        sqlite3_close(db);
        return result;
    }

    int bind_index = 1;
    std::string debug_sql = sql;

    if (start_ts_ms) {
        sqlite3_bind_int64(stmt, bind_index++, *start_ts_ms);
        size_t pos = debug_sql.find('?');
        if (pos != std::string::npos) {
            debug_sql.replace(pos, 1, std::to_string(*start_ts_ms));
        }
    }
    if (end_ts_ms) {
        sqlite3_bind_int64(stmt, bind_index++, *end_ts_ms);
        size_t pos = debug_sql.find('?');
        if (pos != std::string::npos) {
            debug_sql.replace(pos, 1, std::to_string(*end_ts_ms));
        }
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        result.kline_open_times.push_back(sqlite3_column_int64(stmt, 0));
        result.close_prices.push_back(sqlite3_column_double(stmt, 1));
        result.open_prices.push_back(sqlite3_column_double(stmt, 2));
        result.high_prices.push_back(sqlite3_column_double(stmt, 3));
        result.low_prices.push_back(sqlite3_column_double(stmt, 4));
        result.volumes.push_back(sqlite3_column_double(stmt, 5));
        result.quote_asset_volumes.push_back(sqlite3_column_double(stmt, 6));
        result.number_of_trades.push_back(sqlite3_column_int64(stmt, 7));
        result.taker_buy_base_volumes.push_back(sqlite3_column_double(stmt, 8));
        result.taker_buy_quote_volumes.push_back(
            sqlite3_column_double(stmt, 9));
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    return result;
}

void SqliteDatasets::write_sizing_coefficients(
    const std::string &db_path, const std::string &table_name,
    std::span<const long long> timestamps,
    std::span<const char *const> coefficients) {
    if (timestamps.size() != coefficients.size()) {
        throw std::invalid_argument(
            "Timestamps and coefficients must have the same size");
    }

    std::remove(db_path.c_str());

    sqlite3 *db = nullptr;
    sqlite3_stmt *stmt = nullptr;

    if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) {
        throw std::runtime_error("Failed to open database: " +
                                 std::string(sqlite3_errmsg(db)));
    }

    std::string create_table_sql = "CREATE TABLE IF NOT EXISTS " + table_name +
                                   " ("
                                   "kline_open_time_ms INTEGER PRIMARY KEY, "
                                   "sizing_coeff CHAR(5) NOT NULL"
                                   ");";

    char *error_msg = nullptr;
    if (sqlite3_exec(db, create_table_sql.c_str(), nullptr, nullptr,
                     &error_msg) != SQLITE_OK) {
        std::string error(error_msg);
        sqlite3_free(error_msg);
        sqlite3_close(db);
        throw std::runtime_error("Failed to create table: " + error);
    }

    // Begin transaction
    if (sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, nullptr, &error_msg) !=
        SQLITE_OK) {
        std::string error(error_msg);
        sqlite3_free(error_msg);
        sqlite3_close(db);
        throw std::runtime_error("Failed to begin transaction: " + error);
    }

    // Prepare insert statement
    std::string insert_sql =
        "INSERT OR REPLACE INTO " + table_name +
        " (kline_open_time_ms, sizing_coeff) VALUES (?, ?);";

    if (sqlite3_prepare_v2(db, insert_sql.c_str(), -1, &stmt, nullptr) !=
        SQLITE_OK) {
        sqlite3_close(db);
        throw std::runtime_error("Failed to prepare statement: " +
                                 std::string(sqlite3_errmsg(db)));
    }

    // Insert data
    for (size_t i = 0; i < timestamps.size(); ++i) {
        sqlite3_bind_int64(stmt, 1, timestamps[i]);
        sqlite3_bind_text(stmt, 2, coefficients[i], -1, SQLITE_STATIC);

        if (sqlite3_step(stmt) != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
            sqlite3_close(db);
            throw std::runtime_error("Failed to insert row: " +
                                     std::string(sqlite3_errmsg(db)));
        }

        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
    }

    // Commit transaction
    if (sqlite3_exec(db, "COMMIT;", nullptr, nullptr, &error_msg) !=
        SQLITE_OK) {
        std::string error(error_msg);
        sqlite3_free(error_msg);
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        throw std::runtime_error("Failed to commit transaction: " + error);
    }

    // Cleanup
    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

std::vector<MarketData::OhlcvDatasetTick>
SqliteDatasets::load_ohlcv_dataset_ticks(const std::string &path,
                                         const std::string &table_name,
                                         std::optional<long long> start_ts_ms,
                                         std::optional<long long> end_ts_ms) {
    std::vector<MarketData::OhlcvDatasetTick> result;

    sqlite3 *db = nullptr;
    sqlite3_stmt *stmt = nullptr;

    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
        return {};
    }

    std::string query =
        "SELECT kline_open_time_ms, close_price, "
        "open_price, high_price, "
        "low_price, volume, "
        "quote_asset_volume, number_of_trades, "
        "taker_buy_base_asset_volume, "
        "taker_buy_quote_asset_volume, "
        "kline_close_time_ms "
        "FROM " +
        table_name;

    if (start_ts_ms || end_ts_ms) {
        query += " WHERE ";
        if (start_ts_ms) {
            query += "kline_open_time_ms >= ?";
        }
        if (start_ts_ms && end_ts_ms) {
            query += " AND ";
        }
        if (end_ts_ms) {
            query += "kline_close_time_ms <= ?";
        }
    }

    query += " ORDER BY kline_open_time_ms ASC;";

    if (sqlite3_prepare_v2(db, query.c_str(), -1, &stmt, nullptr) !=
        SQLITE_OK) {
        std::cout << "Failed to prepare statement: " << sqlite3_errmsg(db)
                  << std::endl;
        std::cout << "Query: " << query << std::endl;
        return {};
    }

    if (start_ts_ms) {
        sqlite3_bind_int64(stmt, 1, *start_ts_ms);
    }
    if (end_ts_ms) {
        sqlite3_bind_int64(stmt, 2, *end_ts_ms);
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        MarketData::OhlcvDatasetTick tick;
        tick.kline_open_time = sqlite3_column_int64(stmt, 0);
        tick.close_price = sqlite3_column_double(stmt, 1);
        tick.open_price = sqlite3_column_double(stmt, 2);
        tick.high_price = sqlite3_column_double(stmt, 3);
        tick.low_price = sqlite3_column_double(stmt, 4);
        tick.volume = sqlite3_column_double(stmt, 5);
        tick.quote_asset_volume = sqlite3_column_double(stmt, 6);
        tick.number_of_trades = sqlite3_column_int64(stmt, 7);
        tick.taker_buy_base_volume = sqlite3_column_double(stmt, 8);
        tick.taker_buy_quote_volume = sqlite3_column_double(stmt, 9);
        tick.kline_close_time = sqlite3_column_int64(stmt, 10);
        tick.is_read_from_fast_storage = true;

        result.push_back(tick);
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    return result;
}

void SqliteDatasets::write_ohlcv_dataset(
    const std::string &db_path, const std::string &table_name,
    const std::vector<MarketData::OhlcvDatasetTick> &ticks) {
    std::cout << "Writing " << ticks.size() << " ticks to " << db_path
              << std::endl;

    // Check if directory exists, if not create it
    std::filesystem::path dir_path =
        std::filesystem::path(db_path).parent_path();
    if (!std::filesystem::exists(dir_path)) {
        std::cout << "Creating directory: " << dir_path << std::endl;
        std::filesystem::create_directories(dir_path);
    }

    sqlite3 *db = nullptr;
    sqlite3_stmt *stmt = nullptr;

    std::remove(db_path.c_str());

    if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) {
        std::string error = sqlite3_errmsg(db);
        std::cout << "Failed to open database: " << error << std::endl;
        throw std::runtime_error("Failed to open database: " + error);
    }

    std::string create_table_sql = "CREATE TABLE IF NOT EXISTS " + table_name +
                                   " ("
                                   "kline_open_time_ms INTEGER, "
                                   "close_price REAL NOT NULL, "
                                   "open_price REAL NOT NULL, "
                                   "high_price REAL NOT NULL, "
                                   "low_price REAL NOT NULL, "
                                   "volume REAL NOT NULL, "
                                   "quote_asset_volume REAL NOT NULL, "
                                   "number_of_trades INTEGER NOT NULL, "
                                   "taker_buy_base_asset_volume REAL NOT NULL, "
                                   "taker_buy_quote_asset_volume REAL NOT NULL"
                                   ");";

    char *error_msg = nullptr;
    if (sqlite3_exec(db, create_table_sql.c_str(), nullptr, nullptr,
                     &error_msg) != SQLITE_OK) {
        std::string error(error_msg);
        sqlite3_free(error_msg);
        sqlite3_close(db);
        throw std::runtime_error("Failed to create table: " + error);
    }

    if (sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, nullptr, &error_msg) !=
        SQLITE_OK) {
        std::string error(error_msg);
        sqlite3_free(error_msg);
        sqlite3_close(db);
        throw std::runtime_error("Failed to begin transaction: " + error);
    }

    std::string insert_sql =
        "INSERT INTO " + table_name +
        " (kline_open_time_ms, close_price, open_price, high_price, "
        "low_price, volume, quote_asset_volume, number_of_trades, "
        "taker_buy_base_asset_volume, taker_buy_quote_asset_volume) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";

    if (sqlite3_prepare_v2(db, insert_sql.c_str(), -1, &stmt, nullptr) !=
        SQLITE_OK) {
        sqlite3_close(db);
        throw std::runtime_error("Failed to prepare statement: " +
                                 std::string(sqlite3_errmsg(db)));
    }

    int count = 0;
    for (const auto &tick : ticks) {
        sqlite3_bind_int64(stmt, 1, tick.kline_open_time);
        sqlite3_bind_double(stmt, 2, tick.close_price);
        sqlite3_bind_double(stmt, 3, tick.open_price);
        sqlite3_bind_double(stmt, 4, tick.high_price);
        sqlite3_bind_double(stmt, 5, tick.low_price);
        sqlite3_bind_double(stmt, 6, tick.volume);
        sqlite3_bind_double(stmt, 7, tick.quote_asset_volume);
        sqlite3_bind_int64(stmt, 8, tick.number_of_trades);
        sqlite3_bind_double(stmt, 9, tick.taker_buy_base_volume);
        sqlite3_bind_double(stmt, 10, tick.taker_buy_quote_volume);

        if (sqlite3_step(stmt) != SQLITE_DONE) {
            std::string error = sqlite3_errmsg(db);
            std::cout << "Failed to insert row " << count << ": " << error
                      << std::endl;
            std::cout << "Failed tick data: " << "time=" << tick.kline_open_time
                      << ", close=" << tick.close_price
                      << ", open=" << tick.open_price << std::endl;

            sqlite3_finalize(stmt);
            sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
            sqlite3_close(db);
            throw std::runtime_error("Failed to insert row: " + error);
        }

        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        count++;
    }

    std::cout << "Successfully inserted all " << count << " ticks" << std::endl;

    if (sqlite3_exec(db, "COMMIT;", nullptr, nullptr, &error_msg) !=
        SQLITE_OK) {
        std::string error(error_msg);
        sqlite3_free(error_msg);
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        throw std::runtime_error("Failed to commit transaction: " + error);
    }

    // Verify the final database
    if (std::filesystem::exists(db_path)) {
        std::cout << "Final database file exists at: "
                  << std::filesystem::absolute(db_path) << std::endl;
        std::cout << "File size: " << std::filesystem::file_size(db_path)
                  << " bytes" << std::endl;

        // Verify the data
        sqlite3 *verify_db = nullptr;
        if (sqlite3_open(db_path.c_str(), &verify_db) == SQLITE_OK) {
            sqlite3_stmt *verify_stmt = nullptr;
            std::string verify_sql = "SELECT COUNT(*) FROM " + table_name + ";";
            if (sqlite3_prepare_v2(verify_db, verify_sql.c_str(), -1,
                                   &verify_stmt, nullptr) == SQLITE_OK) {
                if (sqlite3_step(verify_stmt) == SQLITE_ROW) {
                    int count = sqlite3_column_int(verify_stmt, 0);
                    std::cout << "Verified row count in database: " << count
                              << std::endl;
                }
                sqlite3_finalize(verify_stmt);
            }
            sqlite3_close(verify_db);
        }
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

void SqliteDatasets::write_ohlcv_dataset_unique(
    const std::string &db_path, const std::string &table_name,
    std::vector<MarketData::OhlcvDatasetTick> &ticks) {
    // Check if directory exists, if not create it
    std::filesystem::path dir_path =
        std::filesystem::path(db_path).parent_path();
    if (!std::filesystem::exists(dir_path)) {
        std::filesystem::create_directories(dir_path);
    }

    sqlite3 *db = nullptr;
    sqlite3_stmt *stmt = nullptr;

    if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) {
        return;
    }

    std::string create_table_sql =
        "CREATE TABLE IF NOT EXISTS " + table_name +
        " ("
        "kline_open_time_ms INTEGER PRIMARY KEY, "
        "close_price REAL NOT NULL, "
        "open_price REAL NOT NULL, "
        "high_price REAL NOT NULL, "
        "low_price REAL NOT NULL, "
        "volume REAL NOT NULL, "
        "quote_asset_volume REAL NOT NULL, "
        "number_of_trades INTEGER NOT NULL, "
        "taker_buy_base_asset_volume REAL NOT NULL, "
        "taker_buy_quote_asset_volume REAL NOT NULL, "
        "kline_close_time_ms INTEGER NOT NULL"
        ");";

    char *error_msg = nullptr;
    if (sqlite3_exec(db, create_table_sql.c_str(), nullptr, nullptr,
                     &error_msg) != SQLITE_OK) {
        std::string error(error_msg);
        sqlite3_free(error_msg);
        sqlite3_close(db);
        return;
    }

    if (sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, nullptr, &error_msg) !=
        SQLITE_OK) {
        std::string error(error_msg);
        sqlite3_free(error_msg);
        sqlite3_close(db);
        return;
    }

    std::string insert_sql =
        "INSERT OR IGNORE INTO " + table_name +
        " (kline_open_time_ms, close_price, open_price, high_price, "
        "low_price, volume, quote_asset_volume, number_of_trades, "
        "taker_buy_base_asset_volume, taker_buy_quote_asset_volume, "
        "kline_close_time_ms) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";

    if (sqlite3_prepare_v2(db, insert_sql.c_str(), -1, &stmt, nullptr) !=
        SQLITE_OK) {
        sqlite3_close(db);
        return;
    }

    int count = 0;
    int skipped = 0;
    for (auto &tick : ticks) {
        if (tick.is_read_from_fast_storage) {
            skipped++;
            continue;
        }

        sqlite3_bind_int64(stmt, 1, tick.kline_open_time);
        sqlite3_bind_double(stmt, 2, tick.close_price);
        sqlite3_bind_double(stmt, 3, tick.open_price);
        sqlite3_bind_double(stmt, 4, tick.high_price);
        sqlite3_bind_double(stmt, 5, tick.low_price);
        sqlite3_bind_double(stmt, 6, tick.volume);
        sqlite3_bind_double(stmt, 7, tick.quote_asset_volume);
        sqlite3_bind_int64(stmt, 8, tick.number_of_trades);
        sqlite3_bind_double(stmt, 9, tick.taker_buy_base_volume);
        sqlite3_bind_double(stmt, 10, tick.taker_buy_quote_volume);
        sqlite3_bind_int64(stmt, 11, tick.kline_close_time);

        if (sqlite3_step(stmt) != SQLITE_DONE) {
            std::string error = sqlite3_errmsg(db);
            std::cout << "Failed to insert row " << count << ": " << error
                      << std::endl;
            std::cout << "Failed tick data: " << "time=" << tick.kline_open_time
                      << ", close=" << tick.close_price
                      << ", open=" << tick.open_price << std::endl;

            sqlite3_finalize(stmt);
            sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
            sqlite3_close(db);
            return;
        }

        if (sqlite3_changes(db) == 0) {
            skipped++;
        }

        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        count++;
        tick.is_read_from_fast_storage = true;
    }

    if (sqlite3_exec(db, "COMMIT;", nullptr, nullptr, &error_msg) !=
        SQLITE_OK) {
        std::string error(error_msg);
        sqlite3_free(error_msg);
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return;
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

void SqliteDatasets::write_ohlcv_dataset_unique_minimal(
    const std::string &db_path, const std::string &table_name,
    std::vector<MarketData::OhlcvDatasetTick> &ticks) {
    std::filesystem::path dir_path =
        std::filesystem::path(db_path).parent_path();
    if (!std::filesystem::exists(dir_path)) {
        std::filesystem::create_directories(dir_path);
    }

    sqlite3 *db = nullptr;
    sqlite3_stmt *stmt = nullptr;

    if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) {
        return;
    }

    std::string create_table_sql = "CREATE TABLE IF NOT EXISTS " + table_name +
                                   " ("
                                   "kline_open_time_ms INTEGER PRIMARY KEY, "
                                   "high_price REAL NOT NULL, "
                                   "low_price REAL NOT NULL, "
                                   "close_price REAL NOT NULL, "
                                   "kline_close_time_ms INTEGER NOT NULL"
                                   ");";

    char *error_msg = nullptr;
    if (sqlite3_exec(db, create_table_sql.c_str(), nullptr, nullptr,
                     &error_msg) != SQLITE_OK) {
        std::string error(error_msg);
        sqlite3_free(error_msg);
        sqlite3_close(db);
        return;
    }

    if (sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, nullptr, &error_msg) !=
        SQLITE_OK) {
        std::string error(error_msg);
        sqlite3_free(error_msg);
        sqlite3_close(db);
        return;
    }

    std::string insert_sql = "INSERT OR IGNORE INTO " + table_name +
                             " (kline_open_time_ms, high_price, low_price, "
                             "close_price, kline_close_time_ms) "
                             "VALUES (?, ?, ?, ?, ?);";

    if (sqlite3_prepare_v2(db, insert_sql.c_str(), -1, &stmt, nullptr) !=
        SQLITE_OK) {
        sqlite3_close(db);
        return;
    }

    int count = 0;
    int skipped = 0;
    for (auto &tick : ticks) {
        if (tick.is_read_from_fast_storage) {
            skipped++;
            continue;
        }

        sqlite3_bind_int64(stmt, 1, tick.kline_open_time);
        sqlite3_bind_double(stmt, 2, tick.high_price);
        sqlite3_bind_double(stmt, 3, tick.low_price);
        sqlite3_bind_double(stmt, 4, tick.close_price);
        sqlite3_bind_int64(stmt, 5, tick.kline_close_time);

        if (sqlite3_step(stmt) != SQLITE_DONE) {
            std::string error = sqlite3_errmsg(db);
            std::cout << "Failed to insert row " << count << ": " << error
                      << std::endl;
            std::cout << "Failed tick data: " << "time=" << tick.kline_open_time
                      << ", high=" << tick.high_price
                      << ", low=" << tick.low_price << std::endl;

            sqlite3_finalize(stmt);
            sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
            sqlite3_close(db);
            return;
        }

        if (sqlite3_changes(db) == 0) {
            skipped++;
        }

        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        count++;
        tick.is_read_from_fast_storage = true;
    }

    if (sqlite3_exec(db, "COMMIT;", nullptr, nullptr, &error_msg) !=
        SQLITE_OK) {
        std::string error(error_msg);
        sqlite3_free(error_msg);
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return;
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

std::vector<StrategyDecisionTick> SqliteDatasets::load_dataset_ticks_vec(
    const std::string &path, long long start_date_ms) {
    std::vector<StrategyDecisionTick> result;

    sqlite3 *db;
    sqlite3_stmt *stmt;

    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
        return result;
    }

    std::string query =
        "SELECT sizing_coeff, kline_open_time_ms "
        "FROM data "
        "WHERE kline_open_time_ms >= ? "
        "ORDER BY kline_open_time_ms";

    if (sqlite3_prepare_v2(db, query.c_str(), -1, &stmt, nullptr) ==
        SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, start_date_ms);

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            StrategyDecisionTick tick;
            tick.timestamp_ms = sqlite3_column_int64(stmt, 1);

            const char *coeff =
                reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));

            if (coeff && strlen(coeff) <= 5) {
                memcpy(tick.sizing_coefficient, coeff, 5);
            } else {
                memcpy(tick.sizing_coefficient, " 0.00", 5);
            }

            result.push_back(tick);
        }
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    return result;
}

std::vector<AggrTradeDatasetTick> SqliteDatasets::load_trades_dataset_dir(
    const std::string &path, std::optional<long long> start_ts_ms,
    std::optional<long long> end_ts_ms) {
    std::vector<AggrTradeDatasetTick> result;

    std::filesystem::path dir_path = std::filesystem::path(path);
    if (!std::filesystem::exists(dir_path)) {
        throw std::runtime_error("Directory does not exist: " + path);
    }

    // Get all files and sort them
    std::vector<std::filesystem::path> files;
    for (const auto &entry : std::filesystem::directory_iterator(dir_path)) {
        if (entry.is_regular_file()) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());

    for (const auto &file : files) {
        std::string filename = file.stem().string();

        try {
            int year = std::stoi(filename.substr(0, 4));
            int month = std::stoi(filename.substr(5, 2));
            int day = std::stoi(filename.substr(8, 2));

            std::tm tm = {};
            tm.tm_year = year - 1900;
            tm.tm_mon = month - 1;
            tm.tm_mday = day;
            long long file_ts = std::mktime(&tm) * 1000LL;

            if (start_ts_ms && file_ts < *start_ts_ms) {
                continue;
            }

            if (end_ts_ms && file_ts > *end_ts_ms) {
                break;
            }

        } catch (const std::exception &e) {
            continue;
        }

        sqlite3 *db = nullptr;
        sqlite3_stmt *stmt = nullptr;

        if (sqlite3_open(file.string().c_str(), &db) != SQLITE_OK) {
            std::cout << "Failed to open database: " << sqlite3_errmsg(db)
                      << std::endl;
            continue;
        }

        std::string query =
            "SELECT trade_id, price, qty, timestamp, is_buyer_maker, quote_qty "
            "FROM data ORDER BY timestamp ASC";

        if (sqlite3_prepare_v2(db, query.c_str(), -1, &stmt, nullptr) !=
            SQLITE_OK) {
            std::cout << "Failed to prepare statement: " << sqlite3_errmsg(db)
                      << std::endl;
            sqlite3_close(db);
            continue;
        }

        bool file_done = false;
        while (sqlite3_step(stmt) == SQLITE_ROW && !file_done) {
            AggrTradeDatasetTick tick;

            tick.trade_id = sqlite3_column_int64(stmt, 0);
            tick.price = sqlite3_column_double(stmt, 1);
            tick.quantity = sqlite3_column_double(stmt, 2);
            tick.timestamp = sqlite3_column_int64(stmt, 3);
            tick.is_buyer_maker = sqlite3_column_int(stmt, 4) != 0;
            // Note: quote_qty is column 5 but not used in the struct currently

            // Skip trades before start time
            if (start_ts_ms && tick.timestamp < *start_ts_ms) {
                continue;
            }

            // Stop if we've passed end time
            if (end_ts_ms && tick.timestamp > *end_ts_ms) {
                file_done = true;
                break;
            }

            result.push_back(tick);
        }

        sqlite3_finalize(stmt);
        sqlite3_close(db);

        if (file_done) {
            break;
        }
    }

    std::cout << "Loaded " << result.size() << " ticks total" << std::endl;
    return result;
}

void SqliteDatasets::write_predictions(const std::string &db_path,
                                       const std::string &table_name,
                                       std::span<const long long> timestamps,
                                       std::span<const double> predictions,
                                       std::span<const double> close_prices) {
    if (timestamps.size() != predictions.size() ||
        timestamps.size() != close_prices.size()) {
        std::cout << "Timestamps size: " << timestamps.size() << std::endl;
        std::cout << "Predictions size: " << predictions.size() << std::endl;
        std::cout << "Close prices size: " << close_prices.size() << std::endl;
        std::cout << "Timestamps, predictions and close_prices must have the "
                     "same size"
                  << std::endl;
        throw std::invalid_argument(
            "Timestamps, predictions and close_prices must have the same size");
    }

    std::remove(db_path.c_str());

    sqlite3 *db = nullptr;
    sqlite3_stmt *stmt = nullptr;

    if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) {
        throw std::runtime_error("Failed to open database: " +
                                 std::string(sqlite3_errmsg(db)));
    }

    std::string create_table_sql = "CREATE TABLE IF NOT EXISTS " + table_name +
                                   " ("
                                   "kline_open_time_ms INTEGER PRIMARY KEY, "
                                   "prediction REAL NOT NULL, "
                                   "close_price REAL NOT NULL"
                                   ");";

    char *error_msg = nullptr;
    if (sqlite3_exec(db, create_table_sql.c_str(), nullptr, nullptr,
                     &error_msg) != SQLITE_OK) {
        std::string error(error_msg);
        sqlite3_free(error_msg);
        sqlite3_close(db);
        throw std::runtime_error("Failed to create table: " + error);
    }

    if (sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, nullptr, &error_msg) !=
        SQLITE_OK) {
        std::string error(error_msg);
        sqlite3_free(error_msg);
        sqlite3_close(db);
        throw std::runtime_error("Failed to begin transaction: " + error);
    }

    std::string insert_sql =
        "INSERT OR REPLACE INTO " + table_name +
        " (kline_open_time_ms, prediction, close_price) VALUES (?, ?, ?);";

    if (sqlite3_prepare_v2(db, insert_sql.c_str(), -1, &stmt, nullptr) !=
        SQLITE_OK) {
        sqlite3_close(db);
        throw std::runtime_error("Failed to prepare statement: " +
                                 std::string(sqlite3_errmsg(db)));
    }

    for (size_t i = 0; i < timestamps.size(); ++i) {
        sqlite3_bind_int64(stmt, 1, timestamps[i]);
        sqlite3_bind_double(stmt, 2, predictions[i]);
        sqlite3_bind_double(stmt, 3, close_prices[i]);

        if (sqlite3_step(stmt) != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
            sqlite3_close(db);
            throw std::runtime_error("Failed to insert row: " +
                                     std::string(sqlite3_errmsg(db)));
        }

        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
    }

    if (sqlite3_exec(db, "COMMIT;", nullptr, nullptr, &error_msg) !=
        SQLITE_OK) {
        std::string error(error_msg);
        sqlite3_free(error_msg);
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        throw std::runtime_error("Failed to commit transaction: " + error);
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

static long long parse_iso8601_to_ms(const std::string &iso_str) {
    if (iso_str.empty()) return 0;

    std::string cleaned = iso_str;
    auto t_pos = cleaned.find('T');
    if (t_pos != std::string::npos) {
        cleaned[t_pos] = ' ';
    }
    if (!cleaned.empty() && cleaned.back() == 'Z') {
        cleaned.pop_back();
    }

    std::tm tm = {};
    std::istringstream ss(cleaned);
    ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
    if (ss.fail()) return 0;

    time_t time_since_epoch = timegm(&tm);
    return static_cast<long long>(time_since_epoch) * 1000LL;
}

static std::vector<std::string> parse_json_string_array(
    const std::string &json_str) {
    std::vector<std::string> result;
    try {
        auto arr = nlohmann::json::parse(json_str);
        if (arr.is_array()) {
            for (const auto &item : arr) {
                result.push_back(item.get<std::string>());
            }
        }
    } catch (...) {
    }
    return result;
}

static std::string sqlite_col_text(sqlite3_stmt *stmt, int col) {
    const unsigned char *text = sqlite3_column_text(stmt, col);
    if (!text) return "";
    return reinterpret_cast<const char *>(text);
}

static std::unordered_map<std::string, std::vector<double>>
load_resolved_market_prices(const std::string &resolved_markets_db_path,
                            const std::string &series_slug) {
    std::unordered_map<std::string, std::vector<double>> result;

    if (resolved_markets_db_path.empty() ||
        !std::filesystem::exists(resolved_markets_db_path)) {
        return result;
    }

    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(resolved_markets_db_path.c_str(), &db,
                        SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        std::cerr << "Error opening resolved_markets DB: " << sqlite3_errmsg(db)
                  << std::endl;
        if (db) sqlite3_close(db);
        return result;
    }

    std::string sql =
        "SELECT conditionId, outcomePrices FROM resolved_markets "
        "WHERE seriesSlug = ?";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Error preparing resolved_markets query: "
                  << sqlite3_errmsg(db) << std::endl;
        sqlite3_close(db);
        return result;
    }

    sqlite3_bind_text(stmt, 1, series_slug.c_str(), -1, SQLITE_STATIC);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::string condition_id = sqlite_col_text(stmt, 0);
        std::string outcome_prices_json = sqlite_col_text(stmt, 1);

        std::vector<double> prices;
        try {
            auto arr = nlohmann::json::parse(outcome_prices_json);
            if (arr.is_array()) {
                for (const auto &item : arr) {
                    prices.push_back(std::stod(item.get<std::string>()));
                }
            }
        } catch (...) {
        }

        if (!prices.empty()) {
            result[condition_id] = std::move(prices);
        }
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    return result;
}

std::vector<PolymarketEventMeta> SqliteDatasets::load_polymarket_markets(
    const std::string &series_base_dir, const std::string &series_slug,
    long long start_ts_ms, long long end_ts_ms) {
    std::vector<PolymarketEventMeta> result;

    // Per-series store: one sqlite file per slug holding the events, markets and
    // resolved_markets tables. All three reads below open this same path.
    std::string store_dir = series_base_dir;
    if (!store_dir.empty() && store_dir.back() != '/') store_dir += '/';
    const std::string store_db_path = store_dir + series_slug + "/store.db";
    const std::string &events_db_path = store_db_path;
    const std::string &markets_db_path = store_db_path;
    const std::string &resolved_markets_db_path = store_db_path;

    // Load events for this series from events DB
    std::unordered_map<std::string, std::pair<std::string, std::string>>
        event_dates;  // eventId -> (startDate, endDate)

    {
        sqlite3 *db = nullptr;
        if (sqlite3_open_v2(events_db_path.c_str(), &db, SQLITE_OPEN_READONLY,
                            nullptr) != SQLITE_OK) {
            std::cerr << "Error opening events DB: " << sqlite3_errmsg(db)
                      << std::endl;
            if (db) sqlite3_close(db);
            return result;
        }

        std::string sql =
            "SELECT id, startDate, endDate FROM events WHERE seriesSlug = ?";
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) !=
            SQLITE_OK) {
            std::cerr << "Error preparing events query: " << sqlite3_errmsg(db)
                      << std::endl;
            sqlite3_close(db);
            return result;
        }

        sqlite3_bind_text(stmt, 1, series_slug.c_str(), -1, SQLITE_STATIC);

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string event_id = sqlite_col_text(stmt, 0);
            std::string start_date = sqlite_col_text(stmt, 1);
            std::string end_date = sqlite_col_text(stmt, 2);
            event_dates[event_id] = {start_date, end_date};
        }

        sqlite3_finalize(stmt);
        sqlite3_close(db);
    }

    if (event_dates.empty()) {
        return result;
    }

    // Load markets that belong to these events
    {
        sqlite3 *db = nullptr;
        if (sqlite3_open_v2(markets_db_path.c_str(), &db, SQLITE_OPEN_READONLY,
                            nullptr) != SQLITE_OK) {
            std::cerr << "Error opening markets DB: " << sqlite3_errmsg(db)
                      << std::endl;
            if (db) sqlite3_close(db);
            return result;
        }

        std::string in_clause = "(";
        bool first = true;
        for (const auto &[event_id, dates] : event_dates) {
            if (!first) in_clause += ",";
            in_clause += "'" + event_id + "'";
            first = false;
        }
        in_clause += ")";

        std::string sql =
            "SELECT conditionId, slug, eventId, outcomes, clobTokenIds, "
            "orderPriceMinTickSize, orderMinSize, negRisk, "
            "active, closed, acceptingOrders, "
            "acceptingOrdersTimestamp, eventStartTime, endDate, "
            "question, groupItemTitle, underlying_symbol "
            "FROM markets WHERE eventId IN " +
            in_clause;

        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) !=
            SQLITE_OK) {
            std::cerr << "Error preparing markets query: " << sqlite3_errmsg(db)
                      << std::endl;
            sqlite3_close(db);
            return result;
        }

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            PolymarketEventMeta meta;
            meta.condition_id = sqlite_col_text(stmt, 0);
            meta.slug = sqlite_col_text(stmt, 1);
            std::string event_id = sqlite_col_text(stmt, 2);
            std::string outcomes_json = sqlite_col_text(stmt, 3);
            std::string clob_token_ids_json = sqlite_col_text(stmt, 4);
            meta.order_price_min_tick_size = sqlite3_column_double(stmt, 5);
            meta.order_min_size = sqlite3_column_double(stmt, 6);
            meta.is_neg_risk = sqlite3_column_int(stmt, 7) != 0;
            meta.active = sqlite3_column_int(stmt, 8) != 0;
            meta.closed = sqlite3_column_int(stmt, 9) != 0;
            meta.accepting_orders = sqlite3_column_int(stmt, 10) != 0;
            std::string accepting_orders_ts = sqlite_col_text(stmt, 11);
            std::string event_start_time = sqlite_col_text(stmt, 12);
            std::string market_end_date = sqlite_col_text(stmt, 13);

            meta.outcome_names = parse_json_string_array(outcomes_json);
            meta.clob_token_ids = parse_json_string_array(clob_token_ids_json);
            meta.num_outcomes = static_cast<int>(meta.outcome_names.size());

            meta.accepting_orders_timestamp_ms =
                parse_iso8601_to_ms(accepting_orders_ts);
            meta.event_start_time_ms = parse_iso8601_to_ms(event_start_time);

            if (event_dates.count(event_id)) {
                meta.event_end_time_ms =
                    parse_iso8601_to_ms(event_dates[event_id].second);
            } else {
                meta.event_end_time_ms = parse_iso8601_to_ms(market_end_date);
            }

            meta.question = sqlite_col_text(stmt, 14);
            meta.group_item_title = sqlite_col_text(stmt, 15);
            meta.underlying_symbol = sqlite_col_text(stmt, 16);
            meta.strike_price =
                PolymarketEventParser::parse_strike_from_group_item_title(
                    meta.group_item_title);
            meta.product_type = PolymarketEventParser::parse_product_type(
                meta.group_item_title, meta.question);

            if (meta.event_end_time_ms < start_ts_ms) continue;
            if (meta.accepting_orders_timestamp_ms > end_ts_ms) continue;

            result.push_back(std::move(meta));
        }

        sqlite3_finalize(stmt);
        sqlite3_close(db);
    }

    // Load resolution prices and populate outcome_prices
    const auto resolved_prices =
        load_resolved_market_prices(resolved_markets_db_path, series_slug);
    for (auto &meta : result) {
        auto it = resolved_prices.find(meta.condition_id);
        if (it != resolved_prices.end()) {
            meta.outcome_prices = it->second;
        }
    }

    std::sort(result.begin(), result.end(),
              [](const PolymarketEventMeta &a, const PolymarketEventMeta &b) {
                  return a.accepting_orders_timestamp_ms <
                         b.accepting_orders_timestamp_ms;
              });

    return result;
}
