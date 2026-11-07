#pragma once

#include <cmath>
#include <ctime>
#include <iostream>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include "datetime_utils.h"

namespace TechnicalAnalysis {

template <typename T>
std::unordered_map<int, std::vector<double>> rocp(
    const std::vector<T>& nums, const std::vector<int>& periods) {
    if (nums.empty()) {
        throw std::invalid_argument("Input vector is empty.");
    }

    std::unordered_map<int, std::vector<double>> rocp_results;

    for (int period : periods) {
        if (period <= 0) {
            throw std::invalid_argument(
                "ROCP period must be greater than zero.");
        }

        std::vector<double> rocp_values(nums.size(), 0.0);

        for (size_t i = period; i < nums.size(); ++i) {
            double prev_value = static_cast<double>(nums[i - period]);
            double curr_value = static_cast<double>(nums[i]);

            if (prev_value == 0.0 || std::isnan(prev_value) ||
                std::isinf(prev_value)) {
                rocp_values[i] = 0.0;
            } else {
                rocp_values[i] = (curr_value - prev_value) / prev_value;
            }
        }

        rocp_results[period] = rocp_values;
    }

    return rocp_results;
}

template <typename T>
std::vector<double> rocp_values(const std::span<T>& nums,
                                int max_lookback_window, int period) {
    if (nums.empty()) {
        throw std::invalid_argument("Input vector is empty.");
    }
    if (period <= 0) {
        throw std::invalid_argument("ROCP period must be greater than zero.");
    }
    if (max_lookback_window < period) {
        throw std::invalid_argument("Max lookback window must be >= period.");
    }

    size_t start_idx = nums.size() > max_lookback_window
                           ? nums.size() - max_lookback_window
                           : 0;

    std::vector<double> rocp_values;
    rocp_values.reserve(max_lookback_window);

    for (size_t i = start_idx + period; i < nums.size(); ++i) {
        double prev_value = static_cast<double>(nums[i - period]);
        double curr_value = static_cast<double>(nums[i]);

        if (prev_value == 0.0 || std::isnan(prev_value) ||
            std::isinf(prev_value)) {
            rocp_values.push_back(0.0);
        } else {
            rocp_values.push_back((curr_value - prev_value) / prev_value);
        }
    }

    return rocp_values;
}

template <typename T>
double rocp(const std::vector<T>& nums, int period) {
    if (nums.empty()) {
        throw std::invalid_argument("Nums is empty");
    }
    if (period <= 0) {
        throw std::invalid_argument("Lookback is zero");
    }
    if (nums.size() < static_cast<size_t>(period)) {
        throw std::invalid_argument(
            "rocp: Not enough elements for lookback period.");
    }

    double prev_value = static_cast<double>(nums[nums.size() - period - 1]);
    double curr_value = static_cast<double>(nums.back());

    if (prev_value == 0.0 || std::isnan(prev_value) || std::isinf(prev_value)) {
        return 0.0;
    }

    return (curr_value - prev_value) / prev_value;
}

template <typename T>
double log_rocp(const std::span<T>& nums, int period) {
    if (nums.empty()) {
        throw std::invalid_argument("nums is empty");
    }
    if (period <= 0) {
        throw std::invalid_argument("period must be positive");
    }
    if (nums.size() < static_cast<size_t>(period + 1)) {
        throw std::invalid_argument(
            "log_rocp: Not enough elements for lookback period.");
    }

    double prev_value = static_cast<double>(nums[nums.size() - period - 1]);
    double curr_value = static_cast<double>(nums.back());

    if (prev_value <= 0.0 || std::isnan(prev_value) || std::isinf(prev_value)) {
        return 0.0;
    }

    double rocp = curr_value / prev_value;
    return std::log(rocp);
}

template <typename T>
double increasing_ratio(const std::vector<T>& nums, int lookback) {
    if (nums.empty()) {
        throw std::invalid_argument("Nums is empty");
    }
    if (lookback <= 0) {
        throw std::invalid_argument("Lookback is zero");
    }
    if (nums.size() < static_cast<size_t>(lookback)) {
        throw std::invalid_argument(
            "increasing_ratio: Not enough elements for lookback period.");
    }

    int winning_count = 0;

    for (size_t i = nums.size() - lookback; i < nums.size(); ++i) {
        if (nums[i] > nums[i - 1]) {
            ++winning_count;
        }
    }

    return (static_cast<double>(winning_count) / lookback) * 100.0;
}

template <typename T>
double off_high(const std::span<T>& nums, int lookback, bool use_all_data) {
    if (nums.empty()) {
        throw std::invalid_argument("Nums is empty");
    }
    if (lookback <= 0) {
        throw std::invalid_argument(
            "Lookback period must be greater than zero.");
    }
    if (!use_all_data && nums.size() < static_cast<size_t>(lookback)) {
        throw std::invalid_argument(
            "off_high: Not enough elements for lookback period.");
    }

    int effective_lookback =
        use_all_data ? static_cast<int>(nums.size())
                     : std::min(lookback, static_cast<int>(nums.size()));

    size_t start_index =
        std::max(nums.size() - effective_lookback, static_cast<size_t>(0));
    size_t end_index = nums.size();

    auto max_iter =
        std::max_element(nums.begin() + start_index, nums.begin() + end_index);
    double lookback_high = *max_iter;
    double curr_value = static_cast<double>(nums.back());

    if (lookback_high == 0.0) {
        return 0.0;
    }

    return ((lookback_high - curr_value) / lookback_high) * 100.0;
}

template <typename T>
double aroon_up(const std::vector<T>& nums, int lookback) {
    if (nums.empty()) {
        throw std::invalid_argument("Nums is empty");
    }
    if (lookback <= 0) {
        throw std::invalid_argument(
            "Lookback period must be greater than zero.");
    }
    if (nums.size() < static_cast<size_t>(lookback)) {
        throw std::invalid_argument(
            "aroon_up: Not enough elements for lookback period.");
    }

    int start_index = nums.size() - lookback;
    int end_index = static_cast<int>(nums.size() - 1);

    using ValueType = std::remove_const_t<T>;
    ValueType max_value = nums[start_index];

    for (int i = start_index; i <= end_index; ++i) {
        if (nums[i] > max_value) {
            max_value = nums[i];
        }
    }

    int days_since_high = 0;
    for (int i = end_index; i >= start_index; --i) {
        days_since_high++;
        if (nums[i] == max_value) {
            break;
        }
    }

    double aroon_up =
        100.0 * (static_cast<double>(lookback - days_since_high) / lookback);

    return aroon_up;
}

template <typename T>
double aroon_down(const std::vector<T>& nums, int lookback) {
    if (nums.empty()) {
        throw std::invalid_argument("Nums is empty");
    }
    if (lookback <= 0) {
        throw std::invalid_argument(
            "Lookback period must be greater than zero.");
    }
    if (nums.size() < static_cast<size_t>(lookback)) {
        throw std::invalid_argument(
            "aroon_down: Not enough elements for lookback period.");
    }

    int start_index = nums.size() - lookback;
    int end_index = static_cast<int>(nums.size() - 1);

    using ValueType = std::remove_const_t<T>;
    ValueType min_value = nums[start_index];

    for (int i = start_index; i <= end_index; ++i) {
        if (nums[i] < min_value) {
            min_value = nums[i];
        }
    }

    int ticks_since_low = 0;
    for (int i = end_index; i >= start_index; --i) {
        ticks_since_low++;
        if (nums[i] == min_value) {
            break;
        }
    }

    double aroon_down =
        100.0 * (static_cast<double>(lookback - ticks_since_low) / lookback);

    return aroon_down;
}

template <typename T>
bool top_percentile(const std::span<T>& nums, int lookback, double percentile) {
    if (nums.empty()) {
        throw std::invalid_argument("Nums is empty");
    }
    if (lookback <= 0) {
        throw std::invalid_argument(
            "Lookback period must be greater than zero.");
    }
    if (percentile <= 0 || percentile >= 100) {
        throw std::invalid_argument("Percentile must be between 0 and 100.");
    }
    if (nums.size() < static_cast<size_t>(lookback)) {
        throw std::invalid_argument(
            "top_percentile: Not enough elements for lookback period.");
    }

    size_t start_index = nums.size() - lookback;
    const T current_value = nums.back();

    size_t values_needed = static_cast<size_t>((percentile / 100.0) * lookback);
    size_t larger_count = 0;

    for (size_t i = start_index; i < nums.size() - 1; ++i) {
        if (nums[i] > current_value) {
            larger_count++;
            if (larger_count > values_needed) {
                return false;
            }
        }
    }

    return larger_count <= values_needed;
}

template <typename T>
bool bottom_percentile(const std::vector<T>& nums, int lookback,
                       double percentile) {
    if (nums.empty()) {
        throw std::invalid_argument("Nums is empty");
    }
    if (lookback <= 0) {
        throw std::invalid_argument(
            "Lookback period must be greater than zero.");
    }
    if (percentile <= 0 || percentile >= 100) {
        throw std::invalid_argument("Percentile must be between 0 and 100.");
    }
    if (nums.size() < static_cast<size_t>(lookback)) {
        throw std::invalid_argument(
            "bottom_percentile: Not enough elements for lookback period.");
    }

    size_t start_index = nums.size() - lookback;
    size_t end_index = nums.size();

    std::vector<T> window(nums.begin() + start_index, nums.begin() + end_index);

    size_t threshold_index =
        static_cast<size_t>((percentile / 100.0) * lookback);
    std::nth_element(window.begin(), window.begin() + threshold_index,
                     window.end());
    T threshold_value = window[threshold_index];
    return nums.back() <= threshold_value;
}

template <typename T>
std::vector<bool> top_percentile(const std::vector<T>& nums, int base_lookback,
                                 double percentile, int num_values) {
    if (nums.empty()) {
        throw std::invalid_argument("Nums is empty");
    }
    if (base_lookback <= 0) {
        throw std::invalid_argument(
            "Base lookback period must be greater than zero.");
    }
    if (percentile <= 0 || percentile >= 100) {
        throw std::invalid_argument("Percentile must be between 0 and 100.");
    }
    if (num_values <= 0) {
        throw std::invalid_argument("num_values must be greater than zero.");
    }
    if (nums.size() < static_cast<size_t>(base_lookback + num_values)) {
        throw std::invalid_argument(
            "top_percentile: Not enough elements for required lookback "
            "period.");
    }

    std::vector<bool> results;

    for (int offset = num_values - 1; offset >= 0; --offset) {
        int adjusted_lookback = base_lookback + offset;
        size_t start_index = nums.size() - adjusted_lookback;
        size_t end_index = nums.size() - offset;

        std::vector<T> window(nums.begin() + start_index,
                              nums.begin() + end_index);

        size_t threshold_index =
            static_cast<size_t>((1 - percentile / 100.0) * adjusted_lookback);
        std::nth_element(window.begin(), window.begin() + threshold_index,
                         window.end());
        T threshold_value = window[threshold_index];

        results.push_back(nums[nums.size() - offset - 1] >= threshold_value);
    }

    return results;
}

template <typename U>
[[gnu::always_inline]] inline double rocp(std::span<const U> nums, int period) {
    if (nums.empty()) {
        throw std::invalid_argument("Nums is empty");
    }
    if (period <= 0) {
        throw std::invalid_argument("Lookback is zero");
    }
    if (nums.size() < static_cast<size_t>(period)) {
        throw std::invalid_argument(
            "rocp: Not enough elements for lookback period.");
    }

    double prev_value = static_cast<double>(nums[nums.size() - period - 1]);
    double curr_value = static_cast<double>(nums.back());

    if (prev_value == 0.0 || std::isnan(prev_value) || std::isinf(prev_value)) {
        return 0.0;
    }

    return (curr_value / prev_value) - 1.0;
}

template <typename T>
[[gnu::always_inline]] inline double dema(std::span<const T> nums, int period) {
    if (nums.empty()) {
        throw std::invalid_argument("Nums is empty");
    }
    if (period <= 0) {
        throw std::invalid_argument("Period must be greater than zero");
    }
    if (nums.size() < static_cast<size_t>(period)) {
        throw std::invalid_argument(
            "dema: Not enough elements for period length");
    }

    const double alpha = 2.0 / (period + 1.0);
    const size_t n = nums.size();

    // --- Compute EMA1 series ---
    std::vector<double> ema1(n);
    ema1[0] = static_cast<double>(nums[0]);
    for (size_t i = 1; i < n; ++i) {
        ema1[i] =
            alpha * static_cast<double>(nums[i]) + (1.0 - alpha) * ema1[i - 1];
    }

    // --- Compute EMA2 = EMA(EMA1) ---
    std::vector<double> ema2(n);
    ema2[0] = ema1[0];
    for (size_t i = 1; i < n; ++i) {
        ema2[i] = alpha * ema1[i] + (1.0 - alpha) * ema2[i - 1];
    }

    // --- DEMA = 2*EMA1 - EMA2 ---
    return 2.0 * ema1.back() - ema2.back();
}

template <typename T>
double normalized_volatility(const std::span<T>& nums, int lookback) {
    if (nums.empty()) {
        throw std::invalid_argument(
            "Normalized volatility: Input vector is empty.");
    }
    if (lookback <= 0) {
        throw std::invalid_argument(
            "Normalized volatility: Lookback period must be greater than "
            "zero.");
    }
    if (nums.size() < static_cast<size_t>(lookback)) {
        throw std::invalid_argument(
            "normalized_volatility: Not enough elements for lookback period.");
    }

    size_t start_idx = nums.size() - lookback;

    double sum = 0.0;
    for (size_t i = start_idx; i < nums.size(); ++i) {
        sum += static_cast<double>(nums[i]);
    }
    double mean = sum / lookback;

    double sq_sum = 0.0;
    for (size_t i = start_idx; i < nums.size(); ++i) {
        double diff = static_cast<double>(nums[i]) - mean;
        sq_sum += diff * diff;
    }
    double std_dev = std::sqrt(sq_sum / (lookback - 1));
    return mean != 0.0 ? std_dev / mean : 0.0;
}

template <typename T>
double normalized_price_range(const std::span<T>& highs,
                              const std::span<T>& lows,
                              const std::span<T>& closes) {
    if (highs.empty() || lows.empty() || closes.empty()) {
        throw std::invalid_argument("Input spans are empty.");
    }
    if (highs.size() != lows.size() || highs.size() != closes.size()) {
        throw std::invalid_argument("Input spans must have the same size.");
    }

    double close = static_cast<double>(closes.back());
    if (close == 0.0 || std::isnan(close) || std::isinf(close)) {
        return 0.0;
    }

    return (static_cast<double>(highs.back()) -
            static_cast<double>(lows.back())) /
           close;
}

template <typename T>
double target(const std::span<T>& column_values, int shift_period) {
    if (column_values.empty()) {
        throw std::invalid_argument("Input vector is empty.");
    }
    if (shift_period <= 0) {
        throw std::invalid_argument("Shift period must be greater than zero.");
    }
    if (column_values.size() <= static_cast<size_t>(shift_period)) {
        throw std::invalid_argument(
            "target: Not enough elements for shift period.");
    }

    double current = static_cast<double>(
        column_values[column_values.size() - shift_period - 1]);
    double future = static_cast<double>(column_values.back());

    if (current > 0 && future > 0) {
        return future / current;
    }
    return 0.0;
}

template <typename T>
double target_log(const std::span<T>& column_values, int shift) {
    if (column_values.empty()) {
        throw std::invalid_argument("Input vector is empty.");
    }
    if (shift <= 0) {
        throw std::invalid_argument("Shift must be greater than zero.");
    }

    double current =
        static_cast<double>(column_values[column_values.size() - shift - 1]);
    double future = static_cast<double>(column_values.back());

    if (current > 0 && future > 0) {
        return std::log(future / current);
    }
    return 0.0;
}

template <typename T>
double high_minus_low_div_close_signed(const std::span<T>& highs,
                                       const std::span<T>& lows,
                                       const std::span<T>& closes,
                                       const std::span<T>& opens) {
    if (highs.empty() || lows.empty() || closes.empty() || opens.empty()) {
        throw std::invalid_argument("Input spans are empty.");
    }

    auto high = static_cast<double>(highs.back());
    auto low = static_cast<double>(lows.back());
    auto close = static_cast<double>(closes.back());
    auto open = static_cast<double>(opens.back());

    if (close == 0.0 || std::isnan(close) || std::isinf(close) || open == 0.0 ||
        std::isnan(open) || std::isinf(open)) {
        return 0.0;
    }

    auto high_low_diff = high - low;

    if (close > open) {
        return high_low_diff / close;
    }

    return -high_low_diff / close;
}

template <typename T>
double stdev(const std::span<T>& column_values, int lookback) {
    if (column_values.empty()) {
        throw std::invalid_argument("Input vector is empty.");
    }
    if (lookback <= 0) {
        throw std::invalid_argument(
            "Lookback period must be greater than zero.");
    }
    if (column_values.size() < static_cast<size_t>(lookback)) {
        throw std::invalid_argument(
            "stdev: Not enough elements for lookback period.");
    }

    double mean_value = 0.0;
    for (size_t i = column_values.size() - lookback; i < column_values.size();
         ++i) {
        mean_value += static_cast<double>(column_values[i]);
    }
    mean_value /= lookback;

    double sq_sum = 0.0;
    for (size_t i = column_values.size() - lookback; i < column_values.size();
         ++i) {
        double diff = static_cast<double>(column_values[i]) - mean_value;
        sq_sum += diff * diff;
    }
    return std::sqrt(sq_sum / lookback);
}

template <typename T>
std::vector<double> one_row_percentage_changes(
    const std::span<T>& column_values, int lookback) {
    if (column_values.empty()) {
        throw std::invalid_argument("Input vector is empty.");
    }

    if (column_values.size() < static_cast<size_t>(lookback + 1)) {
        throw std::invalid_argument(
            "one_row_percentage_changes: Not enough elements for lookback "
            "period.");
    }

    std::vector<double> percentage_changes;
    percentage_changes.reserve(lookback);

    for (size_t i = column_values.size() - lookback; i < column_values.size();
         ++i) {
        double current = static_cast<double>(column_values[i]);
        double previous = static_cast<double>(column_values[i - 1]);

        if (previous == 0.0 || std::isnan(previous) || std::isinf(previous)) {
            percentage_changes.push_back(
                std::numeric_limits<double>::quiet_NaN());
        } else {
            percentage_changes.push_back((current - previous) / previous);
        }
    }

    return percentage_changes;
}

template <typename T>
double mean(const std::span<T>& column_values, int lookback) {
    if (column_values.empty()) {
        throw std::invalid_argument("Input vector is empty.");
    }
    if (lookback <= 0) {
        throw std::invalid_argument(
            "Lookback period must be greater than zero.");
    }
    if (column_values.size() < static_cast<size_t>(lookback)) {
        throw std::invalid_argument(
            "mean: Not enough elements for lookback period.");
    }

    double sum = 0.0;
    for (size_t i = column_values.size() - lookback; i < column_values.size();
         ++i) {
        sum += static_cast<double>(column_values[i]);
    }
    return sum / lookback;
}

template <typename T>
double rolling_sharpe(const std::span<T>& column_values, int lookback,
                      double risk_free_rate) {
    if (column_values.empty()) {
        throw std::invalid_argument("Input vector is empty.");
    }
    if (lookback <= 0) {
        throw std::invalid_argument(
            "Lookback period must be greater than zero.");
    }
    if (column_values.size() < static_cast<size_t>(lookback + 1)) {
        throw std::invalid_argument(
            "rolling_sharpe: Not enough elements for lookback period.");
    }

    std::vector<double> percentage_changes =
        one_row_percentage_changes(column_values, lookback);

    std::vector<double> valid_changes;
    for (const auto& change : percentage_changes) {
        if (!std::isnan(change) && !std::isinf(change)) {
            valid_changes.push_back(change);
        }
    }

    if (valid_changes.empty()) {
        return 0.0;
    }

    double mean_percentage_change =
        std::accumulate(valid_changes.begin(), valid_changes.end(), 0.0) /
        valid_changes.size();

    double sq_sum = 0.0;
    for (const auto& change : valid_changes) {
        double diff = change - mean_percentage_change;
        sq_sum += diff * diff;
    }

    double stdev_percentage_change = std::sqrt(sq_sum / valid_changes.size());

    if (stdev_percentage_change == 0.0) {
        return 0.0;
    }

    return (mean_percentage_change - risk_free_rate) / stdev_percentage_change;
}

template <typename T>
double correlation_on_lookback(const std::span<T>& column_1_values,
                               const std::span<T>& column_2_values,
                               int lookback, int num_skip_recent_rows) {
    if (column_1_values.empty() || column_2_values.empty()) {
        throw std::invalid_argument("Input vectors are empty.");
    }
    if (column_1_values.size() != column_2_values.size()) {
        throw std::invalid_argument("Input vectors must have the same size.");
    }
    if (lookback <= 0) {
        throw std::invalid_argument("Lookback must be greater than zero.");
    }
    if (num_skip_recent_rows < 0) {
        throw std::invalid_argument("num_skip_recent_rows cannot be negative.");
    }

    const size_t start_idx = column_1_values.size() > lookback
                                 ? column_1_values.size() - lookback
                                 : 0;
    const size_t end_idx = column_1_values.size() - num_skip_recent_rows;

    if (end_idx <= start_idx || end_idx - start_idx < 2) {
        return 0.0;  // Not enough data points for correlation
    }

    double sum_x = 0.0, sum_y = 0.0, sum_xy = 0.0;
    double sum_x2 = 0.0, sum_y2 = 0.0;
    size_t n = 0;

    // Calculate sums for correlation formula
    for (size_t i = start_idx; i < end_idx; ++i) {
        double x = static_cast<double>(column_1_values[i]);
        double y = static_cast<double>(column_2_values[i]);

        if (std::isnan(x) || std::isinf(x) || std::isnan(y) || std::isinf(y)) {
            continue;
        }

        sum_x += x;
        sum_y += y;
        sum_xy += x * y;
        sum_x2 += x * x;
        sum_y2 += y * y;
        ++n;
    }

    if (n < 2) {
        std::cout << "Not enough valid data points" << std::endl;
        return 0.0;  // Not enough valid data points
    }

    // Calculate correlation coefficient
    double numerator = n * sum_xy - sum_x * sum_y;
    double denominator =
        std::sqrt((n * sum_x2 - sum_x * sum_x) * (n * sum_y2 - sum_y * sum_y));

    if (denominator == 0.0) {
        std::cout << "Start idx: " << start_idx << std::endl;
        std::cout << "End idx: " << end_idx << std::endl;
        std::cout << "Lookback: " << lookback << std::endl;
        std::cout << "Num skip recent rows: " << num_skip_recent_rows
                  << std::endl;
        std::cout << "Column 1 values size: " << column_1_values.size()
                  << std::endl;
        std::cout << "Column 2 values size: " << column_2_values.size()
                  << std::endl;
        std::cout << "Denominator is 0" << std::endl;
        return 0.0;
    }

    return numerator / denominator;
}

template <typename T>
double largest_downward_candle_distance(const std::span<T>& close_prices,
                                        int lookback) {
    if (close_prices.empty()) {
        throw std::invalid_argument("Input vector is empty.");
    }
    if (lookback <= 0) {
        throw std::invalid_argument("Lookback must be greater than zero.");
    }

    const size_t end_idx = close_prices.size();
    if (end_idx < lookback) {
        return 0.0;
    }

    size_t start_idx = end_idx - lookback;

    double max_drop = 100000;
    size_t relative_max_drop_idx = 0;

    for (size_t i = start_idx + 1; i < end_idx; ++i) {
        double prev_close = static_cast<double>(close_prices[i - 1]);
        double curr_close = static_cast<double>(close_prices[i]);

        if (std::isnan(prev_close) || std::isnan(curr_close) ||
            prev_close == 0) {
            continue;
        }

        double percentage_drop =
            ((curr_close - prev_close) / prev_close) * 100.0;
        size_t relative_idx = i - start_idx;

        if (percentage_drop < max_drop) {
            max_drop = percentage_drop;
            relative_max_drop_idx = relative_idx;
        }
    }

    double scaled_distance =
        (static_cast<double>(relative_max_drop_idx) / lookback) * 100.0;
    return scaled_distance;
}

template <typename T>
double abs_incr(const std::span<T>& values, int lookback) {
    if (values.empty()) {
        throw std::invalid_argument("Input vector is empty.");
    }
    if (lookback <= 0) {
        throw std::invalid_argument("Lookback must be greater than zero.");
    }
    if (values.size() <= static_cast<size_t>(lookback)) {
        return 0.0;
    }

    double new_val = static_cast<double>(values[values.size() - 1]);
    double old_val = static_cast<double>(values[values.size() - 1 - lookback]);

    return new_val - old_val;
}

template <typename T>
int volume_trend_indicator(const std::span<T>& volume_values,
                           const std::span<T>& price_values, int lookback,
                           double volume_threshold) {
    if (volume_values.empty() || price_values.empty()) {
        throw std::invalid_argument("Input vectors are empty.");
    }
    if (volume_values.size() != price_values.size()) {
        throw std::invalid_argument("Input vectors must have the same size.");
    }
    if (lookback <= 0) {
        throw std::invalid_argument("Lookback must be greater than zero.");
    }
    if (volume_values.size() < static_cast<size_t>(lookback)) {
        return 0;
    }

    size_t start_idx = volume_values.size() - lookback;
    double rolling_volume_mean = 0.0;

    // Manual calculation instead of std::accumulate
    for (size_t i = start_idx; i < volume_values.size(); ++i) {
        rolling_volume_mean += static_cast<double>(volume_values[i]);
    }
    rolling_volume_mean /= lookback;

    double latest_volume = static_cast<double>(volume_values.back());
    bool volume_spike =
        latest_volume > (rolling_volume_mean * volume_threshold);

    double latest_price = static_cast<double>(price_values.back());
    double old_price =
        static_cast<double>(price_values[price_values.size() - lookback]);
    double price_trend = latest_price - old_price;

    if (volume_spike) {
        return (price_trend > 0) ? 1 : -1;
    } else {
        return 0;
    }
}

template <typename T>
double sma(const std::span<T>& values, int lookback) {
    if (values.empty()) {
        throw std::invalid_argument("Input vector is empty.");
    }
    if (lookback <= 0) {
        throw std::invalid_argument("Lookback must be greater than zero.");
    }
    if (values.size() < static_cast<size_t>(lookback)) {
        throw std::invalid_argument(
            "sma: Not enough elements for lookback period.");
    }

    double sum = 0.0;
    for (size_t i = values.size() - lookback; i < values.size(); ++i) {
        sum += static_cast<double>(values[i]);
    }
    return sum / lookback;
}

template <typename T>
double high_minus_low_div_close_unsigned(const std::span<T>& highs,
                                         const std::span<T>& lows,
                                         const std::span<T>& closes,
                                         int lookback) {
    if (highs.empty() || lows.empty() || closes.empty()) {
        throw std::invalid_argument(
            "High minus low div close unsigned: Input spans are empty.");
    }
    if (lookback <= 0) {
        throw std::invalid_argument(
            "High minus low div close unsigned: Lookback must be greater than "
            "zero.");
    }
    if (highs.size() < static_cast<size_t>(lookback)) {
        throw std::invalid_argument(
            "High minus low div close unsigned: Not enough elements for "
            "lookback period.");
    }

    auto close = static_cast<double>(closes.back());
    if (close == 0.0 || std::isnan(close) || std::isinf(close)) {
        return 0.0;
    }

    // Find max high and min low over lookback period
    double high = static_cast<double>(highs.back());
    double low = static_cast<double>(lows.back());

    for (size_t i = closes.size() - lookback; i < closes.size(); ++i) {
        high = std::max(high, static_cast<double>(highs[i]));
        low = std::min(low, static_cast<double>(lows[i]));
    }

    return (high - low) / close;
}

template <typename T>
double high_minus_low_div_close_unsigned_log(const std::span<T>& highs,
                                             const std::span<T>& lows,
                                             const std::span<T>& closes,
                                             int lookback) {
    double ratio =
        high_minus_low_div_close_unsigned(highs, lows, closes, lookback);
    return std::log1p(ratio);  // log(1 + x) to prevent log(0) issues
}

template <typename T>
double rocp_ratio(const std::span<T>& nums, int period) {
    if (nums.empty()) {
        throw std::invalid_argument("Nums is empty");
    }
    if (period <= 0) {
        throw std::invalid_argument("Period must be greater than zero");
    }
    if (nums.size() < static_cast<size_t>(period)) {
        throw std::invalid_argument(
            "rocp_ratio: Not enough elements for period length");
    }

    double prev_value = static_cast<double>(nums[nums.size() - period - 1]);
    double curr_value = static_cast<double>(nums.back());

    if (prev_value == 0.0 || std::isnan(prev_value) || std::isinf(prev_value)) {
        return 1.0;
    }

    return curr_value / prev_value;
}

template <typename T>
double log_rocp_ratio(const std::span<T>& nums, int period) {
    double ratio = rocp_ratio(nums, period);
    return std::log(ratio);  // Natural log of the ratio
}

static int minutes_to_friday_8_utc_legacy(long long kline_open_time_ms) {
    time_t seconds = kline_open_time_ms / 1000;

    std::tm* timeinfo = gmtime(&seconds);

    int current_weekday = timeinfo->tm_wday;

    int python_weekday = (current_weekday + 6) % 7;

    int days_until_friday = (4 - python_weekday) % 7;
    if (days_until_friday < 0) days_until_friday += 7;

    std::tm target_time = *timeinfo;
    target_time.tm_hour = 0;
    target_time.tm_min = 0;
    target_time.tm_sec = 0;

    target_time.tm_mday += days_until_friday;

    target_time.tm_hour = 8;
    target_time.tm_min = 0;
    target_time.tm_sec = 0;

    time_t target_timestamp = timegm(&target_time);
    long long target_8am_ms = static_cast<long long>(target_timestamp) * 1000LL;

    if (kline_open_time_ms >= target_8am_ms) {
        target_8am_ms += 7 * 24 * 60 * 60 * 1000LL;
    }

    long long minutes_diff =
        (target_8am_ms - kline_open_time_ms) / (60 * 1000LL);
    return static_cast<int>(minutes_diff);
}

static int minutes_to_friday_8_utc(long long kline_open_time_ms) {
    using namespace std::chrono;

    const sys_time<milliseconds> now_ms{milliseconds{kline_open_time_ms}};
    const sys_days today = floor<days>(now_ms);
    const weekday wd{today};  // 0=Sun .. 6=Sat

    const int wd0 = wd.c_encoding();                   // 0..6
    const int python_weekday = (wd0 + 6) % 7;          // 0=Mon .. 6=Sun
    int days_until_friday = (4 - python_weekday) % 7;  // 4=Fri
    if (days_until_friday < 0) days_until_friday += 7;

    sys_time<milliseconds> target =
        sys_time<milliseconds>(today + days{days_until_friday}) + hours{8};

    if (now_ms >= target) {
        target += days{7};
    }

    return static_cast<int>(duration_cast<minutes>(target - now_ms).count());
}

template <typename T>
double largest_percent_drop(const std::span<T>& values, int pct_change_window,
                            int lookback_window) {
    if (values.empty()) {
        throw std::invalid_argument("Input vector is empty.");
    }
    if (pct_change_window <= 0) {
        throw std::invalid_argument(
            "Percent change window must be greater than zero.");
    }
    if (lookback_window <= 0) {
        throw std::invalid_argument(
            "Lookback window must be greater than zero.");
    }
    if (values.size() <
        static_cast<size_t>(pct_change_window + lookback_window)) {
        throw std::invalid_argument(
            "Not enough elements for required windows.");
    }

    std::vector<double> percent_changes;
    percent_changes.reserve(lookback_window);

    size_t start_idx = values.size() - lookback_window - pct_change_window;

    for (size_t i = 0; i < lookback_window; ++i) {
        size_t current_idx = start_idx + i + pct_change_window;
        size_t previous_idx = start_idx + i;

        auto current = static_cast<double>(values[current_idx]);
        auto previous = static_cast<double>(values[previous_idx]);

        if (previous == 0.0 || std::isnan(previous) || std::isinf(previous)) {
            percent_changes.push_back(0.0);
        } else {
            percent_changes.push_back(((current - previous) / previous) *
                                      100.0);
        }
    }

    auto min_iter =
        std::min_element(percent_changes.begin(), percent_changes.end());
    return *min_iter;
}

template <typename T>
int candles_since_largest_drop(const std::span<T>& values,
                               int pct_change_window, int lookback_window) {
    if (values.empty()) {
        throw std::invalid_argument("Input vector is empty.");
    }
    if (pct_change_window <= 0) {
        throw std::invalid_argument(
            "Percent change window must be greater than zero.");
    }
    if (lookback_window <= 0) {
        throw std::invalid_argument(
            "Lookback window must be greater than zero.");
    }
    if (values.size() <
        static_cast<size_t>(pct_change_window + lookback_window)) {
        throw std::invalid_argument(
            "Not enough elements for required windows.");
    }

    // Calculate percent changes for the lookback window
    std::vector<double> percent_changes;
    percent_changes.reserve(lookback_window);

    size_t start_idx = values.size() - lookback_window - pct_change_window;

    for (size_t i = 0; i < lookback_window; ++i) {
        size_t current_idx = start_idx + i + pct_change_window;
        size_t previous_idx = start_idx + i;

        double current = static_cast<double>(values[current_idx]);
        double previous = static_cast<double>(values[previous_idx]);

        if (previous == 0.0 || std::isnan(previous) || std::isinf(previous)) {
            percent_changes.push_back(0.0);
        } else {
            percent_changes.push_back(((current - previous) / previous) *
                                      100.0);
        }
    }

    // Find the index of the minimum (largest drop)
    auto min_iter =
        std::min_element(percent_changes.begin(), percent_changes.end());
    size_t min_idx = std::distance(percent_changes.begin(), min_iter);

    // Return candles since the largest drop (lookback_window - index)
    return static_cast<int>(lookback_window - min_idx);
}

template <typename T>
double largest_percent_gain(const std::span<T>& values, int pct_change_window,
                            int lookback_window) {
    if (values.empty()) {
        throw std::invalid_argument("Input vector is empty.");
    }
    if (pct_change_window <= 0) {
        throw std::invalid_argument(
            "Percent change window must be greater than zero.");
    }
    if (lookback_window <= 0) {
        throw std::invalid_argument(
            "Lookback window must be greater than zero.");
    }
    if (values.size() <
        static_cast<size_t>(pct_change_window + lookback_window)) {
        throw std::invalid_argument(
            "Not enough elements for required windows.");
    }

    // Calculate percent changes for the lookback window
    std::vector<double> percent_changes;
    percent_changes.reserve(lookback_window);

    size_t start_idx = values.size() - lookback_window - pct_change_window;

    for (size_t i = 0; i < lookback_window; ++i) {
        size_t current_idx = start_idx + i + pct_change_window;
        size_t previous_idx = start_idx + i;

        double current = static_cast<double>(values[current_idx]);
        double previous = static_cast<double>(values[previous_idx]);

        if (previous == 0.0 || std::isnan(previous) || std::isinf(previous)) {
            percent_changes.push_back(0.0);
        } else {
            percent_changes.push_back(((current - previous) / previous) *
                                      100.0);
        }
    }

    // Find the maximum (largest gain)
    auto max_iter =
        std::max_element(percent_changes.begin(), percent_changes.end());
    return *max_iter;
}

template <typename T>
int candles_since_largest_gain(const std::span<T>& values,
                               int pct_change_window, int lookback_window) {
    if (values.empty()) {
        throw std::invalid_argument("Input vector is empty.");
    }
    if (pct_change_window <= 0) {
        throw std::invalid_argument(
            "Percent change window must be greater than zero.");
    }
    if (lookback_window <= 0) {
        throw std::invalid_argument(
            "Lookback window must be greater than zero.");
    }
    if (values.size() <
        static_cast<size_t>(pct_change_window + lookback_window)) {
        throw std::invalid_argument(
            "Not enough elements for required windows.");
    }

    // Calculate percent changes for the lookback window
    std::vector<double> percent_changes;
    percent_changes.reserve(lookback_window);

    size_t start_idx = values.size() - lookback_window - pct_change_window;

    for (size_t i = 0; i < lookback_window; ++i) {
        size_t current_idx = start_idx + i + pct_change_window;
        size_t previous_idx = start_idx + i;

        double current = static_cast<double>(values[current_idx]);
        double previous = static_cast<double>(values[previous_idx]);

        if (previous == 0.0 || std::isnan(previous) || std::isinf(previous)) {
            percent_changes.push_back(0.0);
        } else {
            percent_changes.push_back(((current - previous) / previous) *
                                      100.0);
        }
    }

    // Find the index of the maximum (largest gain)
    auto max_iter =
        std::max_element(percent_changes.begin(), percent_changes.end());
    size_t max_idx = std::distance(percent_changes.begin(), max_iter);

    // Return candles since the largest gain (lookback_window - index)
    return static_cast<int>(lookback_window - max_idx);
}

template <typename T>
std::vector<std::remove_const_t<T>> aggregate_span(
    const std::span<T>& values, std::size_t window, std::string_view method,
    std::size_t max_lookback = std::numeric_limits<std::size_t>::max()) {
    if (values.empty()) throw std::invalid_argument("Input span is empty");
    if (window == 0) throw std::invalid_argument("Window must be > 0");
    if (values.size() < window)
        throw std::invalid_argument("Not enough data for window");

    using V = std::remove_const_t<T>;
    using A = std::conditional_t<std::is_integral_v<V>, long long, long double>;

    const std::size_t total_windows = values.size() - window + 1;
    const std::size_t keep_windows =
        max_lookback == std::numeric_limits<std::size_t>::max()
            ? total_windows
            : std::min(max_lookback, total_windows);

    const std::size_t first_window_id =
        total_windows - keep_windows;  // skip older ones

    std::vector<V> out;
    out.reserve(keep_windows);

    for (std::size_t w = first_window_id; w < total_windows; ++w) {
        const auto first = values.begin() + w;
        const auto last = first + window;  // one-past-the-end of the window

        if (method == "first")
            out.push_back(*first);
        else if (method == "last")
            out.push_back(*(last - 1));
        else if (method == "min")
            out.push_back(*std::min_element(first, last));
        else if (method == "max")
            out.push_back(*std::max_element(first, last));
        else if (method == "sum") {
            A acc = std::accumulate(first, last, A{});
            out.push_back(static_cast<V>(acc));
        } else if (method == "mean") {
            A acc = std::accumulate(first, last, A{});
            out.push_back(static_cast<V>(acc / window));
        } else
            throw std::invalid_argument("Unsupported aggregation method");
    }
    return out;
}

template <typename T>
std::vector<std::remove_const_t<T>> aggregate_span_first(
    const std::span<T>& values, int window, int max_lookback = -1) {
    return aggregate_span(values, window, "first", max_lookback);
}

template <typename T>
std::vector<std::remove_const_t<T>> aggregate_span_last(
    const std::span<T>& values, int window, int max_lookback = -1) {
    return aggregate_span(values, window, "last", max_lookback);
}

template <typename T>
std::vector<std::remove_const_t<T>> aggregate_span_min(
    const std::span<T>& values, int window, int max_lookback = -1) {
    return aggregate_span(values, window, "min", max_lookback);
}

template <typename T>
std::vector<std::remove_const_t<T>> aggregate_span_max(
    const std::span<T>& values, int window, int max_lookback = -1) {
    return aggregate_span(values, window, "max", max_lookback);
}

template <typename T>
std::vector<std::remove_const_t<T>> aggregate_span_sum(
    const std::span<T>& values, int window, int max_lookback = -1) {
    return aggregate_span(values, window, "sum", max_lookback);
}

template <typename T>
std::vector<std::remove_const_t<T>> aggregate_span_mean(
    const std::span<T>& values, int window, int max_lookback = -1) {
    return aggregate_span(values, window, "mean", max_lookback);
}

template <typename T>
double ratio_of_winning_candles(const std::span<T>& values,
                                int lookback_period) {
    if (values.empty()) {
        throw std::invalid_argument("Input span is empty.");
    }
    if (lookback_period <= 0) {
        throw std::invalid_argument(
            "Lookback period must be greater than zero.");
    }
    if (values.size() < static_cast<size_t>(lookback_period + 1)) {
        throw std::invalid_argument(
            "ratio_of_winning_candles: Not enough elements for lookback "
            "period.");
    }

    int winning_count = 0;
    size_t start_idx = values.size() - lookback_period;

    for (size_t i = start_idx + 1; i < values.size(); ++i) {
        if (values[i] > values[i - 1]) {
            ++winning_count;
        }
    }

    return (static_cast<double>(winning_count) / lookback_period) * 100;
}

template <typename T>
double calculate_rsi(const std::span<T>& values, int period) {
    if (values.empty()) {
        throw std::invalid_argument("Input span is empty.");
    }
    if (period <= 0) {
        throw std::invalid_argument("Period must be greater than zero.");
    }
    if (values.size() < static_cast<size_t>(period + 1)) {
        throw std::invalid_argument(
            "calculate_rsi: Not enough elements for RSI calculation.");
    }

    double gain_sum = 0.0;
    double loss_sum = 0.0;

    for (size_t i = values.size() - period; i < values.size(); ++i) {
        double delta =
            static_cast<double>(values[i]) - static_cast<double>(values[i - 1]);
        if (delta > 0.0) {
            gain_sum += delta;
        } else {
            loss_sum += -delta;
        }
    }

    double gain_mean = gain_sum / period;
    double loss_mean = loss_sum / period;

    if (loss_mean == 0.0) {
        return 100.0;  // All gains, RSI = 100
    }

    double rs = gain_mean / loss_mean;
    return 100.0 - (100.0 / (1.0 + rs));
}

inline std::optional<double> compute_intraday_price_change(
    long long market_open_time_ms,
    std::span<const long long> kline_open_times_ms,
    std::span<const double> close_prices, std::span<const double> open_prices) {
    if (kline_open_times_ms.empty() || close_prices.empty()) {
        return std::nullopt;
    }

    if (kline_open_times_ms.size() != close_prices.size() ||
        kline_open_times_ms.size() != open_prices.size()) {
        return std::nullopt;
    }

    int open_index = -1;
    for (int i = static_cast<int>(kline_open_times_ms.size()) - 1; i >= 0;
         --i) {
        if (kline_open_times_ms[i] == market_open_time_ms) {
            open_index = i;
            break;
        }
        if (kline_open_times_ms[i] < market_open_time_ms) {
            break;
        }
    }

    if (open_index == -1) {
        return std::nullopt;
    }

    double open_price = open_prices[open_index];
    double current_price = close_prices[close_prices.size() - 1];

    if (open_price == 0.0) {
        return std::nullopt;
    }

    return (current_price / open_price - 1.0) * 100.0;
}

inline std::optional<double> compute_overnight_gap(
    long long market_open_time_ms,
    std::span<const long long> kline_open_times_ms,
    std::span<const double> close_prices, std::span<const double> open_prices) {
    if (kline_open_times_ms.empty() || close_prices.empty()) {
        return std::nullopt;
    }

    if (kline_open_times_ms.size() != close_prices.size() ||
        kline_open_times_ms.size() != open_prices.size()) {
        return std::nullopt;
    }

    int open_index = -1;
    for (int i = static_cast<int>(kline_open_times_ms.size()) - 1; i >= 0;
         --i) {
        if (kline_open_times_ms[i] == market_open_time_ms) {
            open_index = i;
            break;
        }
        if (kline_open_times_ms[i] < market_open_time_ms) {
            break;
        }
    }

    if (open_index == -1 || open_index == 0) {
        return std::nullopt;
    }

    double previous_close = close_prices[open_index - 1];
    double current_open = open_prices[open_index];

    if (previous_close == 0.0) {
        return std::nullopt;
    }

    return (current_open / previous_close - 1.0);
}

template <typename T>
std::remove_const_t<T> abs_change_signed(const std::span<T>& values,
                                         int period) {
    if (values.empty()) {
        throw std::invalid_argument("Input vector is empty.");
    }
    if (period <= 0) {
        throw std::invalid_argument("Period must be greater than zero.");
    }
    if (values.size() <= static_cast<size_t>(period)) {
        throw std::invalid_argument(
            "abs_change_signed: Not enough elements for period.");
    }

    return values[values.size() - 1] - values[values.size() - 1 - period];
}

template <typename T>
double expected_pct_change_from_volatility(const std::span<T>& values,
                                           int window) {
    if (values.empty()) {
        throw std::invalid_argument("Input vector is empty.");
    }
    if (window <= 0) {
        throw std::invalid_argument("Window must be greater than zero.");
    }
    if (values.size() < static_cast<size_t>(window)) {
        throw std::invalid_argument(
            "expected_pct_change_from_volatility: Not enough elements for "
            "window.");
    }

    size_t start_idx = values.size() - window;

    double sum = 0.0;
    for (size_t i = start_idx; i < values.size(); ++i) {
        sum += static_cast<double>(values[i]);
    }
    double mean = sum / window;

    double sq_sum = 0.0;
    for (size_t i = start_idx; i < values.size(); ++i) {
        double diff = static_cast<double>(values[i]) - mean;
        sq_sum += diff * diff;
    }
    double std_dev = std::sqrt(sq_sum / window);

    auto current_price = static_cast<double>(values.back());
    if (current_price == 0.0 || std::isnan(current_price) ||
        std::isinf(current_price)) {
        return 0.0;
    }

    return (std_dev / current_price);
}

template <typename T, typename U>
double turnover(const std::span<T>& volumes, const std::span<U>& close_prices,
                int window) {
    if (volumes.empty() || close_prices.empty()) {
        throw std::invalid_argument("Input vectors are empty.");
    }
    if (volumes.size() != close_prices.size()) {
        throw std::invalid_argument(
            "Volumes and close prices must have the same size.");
    }
    if (window <= 0) {
        throw std::invalid_argument("Window must be greater than zero.");
    }
    if (volumes.size() < static_cast<size_t>(window)) {
        throw std::invalid_argument(
            "turnover: Not enough elements for window.");
    }

    size_t start_idx = volumes.size() - window;
    double sum = 0.0;
    for (size_t i = start_idx; i < volumes.size(); ++i) {
        sum += static_cast<double>(volumes[i]) *
               static_cast<double>(close_prices[i]);
    }
    return sum;
}

template <typename T>
T value_at_nearest_timestamp(const std::vector<long long>& timestamps,
                             const std::vector<T>& values,
                             long long desired_timestamp) {
    if (timestamps.empty() || values.empty()) {
        throw std::invalid_argument("Input vectors are empty.");
    }
    if (timestamps.size() != values.size()) {
        throw std::invalid_argument(
            "Timestamps and values must have the same size.");
    }

    auto it = std::lower_bound(timestamps.begin(), timestamps.end(),
                               desired_timestamp);

    size_t idx;
    if (it == timestamps.end()) {
        idx = timestamps.size() - 1;
    } else if (it == timestamps.begin()) {
        idx = 0;
    } else {
        long long prev_diff = desired_timestamp - *(it - 1);
        long long next_diff = *it - desired_timestamp;
        idx = (prev_diff <= next_diff) ? (it - timestamps.begin() - 1)
                                       : (it - timestamps.begin());
    }

    return values[idx];
}

template <typename T>
std::vector<T> values_from_nearest_timestamp(
    const std::vector<long long>& timestamps, const std::vector<T>& values,
    long long desired_timestamp) {
    if (timestamps.empty() || values.empty()) {
        throw std::invalid_argument("Input vectors are empty.");
    }
    if (timestamps.size() != values.size()) {
        throw std::invalid_argument(
            "Timestamps and values must have the same size.");
    }

    auto it = std::lower_bound(timestamps.begin(), timestamps.end(),
                               desired_timestamp);

    size_t idx;
    if (it == timestamps.end()) {
        idx = timestamps.size() - 1;
    } else if (it == timestamps.begin()) {
        idx = 0;
    } else {
        long long prev_diff = desired_timestamp - *(it - 1);
        long long next_diff = *it - desired_timestamp;
        idx = (prev_diff <= next_diff) ? (it - timestamps.begin() - 1)
                                       : (it - timestamps.begin());
    }

    std::vector<T> result;
    result.reserve(values.size() - idx);
    for (size_t i = idx; i < values.size(); ++i) {
        result.push_back(values[i]);
    }
    return result;
}

template <typename T>
auto sum_of_values(const std::vector<T>& values) {
    if (values.empty()) {
        return T{};
    }
    auto sum = values[0];
    for (size_t i = 1; i < values.size(); ++i) {
        sum += values[i];
    }
    return sum;
}

template <typename T>
double dist_from_interval_open(std::span<const long long> kline_close_times_ms,
                               const std::span<T>& open_prices,
                               const std::span<T>& close_prices,
                               long long interval_ms) {
    if (kline_close_times_ms.empty() || open_prices.empty() ||
        close_prices.empty()) {
        throw std::invalid_argument("Input spans are empty.");
    }
    if (kline_close_times_ms.size() != open_prices.size() ||
        kline_close_times_ms.size() != close_prices.size()) {
        throw std::invalid_argument("Input spans must have the same size.");
    }
    if (interval_ms <= 0) {
        throw std::invalid_argument("Interval must be greater than zero.");
    }

    long long current_ts = kline_close_times_ms.back();
    long long bucket_start = (current_ts / interval_ms) * interval_ms;

    // Search backwards to find the first candle in the current bucket
    size_t first_in_bucket = kline_close_times_ms.size() - 1;
    for (int i = static_cast<int>(kline_close_times_ms.size()) - 1; i >= 0;
         --i) {
        if (kline_close_times_ms[i] < bucket_start) {
            first_in_bucket = static_cast<size_t>(i + 1);
            break;
        }
        if (i == 0) {
            first_in_bucket = 0;
        }
    }

    double interval_open = static_cast<double>(open_prices[first_in_bucket]);
    double current_close = static_cast<double>(close_prices.back());

    if (interval_open == 0.0 || std::isnan(interval_open) ||
        std::isinf(interval_open)) {
        return 0.0;
    }

    return (current_close - interval_open) / interval_open;
}

static inline long long ms_since_midnight(long long timestamp_ms) {
    return timestamp_ms % (24LL * 60 * 60 * 1000);
}

static inline int day_of_week(long long timestamp_ms) {
    using namespace std::chrono;
    sys_time<milliseconds> tp{milliseconds{timestamp_ms}};
    auto sd = floor<days>(tp);
    weekday wd{sd};
    // Convert to Python convention: 0=Mon, 6=Sun
    return static_cast<int>((wd.c_encoding() + 6) % 7);
}

static inline long long time_left_in_interval(long long timestamp_ms,
                                               long long interval_ms) {
    return interval_ms - (timestamp_ms % interval_ms);
}

template <typename T, typename U>
double signed_volume(const std::span<T>& close_prices,
                     const std::span<T>& open_prices,
                     const std::span<U>& volumes, int lookback) {
    if (close_prices.empty() || open_prices.empty() || volumes.empty()) {
        throw std::invalid_argument("Input spans are empty.");
    }
    if (close_prices.size() != open_prices.size() ||
        close_prices.size() != volumes.size()) {
        throw std::invalid_argument("Input spans must have the same size.");
    }
    if (lookback <= 0) {
        throw std::invalid_argument("Lookback must be greater than zero.");
    }
    if (close_prices.size() < static_cast<size_t>(lookback)) {
        throw std::invalid_argument(
            "signed_volume: Not enough elements for lookback period.");
    }

    size_t start_idx = close_prices.size() - lookback;
    double signed_sum = 0.0;
    double vol_sum = 0.0;

    for (size_t i = start_idx; i < close_prices.size(); ++i) {
        double close = static_cast<double>(close_prices[i]);
        double open = static_cast<double>(open_prices[i]);
        double vol = static_cast<double>(volumes[i]);

        double sign = 0.0;
        if (close > open)
            sign = 1.0;
        else if (close < open)
            sign = -1.0;

        signed_sum += vol * sign;
        vol_sum += vol;
    }

    if (vol_sum <= 0.0) {
        return 0.0;
    }

    return signed_sum / vol_sum;
}

template <typename T, typename U>
double trade_size_regime(const std::span<T>& volumes,
                         const std::span<U>& number_of_trades,
                         int short_window, int long_window) {
    if (volumes.empty() || number_of_trades.empty()) {
        throw std::invalid_argument("Input spans are empty.");
    }
    if (volumes.size() != number_of_trades.size()) {
        throw std::invalid_argument("Input spans must have the same size.");
    }
    if (short_window <= 0 || long_window <= 0) {
        throw std::invalid_argument("Windows must be greater than zero.");
    }
    if (short_window > long_window) {
        throw std::invalid_argument(
            "Short window must be <= long window.");
    }
    if (volumes.size() < static_cast<size_t>(long_window)) {
        throw std::invalid_argument(
            "trade_size_regime: Not enough elements for long window.");
    }

    size_t n = volumes.size();
    size_t short_start = n - short_window;
    size_t long_start = n - long_window;

    double vol_short = 0.0, trades_short = 0.0;
    double vol_long = 0.0, trades_long = 0.0;

    for (size_t i = long_start; i < n; ++i) {
        double v = static_cast<double>(volumes[i]);
        double t = static_cast<double>(number_of_trades[i]);
        vol_long += v;
        trades_long += t;
        if (i >= short_start) {
            vol_short += v;
            trades_short += t;
        }
    }

    if (trades_short <= 0.0 || trades_long <= 0.0 || vol_long <= 0.0) {
        return 0.0;
    }

    double avg_short = vol_short / trades_short;
    double avg_long = vol_long / trades_long;

    return avg_short / avg_long;
}

template <typename T>
double volume_acceleration(const std::span<T>& volumes, int short_window,
                           int long_window) {
    if (volumes.empty()) {
        throw std::invalid_argument("Input span is empty.");
    }
    if (short_window <= 0 || long_window <= 0) {
        throw std::invalid_argument("Windows must be greater than zero.");
    }
    if (short_window > long_window) {
        throw std::invalid_argument(
            "Short window must be <= long window.");
    }
    if (volumes.size() < static_cast<size_t>(long_window)) {
        throw std::invalid_argument(
            "volume_acceleration: Not enough elements for long window.");
    }

    size_t n = volumes.size();
    size_t short_start = n - short_window;
    size_t long_start = n - long_window;

    double vol_short = 0.0;
    double vol_long = 0.0;

    for (size_t i = long_start; i < n; ++i) {
        double v = static_cast<double>(volumes[i]);
        vol_long += v;
        if (i >= short_start) {
            vol_short += v;
        }
    }

    if (vol_long <= 0.0) {
        return 0.0;
    }

    return (vol_short / vol_long) *
           (static_cast<double>(long_window) / short_window);
}

template <typename T, typename U>
double buy_pressure_ratio(const std::span<T>& taker_buy_volumes,
                          const std::span<U>& volumes, int lookback) {
    if (taker_buy_volumes.empty() || volumes.empty()) {
        throw std::invalid_argument("Input spans are empty.");
    }
    if (taker_buy_volumes.size() != volumes.size()) {
        throw std::invalid_argument("Input spans must have the same size.");
    }
    if (lookback <= 0) {
        throw std::invalid_argument("Lookback must be greater than zero.");
    }
    if (volumes.size() < static_cast<size_t>(lookback)) {
        throw std::invalid_argument(
            "buy_pressure_ratio: Not enough elements for lookback period.");
    }

    size_t start_idx = volumes.size() - lookback;
    double buy_sum = 0.0;
    double vol_sum = 0.0;

    for (size_t i = start_idx; i < volumes.size(); ++i) {
        buy_sum += static_cast<double>(taker_buy_volumes[i]);
        vol_sum += static_cast<double>(volumes[i]);
    }

    if (vol_sum <= 0.0) {
        return 0.0;
    }

    return buy_sum / vol_sum;
}

template <typename T>
double buy_pressure_vol_vs_sma(const std::span<T>& volumes, int short_window,
                               int long_window) {
    if (volumes.empty()) {
        throw std::invalid_argument("Input span is empty.");
    }
    if (short_window <= 0 || long_window <= 0) {
        throw std::invalid_argument("Windows must be greater than zero.");
    }
    if (short_window > long_window) {
        throw std::invalid_argument(
            "Short window must be <= long window.");
    }
    if (volumes.size() < static_cast<size_t>(long_window)) {
        throw std::invalid_argument(
            "buy_pressure_vol_vs_sma: Not enough elements for long window.");
    }

    size_t n = volumes.size();
    size_t short_start = n - short_window;
    size_t long_start = n - long_window;

    double vol_short = 0.0;
    double vol_long = 0.0;

    for (size_t i = long_start; i < n; ++i) {
        double v = static_cast<double>(volumes[i]);
        vol_long += v;
        if (i >= short_start) {
            vol_short += v;
        }
    }

    double vol_sma = vol_long / long_window;

    if (vol_sma <= 0.0) {
        return 0.0;
    }

    return vol_short / (vol_sma * short_window);
}

template <typename T, typename U>
double taker_flow_ofi(const std::span<T>& taker_buy_volumes,
                      const std::span<U>& volumes, int lookback) {
    if (taker_buy_volumes.empty() || volumes.empty()) {
        throw std::invalid_argument("Input spans are empty.");
    }
    if (taker_buy_volumes.size() != volumes.size()) {
        throw std::invalid_argument("Input spans must have the same size.");
    }
    if (lookback <= 0) {
        throw std::invalid_argument("Lookback must be greater than zero.");
    }
    if (volumes.size() < static_cast<size_t>(lookback)) {
        throw std::invalid_argument(
            "taker_flow_ofi: Not enough elements for lookback period.");
    }

    size_t start_idx = volumes.size() - lookback;
    double sum_delta = 0.0;
    double sum_vol = 0.0;

    for (size_t i = start_idx; i < volumes.size(); ++i) {
        double buy = static_cast<double>(taker_buy_volumes[i]);
        double vol = static_cast<double>(volumes[i]);
        sum_delta += 2.0 * buy - vol;
        sum_vol += vol;
    }

    if (sum_vol <= 0.0) {
        return 0.0;
    }

    return sum_delta / sum_vol;
}

template <typename T>
double taker_flow_vol_rocp(const std::span<T>& volumes, int sma_window,
                           int rocp_lag) {
    if (volumes.empty()) {
        throw std::invalid_argument("Input span is empty.");
    }
    if (sma_window <= 0 || rocp_lag <= 0) {
        throw std::invalid_argument(
            "SMA window and ROCP lag must be greater than zero.");
    }
    if (volumes.size() < static_cast<size_t>(sma_window + rocp_lag)) {
        throw std::invalid_argument(
            "taker_flow_vol_rocp: Not enough elements.");
    }

    size_t n = volumes.size();

    // sma_now: mean of last sma_window elements
    double sma_now = 0.0;
    for (size_t i = n - sma_window; i < n; ++i) {
        sma_now += static_cast<double>(volumes[i]);
    }
    sma_now /= sma_window;

    // sma_prev: mean of sma_window elements ending rocp_lag bars ago
    double sma_prev = 0.0;
    size_t prev_end = n - rocp_lag;
    size_t prev_start = prev_end - sma_window;
    for (size_t i = prev_start; i < prev_end; ++i) {
        sma_prev += static_cast<double>(volumes[i]);
    }
    sma_prev /= sma_window;

    if (sma_prev <= 0.0) {
        return 0.0;
    }

    return (sma_now - sma_prev) / sma_prev;
}

template <typename T, typename U>
double taker_flow_cvd_slope(const std::span<T>& taker_buy_volumes,
                            const std::span<U>& volumes, int cvd_window,
                            int slope_lag) {
    if (taker_buy_volumes.empty() || volumes.empty()) {
        throw std::invalid_argument("Input spans are empty.");
    }
    if (taker_buy_volumes.size() != volumes.size()) {
        throw std::invalid_argument("Input spans must have the same size.");
    }
    if (cvd_window <= 0 || slope_lag <= 0) {
        throw std::invalid_argument(
            "CVD window and slope lag must be greater than zero.");
    }
    if (volumes.size() < static_cast<size_t>(cvd_window + slope_lag)) {
        throw std::invalid_argument(
            "taker_flow_cvd_slope: Not enough elements.");
    }

    size_t n = volumes.size();

    // cvd_now: rolling sum of delta over last cvd_window bars
    double cvd_now = 0.0;
    double vol_sum = 0.0;
    for (size_t i = n - cvd_window; i < n; ++i) {
        double buy = static_cast<double>(taker_buy_volumes[i]);
        double vol = static_cast<double>(volumes[i]);
        cvd_now += 2.0 * buy - vol;
        vol_sum += vol;
    }

    // cvd_prev: rolling sum of delta over cvd_window bars ending slope_lag ago
    double cvd_prev = 0.0;
    size_t prev_end = n - slope_lag;
    size_t prev_start = prev_end - cvd_window;
    for (size_t i = prev_start; i < prev_end; ++i) {
        double buy = static_cast<double>(taker_buy_volumes[i]);
        double vol = static_cast<double>(volumes[i]);
        cvd_prev += 2.0 * buy - vol;
    }

    if (vol_sum <= 0.0) {
        return 0.0;
    }

    return (cvd_now - cvd_prev) / vol_sum;
}

template <typename T, typename U>
double relative_avg_trade_size(const std::span<T>& volumes,
                               const std::span<U>& number_of_trades,
                               int short_window, int long_window) {
    if (volumes.empty() || number_of_trades.empty()) {
        throw std::invalid_argument("Input spans are empty.");
    }
    if (volumes.size() != number_of_trades.size()) {
        throw std::invalid_argument("Input spans must have the same size.");
    }
    if (short_window <= 0 || long_window <= 0) {
        throw std::invalid_argument("Windows must be greater than zero.");
    }
    if (short_window > long_window) {
        throw std::invalid_argument(
            "Short window must be <= long window.");
    }
    if (volumes.size() < static_cast<size_t>(long_window)) {
        throw std::invalid_argument(
            "relative_avg_trade_size: Not enough elements for long window.");
    }

    size_t n = volumes.size();
    size_t short_start = n - short_window;
    size_t long_start = n - long_window;

    double vol_short = 0.0, trades_short = 0.0;
    double vol_long = 0.0, trades_long = 0.0;

    for (size_t i = long_start; i < n; ++i) {
        double v = static_cast<double>(volumes[i]);
        double t = static_cast<double>(number_of_trades[i]);
        vol_long += v;
        trades_long += t;
        if (i >= short_start) {
            vol_short += v;
            trades_short += t;
        }
    }

    if (trades_short <= 0.0 || trades_long <= 0.0) {
        return 1.0;
    }

    double avg_short = vol_short / trades_short;
    double avg_long = vol_long / trades_long;

    return avg_short / avg_long;
}

template <typename T>
double trade_count_intensity(const std::span<T>& number_of_trades,
                             int short_window, int long_window) {
    if (number_of_trades.empty()) {
        throw std::invalid_argument("Input span is empty.");
    }
    if (short_window <= 0 || long_window <= 0) {
        throw std::invalid_argument("Windows must be greater than zero.");
    }
    if (short_window > long_window) {
        throw std::invalid_argument(
            "Short window must be <= long window.");
    }
    if (number_of_trades.size() < static_cast<size_t>(long_window)) {
        throw std::invalid_argument(
            "trade_count_intensity: Not enough elements for long window.");
    }

    size_t n = number_of_trades.size();
    size_t short_start = n - short_window;
    size_t long_start = n - long_window;

    double short_sum = 0.0;
    double long_sum = 0.0;

    for (size_t i = long_start; i < n; ++i) {
        double t = static_cast<double>(number_of_trades[i]);
        long_sum += t;
        if (i >= short_start) {
            short_sum += t;
        }
    }

    if (long_sum <= 0.0) {
        return 1.0;
    }

    return (short_sum / long_sum) *
           (static_cast<double>(long_window) / short_window);
}

template <typename T>
double trade_count_zscore(const std::span<T>& number_of_trades,
                          int short_window, int long_window) {
    if (number_of_trades.empty()) {
        throw std::invalid_argument("Input span is empty.");
    }
    if (short_window <= 0 || long_window <= 0) {
        throw std::invalid_argument("Windows must be greater than zero.");
    }
    if (number_of_trades.size() <
        static_cast<size_t>(short_window + long_window - 1)) {
        throw std::invalid_argument(
            "trade_count_zscore: Not enough elements.");
    }

    size_t n = number_of_trades.size();

    // We need long_window consecutive short_sum values ending at the
    // most recent bar. The most recent short_sum covers
    // [n-short_window, n). The one before covers [n-short_window-1, n-1).
    // So the oldest short_sum we need starts at
    // n - short_window - (long_window - 1).

    // Compute initial short_sum for the oldest position
    size_t first_start = n - short_window - (long_window - 1);
    double short_sum = 0.0;
    for (size_t i = first_start; i < first_start + short_window; ++i) {
        short_sum += static_cast<double>(number_of_trades[i]);
    }

    double sum_of_sums = short_sum;
    double sum_of_sums_sq = short_sum * short_sum;

    // Slide the window forward to get remaining long_window - 1 values
    for (int j = 1; j < long_window; ++j) {
        size_t drop_idx = first_start + j - 1;
        size_t add_idx = first_start + short_window + j - 1;
        short_sum -= static_cast<double>(number_of_trades[drop_idx]);
        short_sum += static_cast<double>(number_of_trades[add_idx]);
        sum_of_sums += short_sum;
        sum_of_sums_sq += short_sum * short_sum;
    }

    // short_sum now holds the most recent value (current short_sum)
    double current_short_sum = short_sum;
    double mean = sum_of_sums / long_window;
    // Sample variance (ddof=1) to match Python rolling_std default
    double variance =
        (sum_of_sums_sq - mean * mean * long_window) / (long_window - 1);

    if (variance <= 0.0) {
        return 0.0;
    }

    double std_dev = std::sqrt(variance);
    return (current_short_sum - mean) / std_dev;
}

// ============================================================================
// Interval-based features (5m bucket features)
// ============================================================================

inline double distance_from_interval_extremes(
    std::span<const long long> kline_close_times_ms,
    std::span<const double> close_prices,
    std::span<const double> high_prices,
    std::span<const double> low_prices,
    long long interval_ms) {
    if (kline_close_times_ms.empty() || close_prices.empty() ||
        high_prices.empty() || low_prices.empty()) {
        return 0.5;
    }

    const size_t n = kline_close_times_ms.size();
    long long current_bucket = kline_close_times_ms.back() / interval_ms;

    // Scan backwards to find first index in this bucket
    size_t bucket_start = n - 1;
    while (bucket_start > 0 &&
           kline_close_times_ms[bucket_start - 1] / interval_ms ==
               current_bucket) {
        --bucket_start;
    }

    // Compute running high and low within the bucket
    double running_high = high_prices[bucket_start];
    double running_low = low_prices[bucket_start];
    for (size_t i = bucket_start + 1; i < n; ++i) {
        if (high_prices[i] > running_high) running_high = high_prices[i];
        if (low_prices[i] < running_low) running_low = low_prices[i];
    }

    double range = running_high - running_low;
    if (range < 1e-15) {
        return 0.5;
    }

    return (close_prices.back() - running_low) / range;
}

inline double pct_time_above_open(
    std::span<const long long> kline_close_times_ms,
    std::span<const double> close_prices,
    long long interval_ms) {
    if (kline_close_times_ms.empty() || close_prices.empty()) {
        return 0.0;
    }

    const size_t n = kline_close_times_ms.size();
    long long current_bucket = kline_close_times_ms.back() / interval_ms;

    size_t bucket_start = n - 1;
    while (bucket_start > 0 &&
           kline_close_times_ms[bucket_start - 1] / interval_ms ==
               current_bucket) {
        --bucket_start;
    }

    double interval_open = close_prices[bucket_start];
    size_t total_bars = n - bucket_start;
    size_t count_above = 0;

    for (size_t i = bucket_start; i < n; ++i) {
        if (close_prices[i] >= interval_open) {
            ++count_above;
        }
    }

    return static_cast<double>(count_above) / static_cast<double>(total_bars);
}

struct IntervalPriceLevels {
    double current_pct;
    double high_pct;
    double low_pct;
};

inline IntervalPriceLevels interval_price_levels(
    std::span<const long long> kline_close_times_ms,
    std::span<const double> close_prices,
    std::span<const double> high_prices,
    std::span<const double> low_prices,
    long long interval_ms) {
    if (kline_close_times_ms.empty() || close_prices.empty() ||
        high_prices.empty() || low_prices.empty()) {
        return {0.0, 0.0, 0.0};
    }

    const size_t n = kline_close_times_ms.size();
    long long current_bucket = kline_close_times_ms.back() / interval_ms;

    size_t bucket_start = n - 1;
    while (bucket_start > 0 &&
           kline_close_times_ms[bucket_start - 1] / interval_ms ==
               current_bucket) {
        --bucket_start;
    }

    double interval_open = close_prices[bucket_start];
    if (interval_open == 0.0 || std::isnan(interval_open) ||
        std::isinf(interval_open)) {
        return {0.0, 0.0, 0.0};
    }

    double running_high = high_prices[bucket_start];
    double running_low = low_prices[bucket_start];
    for (size_t i = bucket_start + 1; i < n; ++i) {
        if (high_prices[i] > running_high) running_high = high_prices[i];
        if (low_prices[i] < running_low) running_low = low_prices[i];
    }

    return {
        (close_prices.back() - interval_open) / interval_open,
        (running_high - interval_open) / interval_open,
        (running_low - interval_open) / interval_open,
    };
}

// Time-based realized volatility: sample std of pct_change(close)
// over bars within the last window_ms milliseconds.
// Returns are computed at each bar (ret[i] = (close[i]-close[i-1])/close[i-1])
// and the rolling window determines which returns are included.
inline double realized_volatility(
    std::span<const double> close_prices,
    std::span<const long long> kline_close_times_ms,
    long long window_ms) {
    if (close_prices.size() < 2) return 0.0;

    long long current_ts = kline_close_times_ms.back();
    long long window_start = current_ts - window_ms;

    // Find first bar in window via binary search
    size_t lo = 0, hi = kline_close_times_ms.size() - 1;
    size_t start = hi;  // default: only last bar
    while (lo <= hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (kline_close_times_ms[mid] > window_start) {
            start = mid;
            if (mid == 0) break;
            hi = mid - 1;
        } else {
            lo = mid + 1;
        }
    }

    // Returns are computed at each bar in [start, n-1] using the previous bar
    // The previous bar (i-1) may be before start, which is correct
    // (pct_change uses the immediately prior bar, rolling window selects
    //  which returns to aggregate)
    if (start < 1) start = 1;  // need at least one prior bar for returns

    double sum = 0.0;
    int count = 0;
    for (size_t i = start; i < close_prices.size(); ++i) {
        if (kline_close_times_ms[i] <= window_start) continue;
        double prev = close_prices[i - 1];
        if (prev != 0.0 && !std::isnan(prev)) {
            double ret = (close_prices[i] - prev) / prev;
            sum += ret;
            count++;
        }
    }
    if (count < 2) return 0.0;
    double mean = sum / count;

    double sq_sum = 0.0;
    for (size_t i = start; i < close_prices.size(); ++i) {
        if (kline_close_times_ms[i] <= window_start) continue;
        double prev = close_prices[i - 1];
        if (prev != 0.0 && !std::isnan(prev)) {
            double ret = (close_prices[i] - prev) / prev;
            double diff = ret - mean;
            sq_sum += diff * diff;
        }
    }
    return std::sqrt(sq_sum / (count - 1));
}

// Time-based ROCP: find past price at (current_ts - window_ms) using
// kline_open_time_ms, matching the Python join_asof forward approach.
// Finds the bar with smallest open_time >= (current_close_time - window_ms).
inline double rocp_time_based(
    std::span<const double> close_prices,
    std::span<const long long> kline_open_times_ms,
    std::span<const long long> kline_close_times_ms,
    long long window_ms) {
    if (close_prices.size() < 2) return 0.0;

    long long current_ts = kline_close_times_ms.back();
    long long target_open = current_ts - window_ms;

    // Binary search: find first bar with open_time >= target_open
    size_t lo = 0, hi = kline_open_times_ms.size() - 1;
    size_t best = kline_open_times_ms.size();  // sentinel
    while (lo <= hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (kline_open_times_ms[mid] >= target_open) {
            best = mid;
            if (mid == 0) break;
            hi = mid - 1;
        } else {
            lo = mid + 1;
        }
    }

    if (best >= kline_open_times_ms.size()) return 0.0;
    // Also check open_time <= current_ts (tolerance check)
    if (kline_open_times_ms[best] > current_ts) return 0.0;

    double past = close_prices[best];
    if (past == 0.0 || std::isnan(past) || std::isinf(past)) return 0.0;
    return (close_prices.back() - past) / past;
}

struct IntervalCrossings {
    long long time_since;
    int num_total;
    int num_last_1min;
};

inline IntervalCrossings interval_crossings(
    std::span<const long long> kline_close_times_ms,
    std::span<const double> close_prices,
    long long interval_ms,
    long long rolling_window_ms = 60000) {
    if (kline_close_times_ms.empty() || close_prices.empty()) {
        return {0, 0, 0};
    }

    const size_t n = kline_close_times_ms.size();
    long long current_ts = kline_close_times_ms.back();
    long long current_bucket = current_ts / interval_ms;
    long long interval_start_ms = current_bucket * interval_ms;
    long long rolling_cutoff = current_ts - rolling_window_ms;

    // Find current bucket start
    size_t bucket_start = n - 1;
    while (bucket_start > 0 &&
           kline_close_times_ms[bucket_start - 1] / interval_ms ==
               current_bucket) {
        --bucket_start;
    }

    double interval_open = close_prices[bucket_start];

    // Compute crossings within the current bucket
    long long last_crossing_ts = interval_start_ms;
    int total_crossings = 0;

    // For the rolling window, we need crossings from all buckets
    // whose bars fall within [rolling_cutoff, current_ts].
    // Find earliest bar in rolling window.
    size_t roll_start = n;
    for (size_t i = n; i > 0; --i) {
        if (kline_close_times_ms[i - 1] <= rolling_cutoff) {
            roll_start = i;
            break;
        }
        if (i == 1) roll_start = 0;
    }

    // Compute crossing flags for all bars from scan_start to end
    std::vector<bool> crossing_flags(n, false);

    size_t scan_start = std::min(roll_start, bucket_start);
    long long scan_bucket = kline_close_times_ms[scan_start] / interval_ms;
    while (scan_start > 0 &&
           kline_close_times_ms[scan_start - 1] / interval_ms ==
               scan_bucket) {
        --scan_start;
    }

    size_t i = scan_start;
    while (i < n) {
        long long bucket = kline_close_times_ms[i] / interval_ms;
        double bucket_open = close_prices[i];

        int prev_side = 0;
        {
            double diff = close_prices[i] - bucket_open;
            prev_side = (diff > 0.0) ? 1 : (diff < 0.0) ? -1 : 0;
        }

        size_t j = i + 1;
        while (j < n && kline_close_times_ms[j] / interval_ms == bucket) {
            double diff = close_prices[j] - bucket_open;
            int side = (diff > 0.0) ? 1 : (diff < 0.0) ? -1 : 0;

            bool is_crossing =
                (side != prev_side) && (side != 0) && (prev_side != 0);
            if (is_crossing) {
                crossing_flags[j] = true;
                if (bucket == current_bucket) {
                    total_crossings++;
                    last_crossing_ts = kline_close_times_ms[j];
                }
            }
            prev_side = side;
            ++j;
        }
        i = j;
    }

    // Time-based rolling sum: count crossings with timestamp > rolling_cutoff
    int rolling_count = 0;
    for (size_t k = roll_start; k < n; ++k) {
        if (crossing_flags[k] &&
            kline_close_times_ms[k] > rolling_cutoff) {
            rolling_count++;
        }
    }

    return {
        current_ts - last_crossing_ts,
        total_crossings,
        rolling_count,
    };
}

struct IntervalMoneyness {
    double dist_div_vol;
    double dist_div_sqrt_t;
    double dist_div_vol_sqrt_t;
};

inline IntervalMoneyness interval_moneyness(
    std::span<const long long> kline_close_times_ms,
    std::span<const double> close_prices,
    long long interval_ms,
    double realized_vol) {
    if (kline_close_times_ms.empty() || close_prices.empty()) {
        return {0.0, 0.0, 0.0};
    }

    const size_t n = kline_close_times_ms.size();
    long long current_ts = kline_close_times_ms.back();
    long long current_bucket = current_ts / interval_ms;

    // Find bucket start to get interval open
    size_t bucket_start = n - 1;
    while (bucket_start > 0 &&
           kline_close_times_ms[bucket_start - 1] / interval_ms ==
               current_bucket) {
        --bucket_start;
    }

    double interval_open = close_prices[bucket_start];
    if (interval_open == 0.0 || std::isnan(interval_open)) {
        return {0.0, 0.0, 0.0};
    }

    double distance_pct =
        (close_prices.back() - interval_open) / interval_open;

    double time_remaining =
        1.0 -
        static_cast<double>(current_ts % interval_ms) / interval_ms;
    time_remaining = std::max(time_remaining, 1e-9);
    time_remaining = std::min(time_remaining, 1.0);
    double sqrt_t = std::sqrt(time_remaining);

    double dist_div_vol =
        (realized_vol != 0.0) ? distance_pct / realized_vol : 0.0;
    double dist_div_sqrt_t = distance_pct / sqrt_t;
    double dist_div_vol_sqrt_t =
        (realized_vol != 0.0)
            ? distance_pct / (realized_vol * sqrt_t)
            : 0.0;

    return {dist_div_vol, dist_div_sqrt_t, dist_div_vol_sqrt_t};
}

}  // namespace TechnicalAnalysis