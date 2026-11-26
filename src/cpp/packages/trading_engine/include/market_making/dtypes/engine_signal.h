#pragma once

#include <functional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace Core {

// One row of engine_signal: an instruction for running engines, applied by the
// handler registered for its type. Written to the engine_signal table.
struct EngineSignal {
    long long id = 0;
    std::string signal_type;
    std::string key;
    nlohmann::json payload = nlohmann::json::object();
};

enum class EngineSignalStatus { APPLIED, SKIPPED, FAILED };

struct EngineSignalOutcome {
    EngineSignalStatus status = EngineSignalStatus::FAILED;
    std::string error;
};

inline const char* engine_signal_status_name(EngineSignalStatus status) {
    switch (status) {
        case EngineSignalStatus::APPLIED:
            return "applied";
        case EngineSignalStatus::SKIPPED:
            return "skipped";
        case EngineSignalStatus::FAILED:
            return "failed";
    }
    return "failed";
}

// Signal types persisted in the engine_signal table.
inline constexpr const char* ENGINE_SIGNAL_REMOVE_TRADE = "remove_trade";
inline constexpr const char* ENGINE_SIGNAL_STOP_MAKING_MARKETS =
    "stop_making_markets";
inline constexpr const char* ENGINE_SIGNAL_ALT_DATA_CHANGED = "alt_data_changed";
inline constexpr const char* ENGINE_SIGNAL_STRATEGY_COMMAND = "strategy_command";
// Read as state by live_infra's QuoteHolds, never through a handler or acked.
inline constexpr const char* ENGINE_SIGNAL_PULL_QUOTES = "pull_quotes";
inline constexpr const char* ENGINE_SIGNAL_RESUME_QUOTES = "resume_quotes";

using EngineSignalHandler =
    std::function<EngineSignalOutcome(const EngineSignal&)>;
// Unexpired signals of `signal_types` this engine has not acknowledged. Asking
// only for handled types keeps a type deployed ahead of its handler out of the
// fetch, instead of acknowledging it as skipped and losing it.
using FetchEngineSignalsCallback = std::function<std::vector<EngineSignal>(
    const std::vector<std::string>& signal_types)>;
using AckEngineSignalCallback =
    std::function<void(const EngineSignal&, const EngineSignalOutcome&)>;

}  // namespace Core
