#pragma once

#include <sqlite3.h>

#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace Datasets {

using ValueVariant = std::variant<int64_t, double>;
using ColToValueMap = std::unordered_map<std::string, ValueVariant>;
using TsToRowValuesMap = std::unordered_map<int64_t, ColToValueMap>;
using DatasetNameToDatasetMap =
    std::unordered_map<std::string, TsToRowValuesMap>;

TsToRowValuesMap read_table_into_map(
    const std::string &db_path, const std::string &table_name,
    const std::string &key_column, std::optional<long long> start_ts_ms,
    std::optional<long long> end_ts_ms,
    std::optional<std::vector<std::string>> used_columns);

unsigned int get_datasets_min_candle_interval(
    const std::vector<std::string> &datasets, const std::string &time_ms_col);

int64_t get_table_entries_time_delta(const std::string &db_path,
                                     const std::string &table_name,
                                     const std::string &time_ms_col);

double get_dataset_first_price(const std::string &db_path,
                               const std::string &table_name,
                               const std::string &price_col);

std::pair<std::optional<int64_t>, std::optional<int64_t>>
read_first_and_last_values(const std::string &db_path,
                           const std::string &table_name,
                           const std::string &column_name);

std::pair<int64_t, int64_t> get_datasets_start_and_end_ms(
    const std::vector<std::string> &datasets, const std::string &table_name,
    const std::string &column_name);

Datasets::DatasetNameToDatasetMap get_datasets_map(
    std::vector<std::string> dataset_names,
    std::optional<long long> start_ts_ms, std::optional<long long> end_ts_ms,
    std::optional<std::vector<std::string>> used_columns);

std::unordered_map<std::string, double> get_datasets_first_prices(
    const std::vector<std::string> &datasets, const std::string &price_col);

};  // namespace Datasets
