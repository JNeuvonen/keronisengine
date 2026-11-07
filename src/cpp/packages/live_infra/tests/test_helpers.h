#pragma once

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/reader.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <regex>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

static std::string get_data_dir_root() {
    const char* env = std::getenv("DATA_DIR_ROOT");
    if (!env || std::string(env).empty()) {
        throw std::runtime_error(
            "DATA_DIR_ROOT environment variable is not set");
    }
    return std::string(env);
}

static std::shared_ptr<arrow::Table> read_parquet_partitions(
    const std::string& base_path, long long start_ms, long long end_ms) {
    std::vector<fs::path> parquet_files;
    std::regex date_regex(R"(year=(\d{4})/month=(\d{2})/day=(\d{2}))");

    for (const auto& entry : fs::recursive_directory_iterator(base_path)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".parquet")
            continue;

        std::string path_str = entry.path().string();
        std::smatch match;
        if (std::regex_search(path_str, match, date_regex)) {
            int year = std::stoi(match[1].str());
            int month = std::stoi(match[2].str());
            int day = std::stoi(match[3].str());

            struct tm tm_start = {};
            tm_start.tm_year = year - 1900;
            tm_start.tm_mon = month - 1;
            tm_start.tm_mday = day;
            long long day_start_ms =
                static_cast<long long>(timegm(&tm_start)) * 1000LL;
            long long day_end_ms = day_start_ms + 86400000LL;

            if (day_end_ms >= start_ms && day_start_ms < end_ms) {
                parquet_files.push_back(entry.path());
            }
        }
    }

    std::sort(parquet_files.begin(), parquet_files.end());

    std::vector<std::shared_ptr<arrow::Table>> tables;
    arrow::MemoryPool* pool = arrow::default_memory_pool();

    for (const auto& file_path : parquet_files) {
        auto infile = arrow::io::ReadableFile::Open(file_path.string(), pool);
        if (!infile.ok()) continue;

        auto reader_result =
            parquet::arrow::OpenFile(infile.ValueOrDie(), pool);
        if (!reader_result.ok()) continue;

        std::shared_ptr<arrow::Table> table;
        auto status = reader_result.ValueOrDie()->ReadTable(&table);
        if (!status.ok()) continue;

        tables.push_back(table);
    }

    if (tables.empty()) return nullptr;

    auto result = arrow::ConcatenateTables(tables);
    if (!result.ok()) return nullptr;
    return result.ValueOrDie();
}

static std::vector<double> extract_double_column(
    const std::shared_ptr<arrow::Table>& table, const std::string& col_name) {
    auto chunked = table->GetColumnByName(col_name);
    if (!chunked) {
        throw std::runtime_error("Column not found: " + col_name);
    }

    std::vector<double> result;
    result.reserve(table->num_rows());

    for (int c = 0; c < chunked->num_chunks(); ++c) {
        auto chunk = chunked->chunk(c);

        if (auto double_arr =
                std::dynamic_pointer_cast<arrow::DoubleArray>(chunk)) {
            for (int64_t i = 0; i < double_arr->length(); ++i) {
                if (double_arr->IsNull(i)) {
                    result.push_back(std::numeric_limits<double>::quiet_NaN());
                } else {
                    result.push_back(double_arr->Value(i));
                }
            }
        } else if (auto float_arr =
                       std::dynamic_pointer_cast<arrow::FloatArray>(chunk)) {
            for (int64_t i = 0; i < float_arr->length(); ++i) {
                if (float_arr->IsNull(i)) {
                    result.push_back(std::numeric_limits<double>::quiet_NaN());
                } else {
                    result.push_back(
                        static_cast<double>(float_arr->Value(i)));
                }
            }
        } else {
            throw std::runtime_error("Column " + col_name +
                                     " is not a float/double type, got: " +
                                     chunk->type()->ToString());
        }
    }
    return result;
}

static std::vector<long long> extract_int64_column(
    const std::shared_ptr<arrow::Table>& table, const std::string& col_name) {
    auto chunked = table->GetColumnByName(col_name);
    if (!chunked) {
        throw std::runtime_error("Column not found: " + col_name);
    }

    std::vector<long long> result;
    result.reserve(table->num_rows());

    for (int c = 0; c < chunked->num_chunks(); ++c) {
        auto chunk = chunked->chunk(c);

        if (auto int_arr =
                std::dynamic_pointer_cast<arrow::Int64Array>(chunk)) {
            for (int64_t i = 0; i < int_arr->length(); ++i) {
                result.push_back(int_arr->Value(i));
            }
        } else if (auto ts_arr =
                       std::dynamic_pointer_cast<arrow::TimestampArray>(
                           chunk)) {
            for (int64_t i = 0; i < ts_arr->length(); ++i) {
                result.push_back(ts_arr->Value(i));
            }
        } else {
            throw std::runtime_error("Column " + col_name +
                                     " is not int64/timestamp type, got: " +
                                     chunk->type()->ToString());
        }
    }
    return result;
}

static std::vector<int8_t> extract_int8_column(
    const std::shared_ptr<arrow::Table>& table, const std::string& col_name) {
    auto chunked = table->GetColumnByName(col_name);
    if (!chunked) {
        throw std::runtime_error("Column not found: " + col_name);
    }

    std::vector<int8_t> result;
    result.reserve(table->num_rows());

    for (int c = 0; c < chunked->num_chunks(); ++c) {
        auto chunk = chunked->chunk(c);

        if (auto arr = std::dynamic_pointer_cast<arrow::Int8Array>(chunk)) {
            for (int64_t i = 0; i < arr->length(); ++i) {
                result.push_back(arr->Value(i));
            }
        } else {
            throw std::runtime_error("Column " + col_name +
                                     " is not int8 type, got: " +
                                     chunk->type()->ToString());
        }
    }
    return result;
}

static std::vector<int32_t> extract_int32_column(
    const std::shared_ptr<arrow::Table>& table, const std::string& col_name) {
    auto chunked = table->GetColumnByName(col_name);
    if (!chunked) {
        throw std::runtime_error("Column not found: " + col_name);
    }

    std::vector<int32_t> result;
    result.reserve(table->num_rows());

    for (int c = 0; c < chunked->num_chunks(); ++c) {
        auto chunk = chunked->chunk(c);

        if (auto arr = std::dynamic_pointer_cast<arrow::Int32Array>(chunk)) {
            for (int64_t i = 0; i < arr->length(); ++i) {
                result.push_back(arr->Value(i));
            }
        } else {
            throw std::runtime_error("Column " + col_name +
                                     " is not int32 type, got: " +
                                     chunk->type()->ToString());
        }
    }
    return result;
}

struct CompareResult {
    int matched = 0;
    int mismatches = 0;
    int skipped_nan = 0;
};

static CompareResult compare_on_timestamp(
    const std::vector<long long>& bar_timestamps,
    const std::vector<double>& cpp_values,
    const std::vector<long long>& feature_timestamps,
    const std::vector<double>& python_values,
    long long cmp_start_ms,
    long long cmp_end_ms,
    double rel_tol = 1e-8,
    double abs_tol = 1e-12) {
    std::unordered_map<long long, size_t> bar_ts_to_idx;
    bar_ts_to_idx.reserve(bar_timestamps.size());
    for (size_t i = 0; i < bar_timestamps.size(); ++i) {
        bar_ts_to_idx[bar_timestamps[i]] = i;
    }

    CompareResult res;

    for (size_t fi = 0; fi < feature_timestamps.size(); ++fi) {
        if (feature_timestamps[fi] < cmp_start_ms ||
            feature_timestamps[fi] >= cmp_end_ms) {
            continue;
        }

        if (std::isnan(python_values[fi])) {
            res.skipped_nan++;
            continue;
        }

        auto it = bar_ts_to_idx.find(feature_timestamps[fi]);
        if (it == bar_ts_to_idx.end()) continue;

        size_t bi = it->second;
        double cpp_val = cpp_values[bi];
        double py_val = python_values[fi];

        if (std::isnan(cpp_val)) {
            res.mismatches++;
            if (res.mismatches <= 5) {
                std::cout << "Mismatch (C++ NaN): ts=" << feature_timestamps[fi]
                          << " py=" << py_val << std::endl;
            }
            continue;
        }

        double diff = std::abs(cpp_val - py_val);
        if (diff < abs_tol) {
            res.matched++;
            continue;
        }
        double denom = std::max(std::abs(py_val), 1e-15);
        if (diff / denom > rel_tol) {
            res.mismatches++;
            if (res.mismatches <= 5) {
                std::cout << "Mismatch: ts=" << feature_timestamps[fi]
                          << " cpp=" << cpp_val << " py=" << py_val
                          << " diff=" << diff << std::endl;
            }
        }

        res.matched++;
    }

    std::cout << "Matched: " << res.matched << " Skipped NaN: "
              << res.skipped_nan << " Mismatches: " << res.mismatches
              << std::endl;

    return res;
}

static CompareResult compare_int64_on_timestamp(
    const std::vector<long long>& bar_timestamps,
    const std::vector<long long>& cpp_values,
    const std::vector<long long>& feature_timestamps,
    const std::vector<long long>& python_values,
    long long cmp_start_ms,
    long long cmp_end_ms) {
    std::unordered_map<long long, size_t> bar_ts_to_idx;
    bar_ts_to_idx.reserve(bar_timestamps.size());
    for (size_t i = 0; i < bar_timestamps.size(); ++i) {
        bar_ts_to_idx[bar_timestamps[i]] = i;
    }

    CompareResult res;

    for (size_t fi = 0; fi < feature_timestamps.size(); ++fi) {
        if (feature_timestamps[fi] < cmp_start_ms ||
            feature_timestamps[fi] >= cmp_end_ms) {
            continue;
        }

        auto it = bar_ts_to_idx.find(feature_timestamps[fi]);
        if (it == bar_ts_to_idx.end()) continue;

        size_t bi = it->second;
        long long cpp_val = cpp_values[bi];
        long long py_val = python_values[fi];

        if (cpp_val != py_val) {
            res.mismatches++;
            if (res.mismatches <= 5) {
                std::cout << "Mismatch: ts=" << feature_timestamps[fi]
                          << " cpp=" << cpp_val << " py=" << py_val
                          << std::endl;
            }
        }

        res.matched++;
    }

    std::cout << "Matched: " << res.matched << " Mismatches: "
              << res.mismatches << std::endl;

    return res;
}

static CompareResult compare_int32_on_timestamp(
    const std::vector<long long>& bar_timestamps,
    const std::vector<int>& cpp_values,
    const std::vector<long long>& feature_timestamps,
    const std::vector<int32_t>& python_values,
    long long cmp_start_ms,
    long long cmp_end_ms) {
    std::unordered_map<long long, size_t> bar_ts_to_idx;
    bar_ts_to_idx.reserve(bar_timestamps.size());
    for (size_t i = 0; i < bar_timestamps.size(); ++i) {
        bar_ts_to_idx[bar_timestamps[i]] = i;
    }

    CompareResult res;

    for (size_t fi = 0; fi < feature_timestamps.size(); ++fi) {
        if (feature_timestamps[fi] < cmp_start_ms ||
            feature_timestamps[fi] >= cmp_end_ms) {
            continue;
        }

        auto it = bar_ts_to_idx.find(feature_timestamps[fi]);
        if (it == bar_ts_to_idx.end()) continue;

        size_t bi = it->second;
        int cpp_val = cpp_values[bi];
        int py_val = static_cast<int>(python_values[fi]);

        if (cpp_val != py_val) {
            res.mismatches++;
            if (res.mismatches <= 5) {
                std::cout << "Mismatch: ts=" << feature_timestamps[fi]
                          << " cpp=" << cpp_val << " py=" << py_val
                          << std::endl;
            }
        }

        res.matched++;
    }

    std::cout << "Matched: " << res.matched << " Mismatches: "
              << res.mismatches << std::endl;

    return res;
}

static size_t find_bar_start_idx(const std::vector<long long>& bar_timestamps,
                                 long long target_ms) {
    for (size_t i = 0; i < bar_timestamps.size(); ++i) {
        if (bar_timestamps[i] >= target_ms) return i;
    }
    return bar_timestamps.size();
}
