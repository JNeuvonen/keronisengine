#pragma once

#include <cmath>
#include <random>
#include <unordered_map>
#include <vector>

namespace OptionsMonteCarlo {

// Batch-price digital call options: simulate paths ONCE, evaluate multiple
// strikes. Returns map of strike → P(spot_T > strike).
// All strikes must share the same underlying, expiry, and vol.
inline std::unordered_map<double, double> price_digital_call_batch(
    double spot, const std::vector<double>& strikes, double sigma_step,
    double time_to_expiry, double timestep_size, int num_paths = 5000) {
    std::unordered_map<double, double> results;
    if (strikes.empty()) return results;

    if (time_to_expiry <= 0.0) {
        for (double strike : strikes) {
            results[strike] = (spot > strike) ? 1.0 : 0.0;
        }
        return results;
    }

    int num_steps = static_cast<int>(time_to_expiry / timestep_size);
    if (num_steps <= 0) num_steps = 1;

    double drift_correction = -0.5 * sigma_step * sigma_step;

    thread_local std::mt19937 gen(std::random_device{}());
    std::normal_distribution<double> dist(0.0, 1.0);

    // Count per strike
    std::unordered_map<double, int> counts;
    for (double strike : strikes) counts[strike] = 0;

    for (int i = 0; i < num_paths; ++i) {
        double path_pos = spot;
        double path_neg = spot;

        for (int s = 0; s < num_steps; ++s) {
            double z = dist(gen);
            path_pos *= std::exp(drift_correction + sigma_step * z);
            path_neg *= std::exp(drift_correction - sigma_step * z);
        }

        for (auto& [strike, count] : counts) {
            if (path_pos > strike) ++count;
            if (path_neg > strike) ++count;
        }
    }

    double denom = 2.0 * num_paths;
    for (const auto& [strike, count] : counts) {
        results[strike] = static_cast<double>(count) / denom;
    }
    return results;
}

// Batch-price one-touch barrier options: simulate paths ONCE, evaluate
// multiple barriers. Returns map of barrier → P(path touches barrier).
inline std::unordered_map<double, double> price_one_touch_batch(
    double spot, const std::vector<double>& barriers, bool is_up_touch,
    double sigma_step, double time_to_expiry, double timestep_size,
    int num_paths = 5000) {
    std::unordered_map<double, double> results;
    if (barriers.empty()) return results;

    if (time_to_expiry <= 0.0) {
        for (double barrier : barriers) {
            if (is_up_touch)
                results[barrier] = (spot >= barrier) ? 1.0 : 0.0;
            else
                results[barrier] = (spot <= barrier) ? 1.0 : 0.0;
        }
        return results;
    }

    int num_steps = static_cast<int>(time_to_expiry / timestep_size);
    if (num_steps <= 0) num_steps = 1;

    double drift_correction = -0.5 * sigma_step * sigma_step;

    thread_local std::mt19937 gen(std::random_device{}());
    std::normal_distribution<double> dist(0.0, 1.0);

    // Per-path extremes, then check all barriers
    std::unordered_map<double, int> counts;
    for (double barrier : barriers) counts[barrier] = 0;

    for (int i = 0; i < num_paths; ++i) {
        double path_pos = spot;
        double path_neg = spot;
        double extreme_pos = spot;
        double extreme_neg = spot;

        for (int s = 0; s < num_steps; ++s) {
            double z = dist(gen);
            path_pos *= std::exp(drift_correction + sigma_step * z);
            path_neg *= std::exp(drift_correction - sigma_step * z);

            if (is_up_touch) {
                extreme_pos = std::max(extreme_pos, path_pos);
                extreme_neg = std::max(extreme_neg, path_neg);
            } else {
                extreme_pos = std::min(extreme_pos, path_pos);
                extreme_neg = std::min(extreme_neg, path_neg);
            }
        }

        for (auto& [barrier, count] : counts) {
            if (is_up_touch) {
                if (extreme_pos >= barrier) ++count;
                if (extreme_neg >= barrier) ++count;
            } else {
                if (extreme_pos <= barrier) ++count;
                if (extreme_neg <= barrier) ++count;
            }
        }
    }

    double denom = 2.0 * num_paths;
    for (const auto& [barrier, count] : counts) {
        results[barrier] = static_cast<double>(count) / denom;
    }
    return results;
}

}  // namespace OptionsMonteCarlo
