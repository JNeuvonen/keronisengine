#include <filesystem>
#include <arrow/api.h>

#include <catch2/catch_all.hpp>
#include <cmath>
#include <iostream>
#include <vector>

#include "parquet_utils.h"
#include "technical_analysis.h"

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
                    result.push_back(static_cast<double>(float_arr->Value(i)));
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
            // Arrow Timestamp type — extract raw int64 values (milliseconds)
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

// Root of the local market data; tests that need it skip when it is unset.
static std::string market_data_root() {
    const char* root = std::getenv("DATA_DIR_ROOT");
    return root ? root : "";
}

TEST_CASE("ROCP close_price matches Python pipeline output",
          "[test_technical_analysis]") {
    const std::string bars_path = market_data_root() +
        "/derived/bars/binance/spot/time/100_ms/BTCUSDT/parquets";
    const std::string feature_path = market_data_root() +
        "/derived/namespaces/2025/features/bars/binance/spot/time/100_ms/"
        "BTCUSDT/rocp_close_price_1/parquets";
    if (!std::filesystem::exists(bars_path) ||
        !std::filesystem::exists(feature_path)) {
        SKIP("needs BTCUSDT 100ms bars and features under DATA_DIR_ROOT");
    }

    // Load 1 day: 2025-01-02
    auto start_time = std::chrono::system_clock::time_point(
        std::chrono::milliseconds(1735776000000LL));  // 2025-01-02 00:00:00 UTC
    auto end_time = std::chrono::system_clock::time_point(
        std::chrono::milliseconds(1735862400000LL));  // 2025-01-03 00:00:00 UTC

    std::cout << "Loading bar data..." << std::endl;
    auto bars_table = ParquetUtils::scan_partitions_fast(
        bars_path, "kline_close_time_ms", start_time, end_time);
    REQUIRE(bars_table != nullptr);
    REQUIRE(bars_table->num_rows() > 0);
    std::cout << "Bar rows: " << bars_table->num_rows() << std::endl;

    std::cout << "Loading Python feature data..." << std::endl;
    auto feature_table = ParquetUtils::scan_partitions_fast(
        feature_path, "timestamp_ms", start_time, end_time);
    REQUIRE(feature_table != nullptr);
    REQUIRE(feature_table->num_rows() > 0);
    std::cout << "Feature rows: " << feature_table->num_rows() << std::endl;

    // Extract columns
    auto bar_timestamps =
        extract_int64_column(bars_table, "kline_close_time_ms");
    auto close_prices = extract_double_column(bars_table, "close_price");
    auto feature_timestamps =
        extract_int64_column(feature_table, "timestamp_ms");
    auto python_rocp =
        extract_double_column(feature_table, "rocp_close_price_1");

    // Compute C++ ROCP for full series (period=1)
    const int period = 1;
    std::vector<double> cpp_rocp(close_prices.size(),
                                 std::numeric_limits<double>::quiet_NaN());
    for (size_t i = period; i < close_prices.size(); ++i) {
        double prev = close_prices[i - period];
        if (prev != 0.0 && !std::isnan(prev) && !std::isinf(prev)) {
            cpp_rocp[i] = (close_prices[i] / prev) - 1.0;
        }
    }

    // Build timestamp -> index map for bar data
    std::unordered_map<long long, size_t> bar_ts_to_idx;
    bar_ts_to_idx.reserve(bar_timestamps.size());
    for (size_t i = 0; i < bar_timestamps.size(); ++i) {
        bar_ts_to_idx[bar_timestamps[i]] = i;
    }

    // Compare: join on timestamp
    int matched = 0;
    int mismatches = 0;
    int skipped_nan = 0;
    const double rel_tol = 1e-9;

    for (size_t fi = 0; fi < feature_timestamps.size(); ++fi) {
        // Skip NaN Python values (warmup rows)
        if (std::isnan(python_rocp[fi])) {
            skipped_nan++;
            continue;
        }

        auto it = bar_ts_to_idx.find(feature_timestamps[fi]);
        if (it == bar_ts_to_idx.end()) {
            continue;  // No matching bar timestamp
        }

        size_t bi = it->second;
        if (bi < static_cast<size_t>(period)) {
            continue;  // Not enough data for C++ computation
        }

        double cpp_val = cpp_rocp[bi];
        double py_val = python_rocp[fi];

        if (std::isnan(cpp_val)) {
            mismatches++;
            if (mismatches <= 5) {
                std::cout << "Mismatch (C++ NaN): ts=" << feature_timestamps[fi]
                          << " py=" << py_val << std::endl;
            }
            continue;
        }

        // Relative tolerance check
        double diff = std::abs(cpp_val - py_val);
        double denom = std::max(std::abs(py_val), 1e-15);
        if (diff / denom > rel_tol) {
            mismatches++;
            if (mismatches <= 5) {
                std::cout << "Mismatch: ts=" << feature_timestamps[fi]
                          << " cpp=" << cpp_val << " py=" << py_val
                          << " diff=" << diff << std::endl;
            }
        }

        matched++;
    }

    std::cout << "Matched rows: " << matched << std::endl;
    std::cout << "Skipped NaN: " << skipped_nan << std::endl;
    std::cout << "Mismatches: " << mismatches << std::endl;

    REQUIRE(matched > 0);
    REQUIRE(mismatches == 0);
}
