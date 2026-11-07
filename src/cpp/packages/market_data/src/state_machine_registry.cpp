#include "market_data/state_machine_registry.h"

#include <map>
#include <stdexcept>

namespace {

// Function-local static: registrations run during static initialization of
// other translation units.
std::map<std::string, MarketData::StateMachineFactory>& factories() {
    static std::map<std::string, MarketData::StateMachineFactory> instance;
    return instance;
}

}  // namespace

bool MarketData::StateMachineRegistry::add(const std::string& name,
                                           StateMachineFactory factory) {
    if (!factories().emplace(name, factory).second) {
        throw std::logic_error("State machine registered twice: " + name);
    }
    return true;
}

std::unique_ptr<MarketData::StrategyStateMachine>
MarketData::StateMachineRegistry::create(const std::string& name) {
    const auto it = factories().find(name);
    if (it == factories().end()) {
        throw std::runtime_error("Unknown state machine: " + name);
    }
    return it->second();
}

std::vector<std::string> MarketData::StateMachineRegistry::names() {
    std::vector<std::string> out;
    for (const auto& [name, factory] : factories()) out.push_back(name);
    return out;
}
