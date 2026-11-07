#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include "technical_analysis.h"
#include "test_helpers.h"

// Paths for wallclock_feats namespace
static const std::string BARS_SUBPATH =
    "/derived/bars/binance/spot/time/100_ms/BTCUSDT/parquets";
static const std::string WC_FEATURES_SUBPATH =
    "/derived/namespaces/wallclock_feats/features/bars/binance/spot/time/"
    "100_ms/BTCUSDT/";

// Comparison window: 2025-01-02
static const long long WC_CMP_START_MS =
    1735776000000LL;  // 2025-01-02 00:00 UTC
static const long long WC_CMP_END_MS =
    1735862400000LL;  // 2025-01-03 00:00 UTC

// =============================================================================
// realized_vol_close_price
// =============================================================================

TEST_CASE("realized_vol_close_price 1min matches Python",
          "[test_feature_gen_wallclock][realized_vol]") {
    const std::string data_root = get_data_dir_root();
    const std::string bars_path = data_root + BARS_SUBPATH;
    const std::string feature_path =
        data_root + WC_FEATURES_SUBPATH +
        "realized_vol_close_price_1min/parquets";

    const int lookback = 600;  // 1min = 600 bars
    long long bars_start =
        WC_CMP_START_MS - static_cast<long long>(lookback + 1) * 100LL;
    auto bars_table =
        read_parquet_partitions(bars_path, bars_start, WC_CMP_END_MS);
    REQUIRE(bars_table != nullptr);

    auto feature_table =
        read_parquet_partitions(feature_path, WC_CMP_START_MS, WC_CMP_END_MS);
    REQUIRE(feature_table != nullptr);

    auto bar_timestamps =
        extract_int64_column(bars_table, "kline_close_time_ms");
    auto close_prices = extract_double_column(bars_table, "close_price");
    auto feature_timestamps =
        extract_int64_column(feature_table, "timestamp_ms");
    auto python_values = extract_double_column(
        feature_table, "realized_vol_close_price_1min");

    std::vector<double> cpp_values(
        close_prices.size(), std::numeric_limits<double>::quiet_NaN());
    for (size_t i = 1; i < close_prices.size(); ++i) {
        std::span<const double> cp{close_prices.data(), i + 1};
        std::span<const long long> ts{bar_timestamps.data(), i + 1};
        cpp_values[i] =
            TechnicalAnalysis::realized_volatility(cp, ts, 60000LL);
    }

    auto res = compare_on_timestamp(bar_timestamps, cpp_values,
                                    feature_timestamps, python_values,
                                    WC_CMP_START_MS, WC_CMP_END_MS, 1e-7,
                                    1e-10);
    std::cout << "[realized_vol 1min] ";
    REQUIRE(res.matched > 0);
    REQUIRE(res.mismatches == 0);
}

TEST_CASE("realized_vol_close_price 1h matches Python",
          "[test_feature_gen_wallclock][realized_vol]") {
    const std::string data_root = get_data_dir_root();
    const std::string bars_path = data_root + BARS_SUBPATH;
    const std::string feature_path =
        data_root + WC_FEATURES_SUBPATH +
        "realized_vol_close_price_1h/parquets";

    long long bars_start = WC_CMP_START_MS - 3600000LL - 1000LL;
    auto bars_table =
        read_parquet_partitions(bars_path, bars_start, WC_CMP_END_MS);
    REQUIRE(bars_table != nullptr);

    auto feature_table =
        read_parquet_partitions(feature_path, WC_CMP_START_MS, WC_CMP_END_MS);
    REQUIRE(feature_table != nullptr);

    auto bar_timestamps =
        extract_int64_column(bars_table, "kline_close_time_ms");
    auto close_prices = extract_double_column(bars_table, "close_price");
    auto feature_timestamps =
        extract_int64_column(feature_table, "timestamp_ms");
    auto python_values =
        extract_double_column(feature_table, "realized_vol_close_price_1h");

    std::vector<double> cpp_values(
        close_prices.size(), std::numeric_limits<double>::quiet_NaN());
    for (size_t i = 1; i < close_prices.size(); ++i) {
        std::span<const double> cp{close_prices.data(), i + 1};
        std::span<const long long> ts{bar_timestamps.data(), i + 1};
        cpp_values[i] =
            TechnicalAnalysis::realized_volatility(cp, ts, 3600000LL);
    }

    auto res = compare_on_timestamp(bar_timestamps, cpp_values,
                                    feature_timestamps, python_values,
                                    WC_CMP_START_MS, WC_CMP_END_MS, 1e-7,
                                    1e-10);
    std::cout << "[realized_vol 1h] ";
    REQUIRE(res.matched > 0);
    REQUIRE(res.mismatches == 0);
}

// =============================================================================
// rocp_close_price (wallclock naming)
// =============================================================================

TEST_CASE("rocp_close_price wallclock 1s matches Python",
          "[test_feature_gen_wallclock][rocp_close_price]") {
    const std::string data_root = get_data_dir_root();
    const std::string bars_path = data_root + BARS_SUBPATH;
    const std::string feature_path =
        data_root + WC_FEATURES_SUBPATH + "rocp_close_price_1s/parquets";

    long long bars_start = WC_CMP_START_MS - 2000LL;
    auto bars_table =
        read_parquet_partitions(bars_path, bars_start, WC_CMP_END_MS);
    REQUIRE(bars_table != nullptr);

    auto feature_table =
        read_parquet_partitions(feature_path, WC_CMP_START_MS, WC_CMP_END_MS);
    REQUIRE(feature_table != nullptr);

    auto bar_timestamps =
        extract_int64_column(bars_table, "kline_close_time_ms");
    auto open_timestamps =
        extract_int64_column(bars_table, "kline_open_time_ms");
    auto close_prices = extract_double_column(bars_table, "close_price");
    auto feature_timestamps =
        extract_int64_column(feature_table, "timestamp_ms");
    auto python_rocp =
        extract_double_column(feature_table, "rocp_close_price_1s");

    std::vector<double> cpp_rocp(
        close_prices.size(), std::numeric_limits<double>::quiet_NaN());
    for (size_t i = 1; i < close_prices.size(); ++i) {
        std::span<const double> cp{close_prices.data(), i + 1};
        std::span<const long long> ot{open_timestamps.data(), i + 1};
        std::span<const long long> ct{bar_timestamps.data(), i + 1};
        cpp_rocp[i] =
            TechnicalAnalysis::rocp_time_based(cp, ot, ct, 1000LL);
    }

    auto res = compare_on_timestamp(bar_timestamps, cpp_rocp,
                                    feature_timestamps, python_rocp,
                                    WC_CMP_START_MS, WC_CMP_END_MS);
    std::cout << "[rocp 1s] ";
    REQUIRE(res.matched > 0);
    REQUIRE(res.mismatches == 0);
}

TEST_CASE("rocp_close_price wallclock 1min matches Python",
          "[test_feature_gen_wallclock][rocp_close_price]") {
    const std::string data_root = get_data_dir_root();
    const std::string bars_path = data_root + BARS_SUBPATH;
    const std::string feature_path =
        data_root + WC_FEATURES_SUBPATH + "rocp_close_price_1min/parquets";

    long long bars_start = WC_CMP_START_MS - 120000LL;
    auto bars_table =
        read_parquet_partitions(bars_path, bars_start, WC_CMP_END_MS);
    REQUIRE(bars_table != nullptr);

    auto feature_table =
        read_parquet_partitions(feature_path, WC_CMP_START_MS, WC_CMP_END_MS);
    REQUIRE(feature_table != nullptr);

    auto bar_timestamps =
        extract_int64_column(bars_table, "kline_close_time_ms");
    auto open_timestamps =
        extract_int64_column(bars_table, "kline_open_time_ms");
    auto close_prices = extract_double_column(bars_table, "close_price");
    auto feature_timestamps =
        extract_int64_column(feature_table, "timestamp_ms");
    auto python_rocp =
        extract_double_column(feature_table, "rocp_close_price_1min");

    std::vector<double> cpp_rocp(
        close_prices.size(), std::numeric_limits<double>::quiet_NaN());
    for (size_t i = 1; i < close_prices.size(); ++i) {
        std::span<const double> cp{close_prices.data(), i + 1};
        std::span<const long long> ot{open_timestamps.data(), i + 1};
        std::span<const long long> ct{bar_timestamps.data(), i + 1};
        cpp_rocp[i] =
            TechnicalAnalysis::rocp_time_based(cp, ot, ct, 60000LL);
    }

    auto res = compare_on_timestamp(bar_timestamps, cpp_rocp,
                                    feature_timestamps, python_rocp,
                                    WC_CMP_START_MS, WC_CMP_END_MS);
    std::cout << "[rocp 1min] ";
    REQUIRE(res.matched > 0);
    REQUIRE(res.mismatches == 0);
}

TEST_CASE("rocp_close_price wallclock 1h matches Python",
          "[test_feature_gen_wallclock][rocp_close_price]") {
    const std::string data_root = get_data_dir_root();
    const std::string bars_path = data_root + BARS_SUBPATH;
    const std::string feature_path =
        data_root + WC_FEATURES_SUBPATH + "rocp_close_price_1h/parquets";

    long long bars_start = WC_CMP_START_MS - 3600000LL - 1000LL;
    auto bars_table =
        read_parquet_partitions(bars_path, bars_start, WC_CMP_END_MS);
    REQUIRE(bars_table != nullptr);

    auto feature_table =
        read_parquet_partitions(feature_path, WC_CMP_START_MS, WC_CMP_END_MS);
    REQUIRE(feature_table != nullptr);

    auto bar_timestamps =
        extract_int64_column(bars_table, "kline_close_time_ms");
    auto open_timestamps =
        extract_int64_column(bars_table, "kline_open_time_ms");
    auto close_prices = extract_double_column(bars_table, "close_price");
    auto feature_timestamps =
        extract_int64_column(feature_table, "timestamp_ms");
    auto python_rocp =
        extract_double_column(feature_table, "rocp_close_price_1h");

    std::vector<double> cpp_rocp(
        close_prices.size(), std::numeric_limits<double>::quiet_NaN());
    for (size_t i = 1; i < close_prices.size(); ++i) {
        std::span<const double> cp{close_prices.data(), i + 1};
        std::span<const long long> ot{open_timestamps.data(), i + 1};
        std::span<const long long> ct{bar_timestamps.data(), i + 1};
        cpp_rocp[i] =
            TechnicalAnalysis::rocp_time_based(cp, ot, ct, 3600000LL);
    }

    auto res = compare_on_timestamp(bar_timestamps, cpp_rocp,
                                    feature_timestamps, python_rocp,
                                    WC_CMP_START_MS, WC_CMP_END_MS);
    std::cout << "[rocp 1h] ";
    REQUIRE(res.matched > 0);
    REQUIRE(res.mismatches == 0);
}

// =============================================================================
// interval_crossings_5min
// =============================================================================

TEST_CASE("interval_crossings_5min matches Python",
          "[test_feature_gen_wallclock][interval_crossings]") {
    const std::string data_root = get_data_dir_root();
    const std::string bars_path = data_root + BARS_SUBPATH;

    const long long interval_ms = 5LL * 60 * 1000;
    // Need 5 min + 1 min rolling window of preceding data
    long long bars_start = WC_CMP_START_MS - 360000LL;
    auto bars_table =
        read_parquet_partitions(bars_path, bars_start, WC_CMP_END_MS);
    REQUIRE(bars_table != nullptr);

    auto bar_timestamps =
        extract_int64_column(bars_table, "kline_close_time_ms");
    auto close_prices = extract_double_column(bars_table, "close_price");

    // Compute C++ values
    std::vector<long long> cpp_time_since(bar_timestamps.size(), 0);
    std::vector<int> cpp_num_total(bar_timestamps.size(), 0);
    std::vector<int> cpp_num_last_1min(bar_timestamps.size(), 0);

    for (size_t i = 0; i < bar_timestamps.size(); ++i) {
        std::span<const long long> ts_span{bar_timestamps.data(), i + 1};
        std::span<const double> cp_span{close_prices.data(), i + 1};
        auto result = TechnicalAnalysis::interval_crossings(
            ts_span, cp_span, interval_ms, 60000LL);
        cpp_time_since[i] = result.time_since;
        cpp_num_total[i] = result.num_total;
        cpp_num_last_1min[i] = result.num_last_1min;
    }

    // Test time_since (Int64)
    {
        auto feature_table = read_parquet_partitions(
            data_root + WC_FEATURES_SUBPATH +
                "interval_crossings_5min_time_since/parquets",
            WC_CMP_START_MS, WC_CMP_END_MS);
        REQUIRE(feature_table != nullptr);
        auto ft = extract_int64_column(feature_table, "timestamp_ms");
        auto pv = extract_int64_column(
            feature_table, "interval_crossings_5min_time_since");
        std::cout << "[interval_crossings time_since] ";
        auto res = compare_int64_on_timestamp(bar_timestamps, cpp_time_since,
                                              ft, pv, WC_CMP_START_MS,
                                              WC_CMP_END_MS);
        REQUIRE(res.matched > 0);
        REQUIRE(res.mismatches == 0);
    }

    // Test num_total (Int32)
    {
        auto feature_table = read_parquet_partitions(
            data_root + WC_FEATURES_SUBPATH +
                "interval_crossings_5min_num_total/parquets",
            WC_CMP_START_MS, WC_CMP_END_MS);
        REQUIRE(feature_table != nullptr);
        auto ft = extract_int64_column(feature_table, "timestamp_ms");
        auto pv = extract_int32_column(
            feature_table, "interval_crossings_5min_num_total");
        std::cout << "[interval_crossings num_total] ";
        auto res = compare_int32_on_timestamp(bar_timestamps, cpp_num_total,
                                              ft, pv, WC_CMP_START_MS,
                                              WC_CMP_END_MS);
        REQUIRE(res.matched > 0);
        REQUIRE(res.mismatches == 0);
    }

    // Test num_last_1min (Int32)
    {
        auto feature_table = read_parquet_partitions(
            data_root + WC_FEATURES_SUBPATH +
                "interval_crossings_5min_num_last_1min/parquets",
            WC_CMP_START_MS, WC_CMP_END_MS);
        REQUIRE(feature_table != nullptr);
        auto ft = extract_int64_column(feature_table, "timestamp_ms");
        auto pv = extract_int32_column(
            feature_table, "interval_crossings_5min_num_last_1min");
        std::cout << "[interval_crossings num_last_1min] ";
        auto res = compare_int32_on_timestamp(bar_timestamps, cpp_num_last_1min,
                                              ft, pv, WC_CMP_START_MS,
                                              WC_CMP_END_MS);
        REQUIRE(res.matched > 0);
        REQUIRE(res.mismatches == 0);
    }
}

// =============================================================================
// interval_moneyness_5m
// =============================================================================

TEST_CASE("interval_moneyness_5m matches Python",
          "[test_feature_gen_wallclock][interval_moneyness]") {
    const std::string data_root = get_data_dir_root();
    const std::string bars_path = data_root + BARS_SUBPATH;

    const long long interval_ms = 5LL * 60 * 1000;
    const int vol_lookback = 3000;  // 5min realized vol
    long long bars_start =
        WC_CMP_START_MS - static_cast<long long>(vol_lookback + 1) * 100LL;
    auto bars_table =
        read_parquet_partitions(bars_path, bars_start, WC_CMP_END_MS);
    REQUIRE(bars_table != nullptr);

    auto bar_timestamps =
        extract_int64_column(bars_table, "kline_close_time_ms");
    auto close_prices = extract_double_column(bars_table, "close_price");

    std::vector<double> cpp_div_vol(
        bar_timestamps.size(), std::numeric_limits<double>::quiet_NaN());
    std::vector<double> cpp_div_sqrt_t(
        bar_timestamps.size(), std::numeric_limits<double>::quiet_NaN());
    std::vector<double> cpp_div_vol_sqrt_t(
        bar_timestamps.size(), std::numeric_limits<double>::quiet_NaN());

    for (size_t i = vol_lookback; i < bar_timestamps.size(); ++i) {
        std::span<const long long> ts_span{bar_timestamps.data(), i + 1};
        std::span<const double> cp_span{close_prices.data(), i + 1};
        double vol =
            TechnicalAnalysis::realized_volatility(cp_span, ts_span, 300000LL);
        auto result = TechnicalAnalysis::interval_moneyness(
            ts_span, cp_span, interval_ms, vol);
        cpp_div_vol[i] = result.dist_div_vol;
        cpp_div_sqrt_t[i] = result.dist_div_sqrt_t;
        cpp_div_vol_sqrt_t[i] = result.dist_div_vol_sqrt_t;
    }

    // Test dist_div_vol
    {
        auto feature_table = read_parquet_partitions(
            data_root + WC_FEATURES_SUBPATH +
                "interval_moneyness_5m_dist_div_vol/parquets",
            WC_CMP_START_MS, WC_CMP_END_MS);
        REQUIRE(feature_table != nullptr);
        auto ft = extract_int64_column(feature_table, "timestamp_ms");
        auto pv = extract_double_column(
            feature_table, "interval_moneyness_5m_dist_div_vol");
        std::cout << "[interval_moneyness dist_div_vol] ";
        auto res = compare_on_timestamp(bar_timestamps, cpp_div_vol, ft, pv,
                                        WC_CMP_START_MS, WC_CMP_END_MS, 1e-6,
                                        1e-10);
        REQUIRE(res.matched > 0);
        REQUIRE(res.mismatches == 0);
    }

    // Test dist_div_sqrt_t
    {
        auto feature_table = read_parquet_partitions(
            data_root + WC_FEATURES_SUBPATH +
                "interval_moneyness_5m_dist_div_sqrt_t/parquets",
            WC_CMP_START_MS, WC_CMP_END_MS);
        REQUIRE(feature_table != nullptr);
        auto ft = extract_int64_column(feature_table, "timestamp_ms");
        auto pv = extract_double_column(
            feature_table, "interval_moneyness_5m_dist_div_sqrt_t");
        std::cout << "[interval_moneyness dist_div_sqrt_t] ";
        auto res = compare_on_timestamp(bar_timestamps, cpp_div_sqrt_t, ft, pv,
                                        WC_CMP_START_MS, WC_CMP_END_MS, 1e-8,
                                        1e-12);
        REQUIRE(res.matched > 0);
        REQUIRE(res.mismatches == 0);
    }

    // Test dist_div_vol_sqrt_t
    {
        auto feature_table = read_parquet_partitions(
            data_root + WC_FEATURES_SUBPATH +
                "interval_moneyness_5m_dist_div_vol_sqrt_t/parquets",
            WC_CMP_START_MS, WC_CMP_END_MS);
        REQUIRE(feature_table != nullptr);
        auto ft = extract_int64_column(feature_table, "timestamp_ms");
        auto pv = extract_double_column(
            feature_table, "interval_moneyness_5m_dist_div_vol_sqrt_t");
        std::cout << "[interval_moneyness dist_div_vol_sqrt_t] ";
        auto res =
            compare_on_timestamp(bar_timestamps, cpp_div_vol_sqrt_t, ft, pv,
                                 WC_CMP_START_MS, WC_CMP_END_MS, 1e-6, 1e-10);
        REQUIRE(res.matched > 0);
        REQUIRE(res.mismatches == 0);
    }
}

// =============================================================================
// rocp_realized_vol_close_price_1h
// =============================================================================

TEST_CASE("rocp_1min_realized_vol_close_price_1h matches Python",
          "[test_feature_gen_wallclock][rocp_realized_vol]") {
    const std::string data_root = get_data_dir_root();
    const std::string bars_path = data_root + BARS_SUBPATH;
    const std::string feature_path =
        data_root + WC_FEATURES_SUBPATH +
        "rocp_1min_realized_vol_close_price_1h/parquets";

    const long long vol_window_ms = 3600000LL;   // 1h
    const long long rocp_lookback_ms = 60000LL;  // 1min
    // Need vol_window + rocp_lookback of history before comparison window
    long long bars_start =
        WC_CMP_START_MS - vol_window_ms - rocp_lookback_ms - 10000LL;
    auto bars_table =
        read_parquet_partitions(bars_path, bars_start, WC_CMP_END_MS);
    REQUIRE(bars_table != nullptr);

    auto feature_table =
        read_parquet_partitions(feature_path, WC_CMP_START_MS, WC_CMP_END_MS);
    REQUIRE(feature_table != nullptr);

    auto bar_timestamps =
        extract_int64_column(bars_table, "kline_close_time_ms");
    auto close_prices = extract_double_column(bars_table, "close_price");
    auto feature_timestamps =
        extract_int64_column(feature_table, "timestamp_ms");
    auto python_values = extract_double_column(
        feature_table, "rocp_1min_realized_vol_close_price_1h");

    // Pre-compute realized_vol_1h for all bars
    std::vector<double> vol_1h(bar_timestamps.size(),
                               std::numeric_limits<double>::quiet_NaN());
    for (size_t i = 0; i < bar_timestamps.size(); ++i) {
        std::span<const double> cp_span{close_prices.data(), i + 1};
        std::span<const long long> ts_span{bar_timestamps.data(), i + 1};
        vol_1h[i] = TechnicalAnalysis::realized_volatility(
            cp_span, ts_span, vol_window_ms);
    }

    // Compute ROCP with time-based lookback (join_asof forward strategy)
    std::vector<double> cpp_values(
        close_prices.size(), std::numeric_limits<double>::quiet_NaN());
    for (size_t i = 0; i < bar_timestamps.size(); ++i) {
        long long target_ts = bar_timestamps[i] - rocp_lookback_ms;
        // Find first bar with timestamp >= target_ts (forward strategy)
        auto it = std::lower_bound(bar_timestamps.begin(),
                                   bar_timestamps.begin() + i, target_ts);
        if (it == bar_timestamps.begin() + i) continue;
        size_t past_idx = static_cast<size_t>(it - bar_timestamps.begin());
        // Check tolerance: past bar must be within lookback_ms of target
        if (bar_timestamps[past_idx] - target_ts > rocp_lookback_ms) continue;

        double vol_past = vol_1h[past_idx];
        double vol_now = vol_1h[i];
        if (vol_past != 0.0 && !std::isnan(vol_past) && !std::isnan(vol_now)) {
            cpp_values[i] = (vol_now - vol_past) / vol_past;
        }
    }

    auto res = compare_on_timestamp(bar_timestamps, cpp_values,
                                    feature_timestamps, python_values,
                                    WC_CMP_START_MS, WC_CMP_END_MS, 1e-6,
                                    1e-10);
    std::cout << "[rocp_1min_realized_vol_1h] ";
    REQUIRE(res.matched > 0);
    REQUIRE(res.mismatches == 0);
}

// =============================================================================
// taker_flow_ofi (wallclock naming)
// =============================================================================

TEST_CASE("taker_flow_ofi wallclock 5s matches Python",
          "[test_feature_gen_wallclock][taker_flow_ofi]") {
    const std::string data_root = get_data_dir_root();
    const std::string bars_path = data_root + BARS_SUBPATH;
    const std::string feature_path =
        data_root + WC_FEATURES_SUBPATH + "taker_flow_ofi_5s/parquets";

    const int lookback = 50;  // 5s = 50 bars
    long long bars_start =
        WC_CMP_START_MS - static_cast<long long>(lookback) * 100LL;
    auto bars_table =
        read_parquet_partitions(bars_path, bars_start, WC_CMP_END_MS);
    REQUIRE(bars_table != nullptr);

    auto feature_table =
        read_parquet_partitions(feature_path, WC_CMP_START_MS, WC_CMP_END_MS);
    REQUIRE(feature_table != nullptr);

    auto bar_timestamps =
        extract_int64_column(bars_table, "kline_close_time_ms");
    auto taker_buy =
        extract_double_column(bars_table, "taker_buy_base_volume");
    auto volumes = extract_double_column(bars_table, "volume");
    auto feature_timestamps =
        extract_int64_column(feature_table, "timestamp_ms");
    auto python_values =
        extract_double_column(feature_table, "taker_flow_ofi_5s");

    std::vector<double> cpp_values(
        volumes.size(), std::numeric_limits<double>::quiet_NaN());
    for (size_t i = lookback - 1; i < volumes.size(); ++i) {
        std::span<const double> tb_span{taker_buy.data(), i + 1};
        std::span<const double> vol_span{volumes.data(), i + 1};
        cpp_values[i] =
            TechnicalAnalysis::taker_flow_ofi(tb_span, vol_span, lookback);
    }

    auto res = compare_on_timestamp(bar_timestamps, cpp_values,
                                    feature_timestamps, python_values,
                                    WC_CMP_START_MS, WC_CMP_END_MS);
    std::cout << "[taker_flow_ofi 5s] ";
    REQUIRE(res.matched > 0);
    REQUIRE(res.mismatches == 0);
}

TEST_CASE("taker_flow_ofi wallclock 1min matches Python",
          "[test_feature_gen_wallclock][taker_flow_ofi]") {
    const std::string data_root = get_data_dir_root();
    const std::string bars_path = data_root + BARS_SUBPATH;
    const std::string feature_path =
        data_root + WC_FEATURES_SUBPATH + "taker_flow_ofi_1min/parquets";

    const int lookback = 600;  // 1min = 600 bars
    long long bars_start =
        WC_CMP_START_MS - static_cast<long long>(lookback) * 100LL;
    auto bars_table =
        read_parquet_partitions(bars_path, bars_start, WC_CMP_END_MS);
    REQUIRE(bars_table != nullptr);

    auto feature_table =
        read_parquet_partitions(feature_path, WC_CMP_START_MS, WC_CMP_END_MS);
    REQUIRE(feature_table != nullptr);

    auto bar_timestamps =
        extract_int64_column(bars_table, "kline_close_time_ms");
    auto taker_buy =
        extract_double_column(bars_table, "taker_buy_base_volume");
    auto volumes = extract_double_column(bars_table, "volume");
    auto feature_timestamps =
        extract_int64_column(feature_table, "timestamp_ms");
    auto python_values =
        extract_double_column(feature_table, "taker_flow_ofi_1min");

    std::vector<double> cpp_values(
        volumes.size(), std::numeric_limits<double>::quiet_NaN());
    for (size_t i = lookback - 1; i < volumes.size(); ++i) {
        std::span<const double> tb_span{taker_buy.data(), i + 1};
        std::span<const double> vol_span{volumes.data(), i + 1};
        cpp_values[i] =
            TechnicalAnalysis::taker_flow_ofi(tb_span, vol_span, lookback);
    }

    auto res = compare_on_timestamp(bar_timestamps, cpp_values,
                                    feature_timestamps, python_values,
                                    WC_CMP_START_MS, WC_CMP_END_MS);
    std::cout << "[taker_flow_ofi 1min] ";
    REQUIRE(res.matched > 0);
    REQUIRE(res.mismatches == 0);
}

// =============================================================================
// taker_flow_bpr_2s
// =============================================================================

TEST_CASE("taker_flow_bpr_2s matches Python",
          "[test_feature_gen_wallclock][taker_flow_bpr]") {
    const std::string data_root = get_data_dir_root();
    const std::string bars_path = data_root + BARS_SUBPATH;
    const std::string feature_path =
        data_root + WC_FEATURES_SUBPATH + "taker_flow_bpr_2s/parquets";

    const int lookback = 20;  // 2s = 20 bars
    long long bars_start =
        WC_CMP_START_MS - static_cast<long long>(lookback) * 100LL;
    auto bars_table =
        read_parquet_partitions(bars_path, bars_start, WC_CMP_END_MS);
    REQUIRE(bars_table != nullptr);

    auto feature_table =
        read_parquet_partitions(feature_path, WC_CMP_START_MS, WC_CMP_END_MS);
    REQUIRE(feature_table != nullptr);

    auto bar_timestamps =
        extract_int64_column(bars_table, "kline_close_time_ms");
    auto taker_buy =
        extract_double_column(bars_table, "taker_buy_base_volume");
    auto volumes = extract_double_column(bars_table, "volume");
    auto feature_timestamps =
        extract_int64_column(feature_table, "timestamp_ms");
    auto python_values =
        extract_double_column(feature_table, "taker_flow_bpr_2s");

    std::vector<double> cpp_values(
        volumes.size(), std::numeric_limits<double>::quiet_NaN());
    for (size_t i = lookback - 1; i < volumes.size(); ++i) {
        std::span<const double> tb_span{taker_buy.data(), i + 1};
        std::span<const double> vol_span{volumes.data(), i + 1};
        cpp_values[i] =
            TechnicalAnalysis::buy_pressure_ratio(tb_span, vol_span, lookback);
    }

    auto res = compare_on_timestamp(bar_timestamps, cpp_values,
                                    feature_timestamps, python_values,
                                    WC_CMP_START_MS, WC_CMP_END_MS, 1e-8,
                                    1e-11);
    std::cout << "[taker_flow_bpr 2s] ";
    REQUIRE(res.matched > 0);
    REQUIRE(res.mismatches == 0);
}

// =============================================================================
// taker_flow_vol_vs_sma_1s_vs_1min
// =============================================================================

TEST_CASE("taker_flow_vol_vs_sma_1s_vs_1min matches Python",
          "[test_feature_gen_wallclock][vol_vs_sma]") {
    const std::string data_root = get_data_dir_root();
    const std::string bars_path = data_root + BARS_SUBPATH;
    const std::string feature_path =
        data_root + WC_FEATURES_SUBPATH +
        "taker_flow_vol_vs_sma_1s_vs_1min/parquets";

    const int short_window = 10;   // 1s = 10 bars
    const int long_window = 600;   // 1min = 600 bars
    long long bars_start =
        WC_CMP_START_MS - static_cast<long long>(long_window) * 100LL;
    auto bars_table =
        read_parquet_partitions(bars_path, bars_start, WC_CMP_END_MS);
    REQUIRE(bars_table != nullptr);

    auto feature_table =
        read_parquet_partitions(feature_path, WC_CMP_START_MS, WC_CMP_END_MS);
    REQUIRE(feature_table != nullptr);

    auto bar_timestamps =
        extract_int64_column(bars_table, "kline_close_time_ms");
    auto volumes = extract_double_column(bars_table, "volume");
    auto feature_timestamps =
        extract_int64_column(feature_table, "timestamp_ms");
    auto python_values = extract_double_column(
        feature_table, "taker_flow_vol_vs_sma_1s_vs_1min");

    std::vector<double> cpp_values(
        volumes.size(), std::numeric_limits<double>::quiet_NaN());
    for (size_t i = long_window - 1; i < volumes.size(); ++i) {
        std::span<const double> span{volumes.data(), i + 1};
        cpp_values[i] = TechnicalAnalysis::buy_pressure_vol_vs_sma(
            span, short_window, long_window);
    }

    auto res = compare_on_timestamp(bar_timestamps, cpp_values,
                                    feature_timestamps, python_values,
                                    WC_CMP_START_MS, WC_CMP_END_MS);
    std::cout << "[vol_vs_sma 1s_vs_1min] ";
    REQUIRE(res.matched > 0);
    REQUIRE(res.mismatches == 0);
}

// =============================================================================
// interval_price_levels_5min
// =============================================================================

TEST_CASE("interval_price_levels_5min wallclock matches Python",
          "[test_feature_gen_wallclock][interval_price_levels]") {
    const std::string data_root = get_data_dir_root();
    const std::string bars_path = data_root + BARS_SUBPATH;

    const long long interval_ms = 5LL * 60 * 1000;
    long long bars_start = WC_CMP_START_MS - 300000LL;  // 5 min
    auto bars_table =
        read_parquet_partitions(bars_path, bars_start, WC_CMP_END_MS);
    REQUIRE(bars_table != nullptr);

    auto bar_timestamps =
        extract_int64_column(bars_table, "kline_close_time_ms");
    auto close_prices = extract_double_column(bars_table, "close_price");
    auto high_prices = extract_double_column(bars_table, "high_price");
    auto low_prices = extract_double_column(bars_table, "low_price");

    std::vector<double> cpp_current(
        bar_timestamps.size(), std::numeric_limits<double>::quiet_NaN());
    std::vector<double> cpp_high(
        bar_timestamps.size(), std::numeric_limits<double>::quiet_NaN());
    std::vector<double> cpp_low(
        bar_timestamps.size(), std::numeric_limits<double>::quiet_NaN());

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
            data_root + WC_FEATURES_SUBPATH +
                "interval_price_levels_5min_current_pct/parquets",
            WC_CMP_START_MS, WC_CMP_END_MS);
        REQUIRE(feature_table != nullptr);
        auto ft = extract_int64_column(feature_table, "timestamp_ms");
        auto pv = extract_double_column(
            feature_table, "interval_price_levels_5min_current_pct");
        std::cout << "[interval_price_levels current_pct] ";
        auto res = compare_on_timestamp(bar_timestamps, cpp_current, ft, pv,
                                        WC_CMP_START_MS, WC_CMP_END_MS);
        REQUIRE(res.matched > 0);
        REQUIRE(res.mismatches == 0);
    }
}
