#include <dataset.h>
#include <generic_utils.h>

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace Datasets {

constexpr char DATASET_TABLE_NAME[] = "data";

TsToRowValuesMap read_table_into_map(
    const std::string &db_path, const std::string &table_name,
    const std::string &key_column, std::optional<long long> start_ts_ms,
    std::optional<long long> end_ts_ms,
    std::optional<std::vector<std::string>> used_columns) {
    sqlite3 *db;
    sqlite3_stmt *stmt;
    TsToRowValuesMap result;

    std::string sql = "SELECT ";
    if (used_columns && !used_columns->empty()) {
        sql += key_column + ", ";
        sql += CommonUtils::join(*used_columns, ", ");
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
    if (start_ts_ms) {
        sqlite3_bind_int64(stmt, bind_index++, *start_ts_ms);
    }
    if (end_ts_ms) {
        sqlite3_bind_int64(stmt, bind_index++, *end_ts_ms);
    }

    int key_column_index = -1;
    int columns = sqlite3_column_count(stmt);
    for (int i = 0; i < columns; i++) {
        if (key_column == sqlite3_column_name(stmt, i)) {
            key_column_index = i;
            break;
        }
    }

    if (key_column_index == -1) {
        std::cerr << "Key column '" << key_column << "' not found in table."
                  << std::endl;
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return result;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ColToValueMap row_map;
        int64_t key_value = 0;

        for (int i = 0; i < columns; i++) {
            const char *column_name = sqlite3_column_name(stmt, i);
            int column_type = sqlite3_column_type(stmt, i);

            if (i == key_column_index) {
                key_value = sqlite3_column_int64(stmt, i);
            } else {
                if (column_type == SQLITE_INTEGER) {
                    row_map[column_name] = sqlite3_column_int64(stmt, i);
                } else if (column_type == SQLITE_FLOAT) {
                    row_map[column_name] = sqlite3_column_double(stmt, i);
                } else if (column_type == SQLITE_TEXT) {
                    const char *text_value = reinterpret_cast<const char *>(
                        sqlite3_column_text(stmt, i));
                    if (text_value) {
                        try {
                            double value_as_double = std::stod(text_value);
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
                    }
                } else if (column_type == SQLITE_NULL) {
                    row_map[column_name] = 0.0;
                }
            }
        }

        if (key_value != 0) {
            result[key_value] = row_map;
        }
    }

    // Clean up
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    return result;
}

int64_t get_table_entries_time_delta(const std::string &db_path,
                                     const std::string &table_name,
                                     const std::string &time_ms_col) {
    sqlite3 *db;
    sqlite3_stmt *stmt;
    int64_t min_delta = std::numeric_limits<int64_t>::max();
    std::string sql = "SELECT " + time_ms_col + " FROM " + table_name +
                      " ORDER BY " + time_ms_col + " ASC LIMIT 100";

    if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) {
        std::cerr << "Error opening database: " << sqlite3_errmsg(db)
                  << std::endl;
        return min_delta;
    }

    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Error preparing statement: " << sqlite3_errmsg(db)
                  << std::endl;
        sqlite3_close(db);
        return min_delta;
    }

    int64_t previous_time = 0;
    bool first_row = true;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int64_t current_time = sqlite3_column_int64(stmt, 0);

        if (!first_row) {
            int64_t delta = current_time - previous_time;

            if (delta < min_delta) {
                min_delta = delta;
            }
        }

        previous_time = current_time;
        first_row = false;
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    return min_delta;
}

unsigned int get_datasets_min_candle_interval(
    const std::vector<std::string> &datasets, const std::string &time_ms_col) {
    int64_t min_delta = std::numeric_limits<int64_t>::max();

    for (const auto &dataset_path : datasets) {
        try {
            int64_t delta = get_table_entries_time_delta(
                dataset_path, DATASET_TABLE_NAME, time_ms_col);
            if (delta > 0) {
                min_delta = std::min(min_delta, delta);
            }
        } catch (const std::exception &e) {
            std::cerr << "Error processing dataset: " << dataset_path << ". "
                      << e.what() << std::endl;
        }
    }

    if (min_delta == std::numeric_limits<int64_t>::max()) {
        std::cerr << "No valid intervals found in the datasets." << std::endl;
        return -1;
    }

    return static_cast<int>(min_delta);
}

double get_dataset_first_price(const std::string &db_path,
                               const std::string &table_name,
                               const std::string &price_col) {
    sqlite3 *db;
    sqlite3_stmt *stmt;
    double first_price = std::numeric_limits<double>::quiet_NaN();

    std::string sql = "SELECT " + price_col + " FROM " + table_name +
                      " WHERE " + price_col +
                      " IS NOT NULL ORDER BY ROWID ASC LIMIT 1";

    if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) {
        std::cerr << "Error opening database: " << sqlite3_errmsg(db)
                  << std::endl;
        return first_price;
    }

    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Error preparing statement: " << sqlite3_errmsg(db)
                  << std::endl;
        sqlite3_close(db);
        return first_price;
    }

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        int column_type = sqlite3_column_type(stmt, 0);

        if (column_type == SQLITE_INTEGER) {
            first_price = static_cast<double>(sqlite3_column_int64(stmt, 0));
        } else if (column_type == SQLITE_FLOAT) {
            first_price = sqlite3_column_double(stmt, 0);
        } else if (column_type == SQLITE_TEXT) {
            const char *text_value =
                reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
            try {
                first_price = std::stod(text_value);
            } catch (...) {
                first_price = std::numeric_limits<double>::quiet_NaN();
            }
        }
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return first_price;
}

std::unordered_map<std::string, double> get_datasets_first_prices(
    const std::vector<std::string> &datasets, const std::string &price_col) {
    std::unordered_map<std::string, double> first_prices;

    for (const auto &dataset_path : datasets) {
        try {
            double first_price = get_dataset_first_price(
                dataset_path, DATASET_TABLE_NAME, price_col);
            if (!std::isnan(first_price)) {
                first_prices[dataset_path] = first_price;
            } else {
                std::cerr << "No valid price found in dataset, setting first "
                             "price to 1: "
                          << dataset_path << std::endl;
                first_prices[dataset_path] = 1;
            }
        } catch (const std::exception &e) {
            std::cerr << "Error processing dataset, setting first price to 1:"
                      << dataset_path << ". " << e.what() << std::endl;
            first_prices[dataset_path] = 1;
        }
    }

    return first_prices;
}

Datasets::DatasetNameToDatasetMap get_datasets_map(
    std::vector<std::string> dataset_names,
    std::optional<long long> start_ts_ms, std::optional<long long> end_ts_ms,
    std::optional<std::vector<std::string>> used_columns) {
    Datasets::DatasetNameToDatasetMap ret;
    for (auto item : dataset_names) {
        auto dataset_map =
            Datasets::read_table_into_map(item, "data", "kline_open_time_ms",
                                          start_ts_ms, end_ts_ms, used_columns);

        ret[item] = dataset_map;
    }
    return ret;
}

std::pair<int64_t, int64_t> get_datasets_start_and_end_ms(
    const std::vector<std::string> &datasets, const std::string &table_name,
    const std::string &column_name) {
    int64_t datasets_min = std::numeric_limits<int64_t>::max();
    int64_t datasets_max = std::numeric_limits<int64_t>::min();

    for (const auto &dataset_path : datasets) {
        sqlite3 *db = nullptr;
        sqlite3_stmt *stmt = nullptr;

        std::string sql = "SELECT MIN(" + column_name + "), MAX(" +
                          column_name + ") FROM " + table_name;

        if (sqlite3_open(dataset_path.c_str(), &db) != SQLITE_OK) {
            std::cerr << "Error opening database: " << sqlite3_errmsg(db)
                      << std::endl;
            continue;
        }

        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) !=
            SQLITE_OK) {
            std::cerr << "Error preparing statement: " << sqlite3_errmsg(db)
                      << std::endl;
            sqlite3_close(db);
            continue;
        }

        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int column_type_min = sqlite3_column_type(stmt, 0);
            int column_type_max = sqlite3_column_type(stmt, 1);

            if (column_type_min != SQLITE_NULL) {
                int64_t local_min = sqlite3_column_int64(stmt, 0);
                datasets_min = std::min(datasets_min, local_min);
            }

            if (column_type_max != SQLITE_NULL) {
                int64_t local_max = sqlite3_column_int64(stmt, 1);
                datasets_max = std::max(datasets_max, local_max);
            }
        } else {
            std::cerr << "Error executing statement: " << sqlite3_errmsg(db)
                      << std::endl;
        }

        sqlite3_finalize(stmt);
        sqlite3_close(db);
    }

    if (datasets_min == std::numeric_limits<int64_t>::max()) {
        datasets_min = -1;
    }
    if (datasets_max == std::numeric_limits<int64_t>::min()) {
        datasets_max = -1;
    }

    return {datasets_min, datasets_max};
}

std::pair<std::optional<int64_t>, std::optional<int64_t>>
read_first_and_last_values(const std::string &db_path,
                           const std::string &table_name,
                           const std::string &column_name) {
    sqlite3 *db = nullptr;
    sqlite3_stmt *stmt = nullptr;
    std::pair<std::optional<int64_t>, std::optional<int64_t>> result{
        std::nullopt, std::nullopt};

    std::string sql = "SELECT MIN(" + column_name + "), MAX(" + column_name +
                      ") FROM " + table_name;

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

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        int column_type_min = sqlite3_column_type(stmt, 0);
        int column_type_max = sqlite3_column_type(stmt, 1);

        if (column_type_min != SQLITE_NULL) {
            result.first = sqlite3_column_int64(stmt, 0);
        }
        if (column_type_max != SQLITE_NULL) {
            result.second = sqlite3_column_int64(stmt, 1);
        }
    } else {
        std::cerr << "Error executing statement: " << sqlite3_errmsg(db)
                  << std::endl;
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    return result;
}

}  // namespace Datasets
