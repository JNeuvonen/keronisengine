#include <catch2/catch_all.hpp>

#include <cmath>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include "technical_analysis.h"
#include "test_helpers.h"

// Common paths
static const std::string BARS_SUBPATH =
    "/derived/bars/binance/spot/time/100_ms/BTCUSDT/parquets";
static const std::string FEATURES_SUBPATH =
    "/derived/namespaces/2025/features/bars/binance/spot/time/100_ms/"
    "BTCUSDT/";

// Comparison window: 2025-01-02
static const long long CMP_START_MS = 1735776000000LL;  // 2025-01-02 00:00 UTC
static const long long CMP_END_MS = 1735862400000LL;    // 2025-01-03 00:00 UTC

// Local wrappers that inject this file's comparison window
static CompareResult compare_on_timestamp(
    const std::vector<long long>& bar_ts,
    const std::vector<double>& cpp_vals,
    const std::vector<long long>& feat_ts,
    const std::vector<double>& py_vals,
    double rel_tol = 1e-8,
    double abs_tol = 1e-12) {
    return ::compare_on_timestamp(bar_ts, cpp_vals, feat_ts, py_vals,
                                  CMP_START_MS, CMP_END_MS, rel_tol, abs_tol);
}

static CompareResult compare_int64_on_timestamp(
    const std::vector<long long>& bar_ts,
    const std::vector<long long>& cpp_vals,
    const std::vector<long long>& feat_ts,
    const std::vector<long long>& py_vals) {
    return ::compare_int64_on_timestamp(bar_ts, cpp_vals, feat_ts, py_vals,
                                        CMP_START_MS, CMP_END_MS);
}

static CompareResult compare_int8_on_timestamp(
    const std::vector<long long>& bar_ts,
    const std::vector<int>& cpp_vals,
    const std::vector<long long>& feat_ts,
    const std::vector<int8_t>& py_vals) {
    return ::compare_int32_on_timestamp(bar_ts, cpp_vals, feat_ts,
                                        std::vector<int32_t>(py_vals.begin(), py_vals.end()),
                                        CMP_START_MS, CMP_END_MS);
}

// =============================================================================
// Tests
// =============================================================================

TEST_CASE("ROCP close_price matches Python pipeline output",
          "[test_feature_gen][rocp_close_price]") {
    const std::string data_root = get_data_dir_root();
    const std::string bars_path = data_root + BARS_SUBPATH;
    const std::string feature_path =
        data_root + FEATURES_SUBPATH + "rocp_close_price_1/parquets";

    auto bars_table = read_parquet_partitions(bars_path, CMP_START_MS, CMP_END_MS);
    REQUIRE(bars_table != nullptr);

    auto feature_table = read_parquet_partitions(feature_path, CMP_START_MS, CMP_END_MS);
    REQUIRE(feature_table != nullptr);

    auto bar_timestamps = extract_int64_column(bars_table, "kline_close_time_ms");
    auto close_prices = extract_double_column(bars_table, "close_price");
    auto feature_timestamps = extract_int64_column(feature_table, "timestamp_ms");
    auto python_rocp = extract_double_column(feature_table, "rocp_close_price_1");

    const int period = 1;
    std::vector<double> cpp_rocp(close_prices.size(),
                                 std::numeric_limits<double>::quiet_NaN());
    for (size_t i = period; i < close_prices.size(); ++i) {
        double prev = close_prices[i - period];
        if (prev != 0.0 && !std::isnan(prev) && !std::isinf(prev)) {
            cpp_rocp[i] = (close_prices[i] / prev) - 1.0;
        }
    }

    auto res = compare_on_timestamp(bar_timestamps, cpp_rocp,
                                    feature_timestamps, python_rocp);
    REQUIRE(res.matched > 0);
    REQUIRE(res.mismatches == 0);
}

TEST_CASE("ROCP close_price period=600 matches Python",
          "[test_feature_gen][rocp_close_price]") {
    const std::string data_root = get_data_dir_root();
    const std::string bars_path = data_root + BARS_SUBPATH;
    const std::string feature_path =
        data_root + FEATURES_SUBPATH + "rocp_close_price_600/parquets";

    // Need 600 bars lookback = 60s = 60000ms before comparison window
    long long bars_start = CMP_START_MS - 600LL * 100LL;
    auto bars_table = read_parquet_partitions(bars_path, bars_start, CMP_END_MS);
    REQUIRE(bars_table != nullptr);

    auto feature_table = read_parquet_partitions(feature_path, CMP_START_MS, CMP_END_MS);
    REQUIRE(feature_table != nullptr);

    auto bar_timestamps = extract_int64_column(bars_table, "kline_close_time_ms");
    auto close_prices = extract_double_column(bars_table, "close_price");
    auto feature_timestamps = extract_int64_column(feature_table, "timestamp_ms");
    auto python_rocp = extract_double_column(feature_table, "rocp_close_price_600");

    const int period = 600;
    std::vector<double> cpp_rocp(close_prices.size(),
                                 std::numeric_limits<double>::quiet_NaN());
    for (size_t i = period; i < close_prices.size(); ++i) {
        double prev = close_prices[i - period];
        if (prev != 0.0 && !std::isnan(prev) && !std::isinf(prev)) {
            cpp_rocp[i] = (close_prices[i] / prev) - 1.0;
        }
    }

    auto res = compare_on_timestamp(bar_timestamps, cpp_rocp,
                                    feature_timestamps, python_rocp);
    REQUIRE(res.matched > 0);
    REQUIRE(res.mismatches == 0);
}

TEST_CASE("normalized_volatility close_price lookback=600 matches Python",
          "[test_feature_gen][norm_vol_close_price]") {
    const std::string data_root = get_data_dir_root();
    const std::string bars_path = data_root + BARS_SUBPATH;
    const std::string feature_path =
        data_root + FEATURES_SUBPATH + "norm_vol_close_price_600/parquets";

    const int lookback = 600;
    long long bars_start = CMP_START_MS - static_cast<long long>(lookback) * 100LL;
    auto bars_table = read_parquet_partitions(bars_path, bars_start, CMP_END_MS);
    REQUIRE(bars_table != nullptr);

    auto feature_table = read_parquet_partitions(feature_path, CMP_START_MS, CMP_END_MS);
    REQUIRE(feature_table != nullptr);

    auto bar_timestamps = extract_int64_column(bars_table, "kline_close_time_ms");
    auto close_prices = extract_double_column(bars_table, "close_price");
    auto feature_timestamps = extract_int64_column(feature_table, "timestamp_ms");
    auto python_values = extract_double_column(feature_table, "norm_vol_close_price_600");

    std::vector<double> cpp_values(close_prices.size(),
                                   std::numeric_limits<double>::quiet_NaN());
    for (size_t i = lookback - 1; i < close_prices.size(); ++i) {
        std::span<const double> span{close_prices.data(), i + 1};
        cpp_values[i] = TechnicalAnalysis::normalized_volatility(span, lookback);
    }

    // Slightly relaxed tolerance: C++ two-pass accumulation vs Polars streaming
    // causes ~1e-8 relative / ~1e-11 absolute differences
    auto res = compare_on_timestamp(bar_timestamps, cpp_values,
                                    feature_timestamps, python_values, 1e-7, 1e-10);
    REQUIRE(res.matched > 0);
    REQUIRE(res.mismatches == 0);
}

TEST_CASE("signed_volume lookback=10 matches Python",
          "[test_feature_gen][signed_volume]") {
    const std::string data_root = get_data_dir_root();
    const std::string bars_path = data_root + BARS_SUBPATH;
    const std::string feature_path =
        data_root + FEATURES_SUBPATH + "signed_volume_10/parquets";

    const int lookback = 10;
    long long bars_start = CMP_START_MS - static_cast<long long>(lookback) * 100LL;
    auto bars_table = read_parquet_partitions(bars_path, bars_start, CMP_END_MS);
    REQUIRE(bars_table != nullptr);

    auto feature_table = read_parquet_partitions(feature_path, CMP_START_MS, CMP_END_MS);
    REQUIRE(feature_table != nullptr);

    auto bar_timestamps = extract_int64_column(bars_table, "kline_close_time_ms");
    auto close_prices = extract_double_column(bars_table, "close_price");
    auto open_prices = extract_double_column(bars_table, "open_price");
    auto volumes = extract_double_column(bars_table, "volume");
    auto feature_timestamps = extract_int64_column(feature_table, "timestamp_ms");
    auto python_values = extract_double_column(feature_table, "signed_volume_10");

    std::vector<double> cpp_values(close_prices.size(),
                                   std::numeric_limits<double>::quiet_NaN());
    for (size_t i = lookback - 1; i < close_prices.size(); ++i) {
        std::span<const double> cp{close_prices.data(), i + 1};
        std::span<const double> op{open_prices.data(), i + 1};
        std::span<const double> vol{volumes.data(), i + 1};
        cpp_values[i] = TechnicalAnalysis::signed_volume(cp, op, vol, lookback);
    }

    // Use larger abs_tol: when vol_sum~0, C++ returns exact 0 while Python
    // returns a tiny float due to floating-point division
    auto res = compare_on_timestamp(bar_timestamps, cpp_values,
                                    feature_timestamps, python_values, 1e-8, 1e-10);
    REQUIRE(res.matched > 0);
    REQUIRE(res.mismatches == 0);
}

TEST_CASE("volume_acceleration 3_15 matches Python",
          "[test_feature_gen][volume_acceleration]") {
    const std::string data_root = get_data_dir_root();
    const std::string bars_path = data_root + BARS_SUBPATH;
    const std::string feature_path =
        data_root + FEATURES_SUBPATH + "volume_acceleration_3_15/parquets";

    const int short_window = 3;
    const int long_window = 15;
    long long bars_start = CMP_START_MS - static_cast<long long>(long_window) * 100LL;
    auto bars_table = read_parquet_partitions(bars_path, bars_start, CMP_END_MS);
    REQUIRE(bars_table != nullptr);

    auto feature_table = read_parquet_partitions(feature_path, CMP_START_MS, CMP_END_MS);
    REQUIRE(feature_table != nullptr);

    auto bar_timestamps = extract_int64_column(bars_table, "kline_close_time_ms");
    auto volumes = extract_double_column(bars_table, "volume");
    auto feature_timestamps = extract_int64_column(feature_table, "timestamp_ms");
    auto python_values = extract_double_column(feature_table, "volume_acceleration_3_15");

    std::vector<double> cpp_values(volumes.size(),
                                   std::numeric_limits<double>::quiet_NaN());
    for (size_t i = long_window - 1; i < volumes.size(); ++i) {
        std::span<const double> span{volumes.data(), i + 1};
        cpp_values[i] = TechnicalAnalysis::volume_acceleration(span, short_window, long_window);
    }

    auto res = compare_on_timestamp(bar_timestamps, cpp_values,
                                    feature_timestamps, python_values);
    REQUIRE(res.matched > 0);
    REQUIRE(res.mismatches == 0);
}

TEST_CASE("buy_pressure_vol_vs_sma 3_50 matches Python",
          "[test_feature_gen][buy_pressure_vol_vs_sma]") {
    const std::string data_root = get_data_dir_root();
    const std::string bars_path = data_root + BARS_SUBPATH;
    const std::string feature_path =
        data_root + FEATURES_SUBPATH + "buy_pressure_vol_vs_sma_3_50/parquets";

    const int short_window = 3;
    const int long_window = 50;
    long long bars_start = CMP_START_MS - static_cast<long long>(long_window) * 100LL;
    auto bars_table = read_parquet_partitions(bars_path, bars_start, CMP_END_MS);
    REQUIRE(bars_table != nullptr);

    auto feature_table = read_parquet_partitions(feature_path, CMP_START_MS, CMP_END_MS);
    REQUIRE(feature_table != nullptr);

    auto bar_timestamps = extract_int64_column(bars_table, "kline_close_time_ms");
    auto volumes = extract_double_column(bars_table, "volume");
    auto feature_timestamps = extract_int64_column(feature_table, "timestamp_ms");
    auto python_values = extract_double_column(feature_table, "buy_pressure_vol_vs_sma_3_50");

    std::vector<double> cpp_values(volumes.size(),
                                   std::numeric_limits<double>::quiet_NaN());
    for (size_t i = long_window - 1; i < volumes.size(); ++i) {
        std::span<const double> span{volumes.data(), i + 1};
        cpp_values[i] = TechnicalAnalysis::buy_pressure_vol_vs_sma(span, short_window, long_window);
    }

    auto res = compare_on_timestamp(bar_timestamps, cpp_values,
                                    feature_timestamps, python_values);
    REQUIRE(res.matched > 0);
    REQUIRE(res.mismatches == 0);
}

TEST_CASE("taker_flow_vol_rocp 150_15 matches Python",
          "[test_feature_gen][taker_flow_vol_rocp]") {
    const std::string data_root = get_data_dir_root();
    const std::string bars_path = data_root + BARS_SUBPATH;
    const std::string feature_path =
        data_root + FEATURES_SUBPATH + "taker_flow_vol_rocp_150_15/parquets";

    const int sma_window = 150;
    const int rocp_lag = 15;
    const int total_lookback = sma_window + rocp_lag;
    long long bars_start = CMP_START_MS - static_cast<long long>(total_lookback) * 100LL;
    auto bars_table = read_parquet_partitions(bars_path, bars_start, CMP_END_MS);
    REQUIRE(bars_table != nullptr);

    auto feature_table = read_parquet_partitions(feature_path, CMP_START_MS, CMP_END_MS);
    REQUIRE(feature_table != nullptr);

    auto bar_timestamps = extract_int64_column(bars_table, "kline_close_time_ms");
    auto volumes = extract_double_column(bars_table, "volume");
    auto feature_timestamps = extract_int64_column(feature_table, "timestamp_ms");
    auto python_values = extract_double_column(feature_table, "taker_flow_vol_rocp_150_15");

    std::vector<double> cpp_values(volumes.size(),
                                   std::numeric_limits<double>::quiet_NaN());
    for (size_t i = total_lookback - 1; i < volumes.size(); ++i) {
        std::span<const double> span{volumes.data(), i + 1};
        cpp_values[i] = TechnicalAnalysis::taker_flow_vol_rocp(span, sma_window, rocp_lag);
    }

    auto res = compare_on_timestamp(bar_timestamps, cpp_values,
                                    feature_timestamps, python_values);
    REQUIRE(res.matched > 0);
    REQUIRE(res.mismatches == 0);
}

TEST_CASE("trade_size_regime 10_50 matches Python",
          "[test_feature_gen][trade_size_regime]") {
    const std::string data_root = get_data_dir_root();
    const std::string bars_path = data_root + BARS_SUBPATH;
    const std::string feature_path =
        data_root + FEATURES_SUBPATH + "trade_size_regime_10_50/parquets";

    const int short_window = 10;
    const int long_window = 50;
    long long bars_start = CMP_START_MS - static_cast<long long>(long_window) * 100LL;
    auto bars_table = read_parquet_partitions(bars_path, bars_start, CMP_END_MS);
    REQUIRE(bars_table != nullptr);

    auto feature_table = read_parquet_partitions(feature_path, CMP_START_MS, CMP_END_MS);
    REQUIRE(feature_table != nullptr);

    auto bar_timestamps = extract_int64_column(bars_table, "kline_close_time_ms");
    auto volumes = extract_double_column(bars_table, "volume");
    auto num_trades_ll = extract_int64_column(bars_table, "number_of_trades");
    // Convert to long for span compatibility
    std::vector<long> num_trades(num_trades_ll.begin(), num_trades_ll.end());

    auto feature_timestamps = extract_int64_column(feature_table, "timestamp_ms");
    auto python_values = extract_double_column(feature_table, "trade_size_regime_10_50");

    std::vector<double> cpp_values(volumes.size(),
                                   std::numeric_limits<double>::quiet_NaN());
    for (size_t i = long_window - 1; i < volumes.size(); ++i) {
        std::span<const double> vol_span{volumes.data(), i + 1};
        std::span<const long> trades_span{num_trades.data(), i + 1};
        cpp_values[i] = TechnicalAnalysis::trade_size_regime(vol_span, trades_span, short_window, long_window);
    }

    auto res = compare_on_timestamp(bar_timestamps, cpp_values,
                                    feature_timestamps, python_values);
    REQUIRE(res.matched > 0);
    REQUIRE(res.mismatches == 0);
}

TEST_CASE("relative_avg_trade_size 15_3000 matches Python",
          "[test_feature_gen][relative_avg_trade_size]") {
    const std::string data_root = get_data_dir_root();
    const std::string bars_path = data_root + BARS_SUBPATH;
    const std::string feature_path =
        data_root + FEATURES_SUBPATH + "relative_avg_trade_size_15_3000/parquets";

    const int short_window = 15;
    const int long_window = 3000;
    long long bars_start = CMP_START_MS - static_cast<long long>(long_window) * 100LL;
    auto bars_table = read_parquet_partitions(bars_path, bars_start, CMP_END_MS);
    REQUIRE(bars_table != nullptr);

    auto feature_table = read_parquet_partitions(feature_path, CMP_START_MS, CMP_END_MS);
    REQUIRE(feature_table != nullptr);

    auto bar_timestamps = extract_int64_column(bars_table, "kline_close_time_ms");
    auto volumes = extract_double_column(bars_table, "volume");
    auto num_trades_ll = extract_int64_column(bars_table, "number_of_trades");
    std::vector<long> num_trades(num_trades_ll.begin(), num_trades_ll.end());

    auto feature_timestamps = extract_int64_column(feature_table, "timestamp_ms");
    auto python_values = extract_double_column(feature_table, "relative_avg_trade_size_15_3000");

    std::vector<double> cpp_values(volumes.size(),
                                   std::numeric_limits<double>::quiet_NaN());
    for (size_t i = long_window - 1; i < volumes.size(); ++i) {
        std::span<const double> vol_span{volumes.data(), i + 1};
        std::span<const long> trades_span{num_trades.data(), i + 1};
        cpp_values[i] = TechnicalAnalysis::relative_avg_trade_size(vol_span, trades_span, short_window, long_window);
    }

    auto res = compare_on_timestamp(bar_timestamps, cpp_values,
                                    feature_timestamps, python_values);
    REQUIRE(res.matched > 0);
    REQUIRE(res.mismatches == 0);
}

TEST_CASE("trade_count_zscore 15_3000 matches Python",
          "[test_feature_gen][trade_count_zscore]") {
    const std::string data_root = get_data_dir_root();
    const std::string bars_path = data_root + BARS_SUBPATH;
    const std::string feature_path =
        data_root + FEATURES_SUBPATH + "trade_count_zscore_15_3000/parquets";

    const int short_window = 15;
    const int long_window = 3000;
    // trade_count_zscore needs short_window + long_window - 1 elements
    const int total_lookback = short_window + long_window - 1;
    long long bars_start = CMP_START_MS - static_cast<long long>(total_lookback) * 100LL;
    auto bars_table = read_parquet_partitions(bars_path, bars_start, CMP_END_MS);
    REQUIRE(bars_table != nullptr);

    auto feature_table = read_parquet_partitions(feature_path, CMP_START_MS, CMP_END_MS);
    REQUIRE(feature_table != nullptr);

    auto bar_timestamps = extract_int64_column(bars_table, "kline_close_time_ms");
    auto num_trades_ll = extract_int64_column(bars_table, "number_of_trades");
    std::vector<long> num_trades(num_trades_ll.begin(), num_trades_ll.end());

    auto feature_timestamps = extract_int64_column(feature_table, "timestamp_ms");
    auto python_values = extract_double_column(feature_table, "trade_count_zscore_15_3000");

    std::vector<double> cpp_values(num_trades.size(),
                                   std::numeric_limits<double>::quiet_NaN());
    for (size_t i = total_lookback - 1; i < num_trades.size(); ++i) {
        std::span<const long> span{num_trades.data(), i + 1};
        cpp_values[i] = TechnicalAnalysis::trade_count_zscore(span, short_window, long_window);
    }

    auto res = compare_on_timestamp(bar_timestamps, cpp_values,
                                    feature_timestamps, python_values);
    REQUIRE(res.matched > 0);
    REQUIRE(res.mismatches == 0);
}

TEST_CASE("trade_count_intensity 15_600 matches Python",
          "[test_feature_gen][trade_count_intensity]") {
    const std::string data_root = get_data_dir_root();
    const std::string bars_path = data_root + BARS_SUBPATH;
    const std::string feature_path =
        data_root + FEATURES_SUBPATH + "trade_count_intensity_15_600/parquets";

    const int short_window = 15;
    const int long_window = 600;
    long long bars_start = CMP_START_MS - static_cast<long long>(long_window) * 100LL;
    auto bars_table = read_parquet_partitions(bars_path, bars_start, CMP_END_MS);
    REQUIRE(bars_table != nullptr);

    auto feature_table = read_parquet_partitions(feature_path, CMP_START_MS, CMP_END_MS);
    REQUIRE(feature_table != nullptr);

    auto bar_timestamps = extract_int64_column(bars_table, "kline_close_time_ms");
    auto num_trades_ll = extract_int64_column(bars_table, "number_of_trades");
    std::vector<long> num_trades(num_trades_ll.begin(), num_trades_ll.end());

    auto feature_timestamps = extract_int64_column(feature_table, "timestamp_ms");
    auto python_values = extract_double_column(feature_table, "trade_count_intensity_15_600");

    std::vector<double> cpp_values(num_trades.size(),
                                   std::numeric_limits<double>::quiet_NaN());
    for (size_t i = long_window - 1; i < num_trades.size(); ++i) {
        std::span<const long> span{num_trades.data(), i + 1};
        cpp_values[i] = TechnicalAnalysis::trade_count_intensity(span, short_window, long_window);
    }

    auto res = compare_on_timestamp(bar_timestamps, cpp_values,
                                    feature_timestamps, python_values);
    REQUIRE(res.matched > 0);
    REQUIRE(res.mismatches == 0);
}

TEST_CASE("buy_pressure_ratio 10 matches Python",
          "[test_feature_gen][buy_pressure_ratio]") {
    const std::string data_root = get_data_dir_root();
    const std::string bars_path = data_root + BARS_SUBPATH;
    const std::string feature_path =
        data_root + FEATURES_SUBPATH + "buy_pressure_ratio_10/parquets";

    const int lookback = 10;
    long long bars_start = CMP_START_MS - static_cast<long long>(lookback) * 100LL;
    auto bars_table = read_parquet_partitions(bars_path, bars_start, CMP_END_MS);
    REQUIRE(bars_table != nullptr);

    auto feature_table = read_parquet_partitions(feature_path, CMP_START_MS, CMP_END_MS);
    REQUIRE(feature_table != nullptr);

    auto bar_timestamps = extract_int64_column(bars_table, "kline_close_time_ms");
    auto taker_buy = extract_double_column(bars_table, "taker_buy_base_volume");
    auto volumes = extract_double_column(bars_table, "volume");
    auto feature_timestamps = extract_int64_column(feature_table, "timestamp_ms");
    auto python_values = extract_double_column(feature_table, "buy_pressure_ratio_10");

    std::vector<double> cpp_values(volumes.size(),
                                   std::numeric_limits<double>::quiet_NaN());
    for (size_t i = lookback - 1; i < volumes.size(); ++i) {
        std::span<const double> tb_span{taker_buy.data(), i + 1};
        std::span<const double> vol_span{volumes.data(), i + 1};
        cpp_values[i] = TechnicalAnalysis::buy_pressure_ratio(tb_span, vol_span, lookback);
    }

    auto res = compare_on_timestamp(bar_timestamps, cpp_values,
                                    feature_timestamps, python_values, 1e-8, 1e-11);
    REQUIRE(res.matched > 0);
    REQUIRE(res.mismatches == 0);
}

TEST_CASE("taker_flow_ofi 150 matches Python",
          "[test_feature_gen][taker_flow_ofi]") {
    const std::string data_root = get_data_dir_root();
    const std::string bars_path = data_root + BARS_SUBPATH;
    const std::string feature_path =
        data_root + FEATURES_SUBPATH + "taker_flow_ofi_150/parquets";

    const int lookback = 150;
    long long bars_start = CMP_START_MS - static_cast<long long>(lookback) * 100LL;
    auto bars_table = read_parquet_partitions(bars_path, bars_start, CMP_END_MS);
    REQUIRE(bars_table != nullptr);

    auto feature_table = read_parquet_partitions(feature_path, CMP_START_MS, CMP_END_MS);
    REQUIRE(feature_table != nullptr);

    auto bar_timestamps = extract_int64_column(bars_table, "kline_close_time_ms");
    auto taker_buy = extract_double_column(bars_table, "taker_buy_base_volume");
    auto volumes = extract_double_column(bars_table, "volume");
    auto feature_timestamps = extract_int64_column(feature_table, "timestamp_ms");
    auto python_values = extract_double_column(feature_table, "taker_flow_ofi_150");

    std::vector<double> cpp_values(volumes.size(),
                                   std::numeric_limits<double>::quiet_NaN());
    for (size_t i = lookback - 1; i < volumes.size(); ++i) {
        std::span<const double> tb_span{taker_buy.data(), i + 1};
        std::span<const double> vol_span{volumes.data(), i + 1};
        cpp_values[i] = TechnicalAnalysis::taker_flow_ofi(tb_span, vol_span, lookback);
    }

    auto res = compare_on_timestamp(bar_timestamps, cpp_values,
                                    feature_timestamps, python_values);
    REQUIRE(res.matched > 0);
    REQUIRE(res.mismatches == 0);
}

TEST_CASE("taker_flow_cvd_slope 600_150 matches Python",
          "[test_feature_gen][taker_flow_cvd_slope]") {
    const std::string data_root = get_data_dir_root();
    const std::string bars_path = data_root + BARS_SUBPATH;
    const std::string feature_path =
        data_root + FEATURES_SUBPATH + "taker_flow_cvd_slope_600_150/parquets";

    const int cvd_window = 600;
    const int slope_lag = 150;
    const int total_lookback = cvd_window + slope_lag;
    long long bars_start = CMP_START_MS - static_cast<long long>(total_lookback) * 100LL;
    auto bars_table = read_parquet_partitions(bars_path, bars_start, CMP_END_MS);
    REQUIRE(bars_table != nullptr);

    auto feature_table = read_parquet_partitions(feature_path, CMP_START_MS, CMP_END_MS);
    REQUIRE(feature_table != nullptr);

    auto bar_timestamps = extract_int64_column(bars_table, "kline_close_time_ms");
    auto taker_buy = extract_double_column(bars_table, "taker_buy_base_volume");
    auto volumes = extract_double_column(bars_table, "volume");
    auto feature_timestamps = extract_int64_column(feature_table, "timestamp_ms");
    auto python_values = extract_double_column(feature_table, "taker_flow_cvd_slope_600_150");

    std::vector<double> cpp_values(volumes.size(),
                                   std::numeric_limits<double>::quiet_NaN());
    for (size_t i = total_lookback - 1; i < volumes.size(); ++i) {
        std::span<const double> tb_span{taker_buy.data(), i + 1};
        std::span<const double> vol_span{volumes.data(), i + 1};
        cpp_values[i] = TechnicalAnalysis::taker_flow_cvd_slope(tb_span, vol_span, cvd_window, slope_lag);
    }

    auto res = compare_on_timestamp(bar_timestamps, cpp_values,
                                    feature_timestamps, python_values);
    REQUIRE(res.matched > 0);
    REQUIRE(res.mismatches == 0);
}

TEST_CASE("time_left_in_interval 5m matches Python",
          "[test_feature_gen][time_left_in_interval]") {
    const std::string data_root = get_data_dir_root();
    const std::string bars_path = data_root + BARS_SUBPATH;
    const std::string feature_path =
        data_root + FEATURES_SUBPATH + "time_left_in_interval_5m/parquets";

    auto bars_table = read_parquet_partitions(bars_path, CMP_START_MS, CMP_END_MS);
    REQUIRE(bars_table != nullptr);

    auto feature_table = read_parquet_partitions(feature_path, CMP_START_MS, CMP_END_MS);
    REQUIRE(feature_table != nullptr);

    auto bar_timestamps = extract_int64_column(bars_table, "kline_close_time_ms");
    auto feature_timestamps = extract_int64_column(feature_table, "timestamp_ms");
    auto python_values = extract_int64_column(feature_table, "time_left_in_interval_5m");

    const long long interval_ms = 5LL * 60 * 1000;
    std::vector<long long> cpp_values(bar_timestamps.size());
    for (size_t i = 0; i < bar_timestamps.size(); ++i) {
        cpp_values[i] = TechnicalAnalysis::time_left_in_interval(
            bar_timestamps[i], interval_ms);
    }

    auto res = compare_int64_on_timestamp(bar_timestamps, cpp_values,
                                          feature_timestamps, python_values);
    REQUIRE(res.matched > 0);
    REQUIRE(res.mismatches == 0);
}

TEST_CASE("distance_from_interval_extremes 5m matches Python",
          "[test_feature_gen][distance_from_interval_extremes]") {
    const std::string data_root = get_data_dir_root();
    const std::string bars_path = data_root + BARS_SUBPATH;
    const std::string feature_path =
        data_root + FEATURES_SUBPATH +
        "distance_from_interval_extremes_5m/parquets";

    // Need enough preceding data for the 5m interval bucket
    long long bars_start = CMP_START_MS - 300000LL;  // 5 minutes
    auto bars_table = read_parquet_partitions(bars_path, bars_start, CMP_END_MS);
    REQUIRE(bars_table != nullptr);

    auto feature_table = read_parquet_partitions(feature_path, CMP_START_MS, CMP_END_MS);
    REQUIRE(feature_table != nullptr);

    auto bar_timestamps = extract_int64_column(bars_table, "kline_close_time_ms");
    auto close_prices = extract_double_column(bars_table, "close_price");
    auto high_prices = extract_double_column(bars_table, "high_price");
    auto low_prices = extract_double_column(bars_table, "low_price");
    auto feature_timestamps = extract_int64_column(feature_table, "timestamp_ms");
    auto python_values = extract_double_column(feature_table, "distance_from_interval_extremes_5m");

    const long long interval_ms = 5LL * 60 * 1000;
    std::vector<double> cpp_values(bar_timestamps.size(),
                                   std::numeric_limits<double>::quiet_NaN());
    for (size_t i = 0; i < bar_timestamps.size(); ++i) {
        std::span<const long long> ts_span{bar_timestamps.data(), i + 1};
        std::span<const double> cp_span{close_prices.data(), i + 1};
        std::span<const double> hp_span{high_prices.data(), i + 1};
        std::span<const double> lp_span{low_prices.data(), i + 1};
        cpp_values[i] = TechnicalAnalysis::distance_from_interval_extremes(
            ts_span, cp_span, hp_span, lp_span, interval_ms);
    }

    auto res = compare_on_timestamp(bar_timestamps, cpp_values,
                                    feature_timestamps, python_values);
    REQUIRE(res.matched > 0);
    REQUIRE(res.mismatches == 0);
}

TEST_CASE("pct_time_above_open 5m matches Python",
          "[test_feature_gen][pct_time_above_open]") {
    const std::string data_root = get_data_dir_root();
    const std::string bars_path = data_root + BARS_SUBPATH;
    const std::string feature_path =
        data_root + FEATURES_SUBPATH + "pct_time_above_open_5m/parquets";

    long long bars_start = CMP_START_MS - 300000LL;
    auto bars_table = read_parquet_partitions(bars_path, bars_start, CMP_END_MS);
    REQUIRE(bars_table != nullptr);

    auto feature_table = read_parquet_partitions(feature_path, CMP_START_MS, CMP_END_MS);
    REQUIRE(feature_table != nullptr);

    auto bar_timestamps = extract_int64_column(bars_table, "kline_close_time_ms");
    auto close_prices = extract_double_column(bars_table, "close_price");
    auto feature_timestamps = extract_int64_column(feature_table, "timestamp_ms");
    auto python_values = extract_double_column(feature_table, "pct_time_above_open_5m");

    const long long interval_ms = 5LL * 60 * 1000;
    std::vector<double> cpp_values(bar_timestamps.size(),
                                   std::numeric_limits<double>::quiet_NaN());
    for (size_t i = 0; i < bar_timestamps.size(); ++i) {
        std::span<const long long> ts_span{bar_timestamps.data(), i + 1};
        std::span<const double> cp_span{close_prices.data(), i + 1};
        cpp_values[i] = TechnicalAnalysis::pct_time_above_open(
            ts_span, cp_span, interval_ms);
    }

    auto res = compare_on_timestamp(bar_timestamps, cpp_values,
                                    feature_timestamps, python_values);
    REQUIRE(res.matched > 0);
    REQUIRE(res.mismatches == 0);
}

TEST_CASE("interval_price_levels 5m matches Python",
          "[test_feature_gen][interval_price_levels]") {
    const std::string data_root = get_data_dir_root();
    const std::string bars_path = data_root + BARS_SUBPATH;

    long long bars_start = CMP_START_MS - 300000LL;
    auto bars_table = read_parquet_partitions(bars_path, bars_start, CMP_END_MS);
    REQUIRE(bars_table != nullptr);

    auto bar_timestamps = extract_int64_column(bars_table, "kline_close_time_ms");
    auto close_prices = extract_double_column(bars_table, "close_price");
    auto high_prices = extract_double_column(bars_table, "high_price");
    auto low_prices = extract_double_column(bars_table, "low_price");

    const long long interval_ms = 5LL * 60 * 1000;

    // Compute all three components
    std::vector<double> cpp_current(bar_timestamps.size(),
                                    std::numeric_limits<double>::quiet_NaN());
    std::vector<double> cpp_high(bar_timestamps.size(),
                                 std::numeric_limits<double>::quiet_NaN());
    std::vector<double> cpp_low(bar_timestamps.size(),
                                std::numeric_limits<double>::quiet_NaN());

    for (size_t i = 0; i < bar_timestamps.size(); ++i) {
        std::span<const long long> ts_span{bar_timestamps.data(), i + 1};
        std::span<const double> cp_span{close_prices.data(), i + 1};
        std::span<const double> hp_span{high_prices.data(), i + 1};
        std::span<const double> lp_span{low_prices.data(), i + 1};
        auto result = TechnicalAnalysis::interval_price_levels(
            ts_span, cp_span, hp_span, lp_span, interval_ms);
        cpp_current[i] = result.current_pct;
        cpp_high[i] = result.high_pct;
        cpp_low[i] = result.low_pct;
    }

    // Test current_pct
    {
        auto feature_table = read_parquet_partitions(
            data_root + FEATURES_SUBPATH +
                "interval_price_levels_5m_current_pct/parquets",
            CMP_START_MS, CMP_END_MS);
        REQUIRE(feature_table != nullptr);
        auto ft = extract_int64_column(feature_table, "timestamp_ms");
        auto pv = extract_double_column(feature_table, "interval_price_levels_5m_current_pct");
        std::cout << "[interval_price_levels current_pct]" << std::endl;
        auto res = compare_on_timestamp(bar_timestamps, cpp_current, ft, pv);
        REQUIRE(res.matched > 0);
        REQUIRE(res.mismatches == 0);
    }

    // NOTE: high_pct and low_pct are NOT compared against Python.
    // Python computes full-bucket aggregates (max high / min low for the
    // entire 5m bucket) and assigns them to ALL rows. C++ computes
    // streaming (running high/low up to the current bar). These are
    // intentionally different — C++ is correct for live inference where
    // each bar only sees past data. Only current_pct matches exactly
    // since both compute (close - interval_open) / interval_open per row.
}

TEST_CASE("calendar_features (ms_since_midnight, day_of_week) match Python",
          "[test_feature_gen][calendar_features]") {
    const std::string data_root = get_data_dir_root();
    const std::string bars_path = data_root + BARS_SUBPATH;
    const std::string feature_path =
        data_root + FEATURES_SUBPATH + "calendar_features/parquets";

    auto bars_table = read_parquet_partitions(bars_path, CMP_START_MS, CMP_END_MS);
    REQUIRE(bars_table != nullptr);

    auto feature_table = read_parquet_partitions(feature_path, CMP_START_MS, CMP_END_MS);
    REQUIRE(feature_table != nullptr);

    auto bar_timestamps = extract_int64_column(bars_table, "kline_close_time_ms");

    auto feature_timestamps = extract_int64_column(feature_table, "timestamp_ms");
    auto python_ms_midnight = extract_int64_column(feature_table, "ms_since_midnight");
    auto python_dow = extract_int8_column(feature_table, "day_of_week");

    // Python computes calendar features from kline_close_time_ms (= timestamp_ms)
    std::vector<long long> cpp_ms_midnight(bar_timestamps.size());
    std::vector<int> cpp_dow(bar_timestamps.size());
    for (size_t i = 0; i < bar_timestamps.size(); ++i) {
        cpp_ms_midnight[i] = TechnicalAnalysis::ms_since_midnight(bar_timestamps[i]);
        cpp_dow[i] = TechnicalAnalysis::day_of_week(bar_timestamps[i]);
    }

    std::cout << "[ms_since_midnight]" << std::endl;
    auto res1 = compare_int64_on_timestamp(bar_timestamps, cpp_ms_midnight,
                                           feature_timestamps, python_ms_midnight);
    REQUIRE(res1.matched > 0);
    REQUIRE(res1.mismatches == 0);

    std::cout << "[day_of_week]" << std::endl;
    auto res2 = compare_int8_on_timestamp(bar_timestamps, cpp_dow,
                                          feature_timestamps, python_dow);
    REQUIRE(res2.matched > 0);
    REQUIRE(res2.mismatches == 0);
}
