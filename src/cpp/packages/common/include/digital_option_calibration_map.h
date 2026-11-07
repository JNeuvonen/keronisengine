#pragma once

#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

struct DigitalOptionCalibrationMap {
    int time_bucket_ms = 0;
    int num_time_buckets = 0;
    int num_prob_bins = 0;
    std::vector<std::vector<float>> map;  // [time_bucket][prob_bin]

    bool loaded = false;

    static DigitalOptionCalibrationMap load(const std::string& path) {
        DigitalOptionCalibrationMap cm;
        std::ifstream f(path);
        if (!f.is_open()) {
            return cm;
        }
        try {
            nlohmann::json j = nlohmann::json::parse(f);
            cm.time_bucket_ms = j.value("time_bucket_ms", 3000);
            cm.num_time_buckets = j.value("num_time_buckets", 100);
            cm.num_prob_bins = j.value("num_prob_bins", 100);
            cm.map = j["map"].get<std::vector<std::vector<float>>>();
            cm.loaded = true;
            std::cout << "Loaded calibration map: " << path
                      << " (" << cm.num_time_buckets << "x"
                      << cm.num_prob_bins << ")" << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "Failed to load calibration map: " << e.what()
                      << std::endl;
        }
        return cm;
    }

    float apply(float raw_prediction, float time_value) const {
        if (!loaded) return raw_prediction;
        int tb = std::clamp(
            static_cast<int>(time_value / time_bucket_ms),
            0, num_time_buckets - 1);
        int pb = std::clamp(
            static_cast<int>(raw_prediction * num_prob_bins),
            0, num_prob_bins - 1);
        return map[tb][pb];
    }
};
