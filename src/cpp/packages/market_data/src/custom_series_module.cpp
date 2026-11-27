#include "market_data/custom_series_module.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace MarketData {

namespace {

CustomObservation parse_row_json(const nlohmann::json& row,
                                 const std::string& source_path) {
    if (!row.is_object() || !row.contains("available_ts_ms") ||
        !row["available_ts_ms"].is_number_integer()) {
        throw std::runtime_error(
            "CustomSeriesModule: row missing integer available_ts_ms in " +
            source_path);
    }
    CustomObservation obs;
    obs.available_ts_ms = row["available_ts_ms"].get<long long>();
    if (obs.available_ts_ms <= 0) {
        throw std::runtime_error(
            "CustomSeriesModule: available_ts_ms must be > 0 in " + source_path);
    }

    int numeric_count = 0;
    double last_numeric = 0.0;
    bool has_value_field = false;
    for (auto it = row.begin(); it != row.end(); ++it) {
        if (it.key() == "available_ts_ms") continue;
        if (it.value().is_number()) {
            const double v = it.value().get<double>();
            obs.values[it.key()] = v;
            last_numeric = v;
            ++numeric_count;
            if (it.key() == "value") {
                obs.value = v;
                has_value_field = true;
            }
        }
    }
    if (!has_value_field && numeric_count == 1) {
        obs.value = last_numeric;
    }
    return obs;
}

void parse_file_into(const fs::path& file_path,
                     std::vector<CustomObservation>& out) {
    std::ifstream in(file_path);
    if (!in.is_open()) {
        throw std::runtime_error("CustomSeriesModule: cannot open " +
                                 file_path.string());
    }
    const std::string ext = file_path.extension().string();
    if (ext == ".json") {
        nlohmann::json doc;
        in >> doc;
        if (!doc.is_array()) {
            throw std::runtime_error(
                "CustomSeriesModule: expected JSON array in " +
                file_path.string());
        }
        for (const auto& row : doc) {
            out.push_back(parse_row_json(row, file_path.string()));
        }
        return;
    }

    // Default: JSON Lines (.jsonl)
    std::string line;
    while (std::getline(in, line)) {
        const auto first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) continue;
        out.push_back(
            parse_row_json(nlohmann::json::parse(line), file_path.string()));
    }
}

}  // namespace

CustomSeriesModule::CustomSeriesModule(std::string dataset_name,
                                       std::string root_override)
    : dataset_name_(std::move(dataset_name)),
      root_override_(std::move(root_override)) {
    if (dataset_name_.empty()) {
        throw std::invalid_argument("CustomSeriesModule: dataset_name cannot be empty");
    }
}

std::string CustomSeriesModule::base_dir() const {
    if (!root_override_.empty()) {
        return root_override_ + "/" + dataset_name_;
    }
    return alt_data_root() + "/custom/" + dataset_name_;
}

std::string CustomSeriesModule::path_for(const std::string& key) const {
    const std::string base = base_dir() + "/" + key;
    std::error_code ec;
    if (fs::is_directory(base, ec)) return base;
    if (fs::is_regular_file(base + ".jsonl", ec)) return base + ".jsonl";
    if (fs::is_regular_file(base + ".json", ec)) return base + ".json";
    return base + ".jsonl";
}

long long CustomSeriesModule::content_fingerprint(const std::string& path) const {
    std::error_code ec;
    if (fs::is_directory(path, ec)) {
        return newest_mtime_ns_under(path);
    }
    return file_mtime_ns(path);
}

std::vector<CustomObservation> CustomSeriesModule::parse(
    const std::string& path) const {
    std::vector<CustomObservation> rows;
    std::error_code ec;
    if (fs::is_directory(path, ec)) {
        std::vector<fs::path> files;
        for (const auto& entry : fs::recursive_directory_iterator(path, ec)) {
            if (ec) break;
            if (!entry.is_regular_file(ec)) continue;
            const auto ext = entry.path().extension().string();
            if (ext == ".jsonl" || ext == ".json") {
                files.push_back(entry.path());
            }
        }
        std::sort(files.begin(), files.end());
        for (const auto& f : files) {
            parse_file_into(f, rows);
        }
    } else {
        parse_file_into(path, rows);
    }

    std::stable_sort(rows.begin(), rows.end(),
                     [](const CustomObservation& a, const CustomObservation& b) {
                         return a.available_ts_ms < b.available_ts_ms;
                     });
    return rows;
}

std::optional<CustomObservation> CustomSeriesModule::latest(
    const std::string& key, long long as_of_ms, long long max_age_ms) const {
    const auto entry = snapshot(key);
    if (entry == nullptr) return std::nullopt;
    const auto& rows = entry->payload;
    const auto it = std::upper_bound(
        rows.begin(), rows.end(), as_of_ms,
        [](long long t, const CustomObservation& row) {
            return t < row.available_ts_ms;
        });
    if (it == rows.begin()) return std::nullopt;
    const auto& row = *std::prev(it);
    if (max_age_ms != kNoMaxAge && (as_of_ms - row.available_ts_ms) > max_age_ms) {
        return std::nullopt;
    }
    return row;
}

std::optional<double> CustomSeriesModule::latest_value(
    const std::string& key, long long as_of_ms, const std::string& field,
    long long max_age_ms) const {
    const auto obs = latest(key, as_of_ms, max_age_ms);
    if (!obs.has_value()) return std::nullopt;
    const auto it = obs->values.find(field);
    if (it != obs->values.end()) return it->second;
    if (field == "value" && obs->values.size() == 1) return obs->value;
    return std::nullopt;
}

std::optional<std::vector<CustomObservation>> CustomSeriesModule::window(
    const std::string& key, long long from_ms_inclusive,
    long long to_ms_exclusive) const {
    const auto entry = snapshot(key);
    if (entry == nullptr) return std::nullopt;
    const auto& rows = entry->payload;
    std::vector<CustomObservation> out;
    auto it = std::lower_bound(
        rows.begin(), rows.end(), from_ms_inclusive,
        [](const CustomObservation& row, long long t) {
            return row.available_ts_ms < t;
        });
    for (; it != rows.end() && it->available_ts_ms < to_ms_exclusive; ++it) {
        out.push_back(*it);
    }
    return out;
}

}  // namespace MarketData
