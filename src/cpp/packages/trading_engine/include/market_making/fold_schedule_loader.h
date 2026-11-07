#pragma once

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "market_making/dtypes/market_event.h"
#include "nlohmann/json.hpp"

namespace Core::FoldScheduleLoader {

// Parsed schedule entry: when this fold becomes active + the ModelUpdateEvent
// payload (absolute ONNX paths, input columns per member, opaque metadata).
struct LoadedEntry {
    long long timestamp;
    Core::ModelUpdateEvent event;
};

// Parses a fold_schedule.json file. Returns entries in file order with each
// member's onnx_path resolved to absolute (relative to the schedule file's
// parent directory). Returns empty on missing file, parse error, or missing
// "entries" key — caller decides how to react. Callers must not assume a
// timestamp ordering — find the entry they need by predicate.
//
// Used by both backtest reader (filters by chunk window) and live init (picks
// latest entry whose timestamp <= now to seed active_bundle on startup).
inline std::vector<LoadedEntry> load(const std::string& schedule_path) {
    std::vector<LoadedEntry> out;
    std::ifstream f(schedule_path);
    if (!f) return out;
    nlohmann::json doc;
    try {
        f >> doc;
    } catch (...) {
        return out;
    }
    if (!doc.contains("entries")) return out;
    const auto schedule_dir =
        std::filesystem::path(schedule_path).parent_path();
    for (const auto& entry : doc.at("entries")) {
        LoadedEntry le;
        le.timestamp = entry.at("timestamp").get<long long>();
        le.event.schedule_path = schedule_path;
        for (const auto& m : entry.at("members")) {
            Core::ModelMemberSpec spec;
            spec.onnx_path =
                (schedule_dir / m.at("onnx_path").get<std::string>()).string();
            spec.input_columns =
                m.at("input_columns").get<std::vector<std::string>>();
            le.event.members.push_back(std::move(spec));
        }
        if (entry.contains("metadata")) {
            le.event.metadata = entry.at("metadata");
        }
        out.push_back(std::move(le));
    }
    return out;
}

}  // namespace Core::FoldScheduleLoader
