#include "math_utils.h"

#include <algorithm>
#include <random>

#include "ohlcv_dataset_tick.h"

double MathUtils::linear_mapping(double input, double x1, double y1, double x2,
                                 double y2, bool clamp) {
    double slope = (y2 - y1) / (x2 - x1);
    double result = y1 + slope * (input - x1);

    if (clamp) {
        if (y1 < y2)
            return std::clamp(result, y1, y2);
        else
            return std::clamp(result, y2, y1);
    }

    return result;
}

double MathUtils::exponential_mapping(double input, double x1, double y1,
                                      double x2, double y2,
                                      double epsilon,  // how close to y2 at x2
                                      bool clamp) {
    if (x1 == x2) return y2;  // avoid division by zero

    double k = -std::log(epsilon) / (x2 - x1);

    double result =
        y1 + (y2 - y1) * (1.0 - std::exp(-k * std::max(0.0, input - x1)));

    if (clamp) {
        if (y1 < y2)
            return std::clamp(result, y1, y2);
        else
            return std::clamp(result, y2, y1);
    }

    return result;
}

double MathUtils::power_mapping(double input, double x1, double y1, double x2,
                                double y2, double power, bool clamp) {
    if (x1 == x2) return y2;

    double norm = std::clamp((input - x1) / (x2 - x1), 0.0, 1.0);
    double result = y1 + (y2 - y1) * std::pow(norm, power);

    if (clamp) {
        if (y1 < y2)
            return std::clamp(result, y1, y2);
        else
            return std::clamp(result, y2, y1);
    }

    return result;
}

double MathUtils::power_mapping_with_start_ramp(double input, double x1,
                                                double y1, double x2, double y2,
                                                double power, double y_start,
                                                bool clamp) {
    if (x1 == x2) return y2;

    if (input < x1) return y1;

    double norm = std::clamp((input - x1) / (x2 - x1), 0.0, 1.0);
    double scaled_range = y2 - y1 - y_start;
    double result = y1 + y_start + scaled_range * std::pow(norm, power);

    if (clamp) {
        if (y1 < y2)
            return std::clamp(result, y1, y2);
        else
            return std::clamp(result, y2, y1);
    }

    return result;
}

double MathUtils::centered_power_skew(double value, double center, double edge,
                                      double max_output, double power) {
    double range = edge - center;
    if (range == 0.0) return 0.0;

    double x = (value - center) / range;
    x = std::clamp(x, -1.0, 1.0);

    double sign = (x >= 0.0) ? 1.0 : -1.0;
    return max_output * sign * std::pow(std::abs(x), power);
}

double MathUtils::randomize_number_between_range(double min, double max) {
    thread_local std::mt19937 generator(std::random_device{}());
    thread_local std::uniform_real_distribution<double> distribution(0.0, 1.0);

    return min + (max - min) * distribution(generator);
}