#pragma once

#include <algorithm>
#include <cstddef>
#include <random>
#include <span>
#include <type_traits>

namespace MathUtils {

// Given a sorted array of quantile boundaries (N+1 values for N bins),
// returns a continuous percentile rank in [0, 1] using binary search
// and linear interpolation within the matched bin.
inline double percentile_rank(float value, std::span<const float> boundaries) {
    const int n_bins = static_cast<int>(boundaries.size()) - 1;
    if (n_bins <= 0) return 0.5;

    // Clamp to boundary range
    if (value <= boundaries.front()) return 0.0;
    if (value >= boundaries.back()) return 1.0;

    // Binary search: find the last boundary <= value
    auto it = std::upper_bound(boundaries.begin(), boundaries.end(), value);
    size_t idx = static_cast<size_t>(it - boundaries.begin()) - 1;

    // Linear interpolation within the bin
    float lo = boundaries[idx];
    float hi = boundaries[idx + 1];
    double frac = (hi > lo) ? static_cast<double>(value - lo) / (hi - lo) : 0.0;

    return (static_cast<double>(idx) + frac) / n_bins;
}

double linear_mapping(double input, double x1, double y1, double x2, double y2,
                      bool clamp = false);

double exponential_mapping(double input, double x1, double y1, double x2,
                           double y2,
                           double epsilon,  // how close to y2 at x2
                           bool clamp = false);

double power_mapping(double input, double x1, double y1, double x2, double y2,
                     double power, bool clamp);

double power_mapping_with_start_ramp(double input, double x1, double y1,
                                     double x2, double y2, double power,
                                     double y_start, bool clamp);

// Smooth, symmetric skew function centered at `center`.
// Maps `value` to a signed output in [-max_output, +max_output].
//   - At center: returns 0.
//   - At center ± (edge - center): returns ±max_output.
//   - Beyond edge: clamped to ±max_output.
//   - power controls curvature (1.0 = linear, >1 = concentrated at tails).
double centered_power_skew(double value, double center, double edge,
                           double max_output, double power);

double randomize_number_between_range(double min, double max);

template <typename T>
T randomize_number_between_range_v2(T min, T max) {
    thread_local std::mt19937 generator(std::random_device{}());

    if constexpr (std::is_integral_v<T>) {
        std::uniform_int_distribution<T> distribution(min, max);
        return distribution(generator);
    } else {
        thread_local std::uniform_real_distribution<T> dist(0.0, 1.0);
        return min + (max - min) * dist(generator);
    }
}

}  // namespace MathUtils
