#pragma once

#include <memory>
#include <string>
#include <vector>

#include "market_data/strategy_state_machine.h"

namespace MarketData {

using StateMachineFactory = std::unique_ptr<StrategyStateMachine> (*)();

// Name -> factory for every state machine linked into the binary. Live
// startup and offline feature generation both build machines from here.
class StateMachineRegistry {
   public:
    static bool add(const std::string& name, StateMachineFactory factory);
    // Throws std::runtime_error for an unregistered name.
    static std::unique_ptr<StrategyStateMachine> create(const std::string& name);
    static std::vector<std::string> names();
};

}  // namespace MarketData

// Static-initializer registration: link the defining object file directly
// into the binary, never through a static library, or the linker drops it.
#define KERONIS_REGISTER_STATE_MACHINE(name, Type)                          \
    static const bool keronis_state_machine_registered_##Type               \
        [[maybe_unused]] = ::MarketData::StateMachineRegistry::add(         \
            name, []() -> std::unique_ptr<::MarketData::StrategyStateMachine> { \
                return std::make_unique<Type>();                            \
            })
