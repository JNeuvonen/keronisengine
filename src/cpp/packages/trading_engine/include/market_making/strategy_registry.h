#pragma once

#include <string>
#include <vector>

namespace Core {

class Strategy;

// Sets a freshly constructed Strategy's callbacks, bar types, signal
// exchanges and state machines.
using StrategyBinder = void (*)(Strategy&);
using StrategyNameMatcher = bool (*)(const std::string&);

class StrategyRegistry {
   public:
    // owns_state_machine: the binder registers a machine under a fixed name,
    // so a second live instance would collide and the strategy is never
    // rebuilt in place.
    static bool add(const std::string& name, StrategyBinder binder,
                    bool owns_state_machine = false);
    static bool add_matcher(StrategyNameMatcher matcher, StrategyBinder binder);
    static StrategyBinder find(const std::string& name);
    static bool owns_state_machine(const std::string& name);
    static std::vector<std::string> names();
};

}  // namespace Core

// Registration runs from a static initializer, so the strategy's object file
// must be linked directly into the binary: from a static library the linker
// drops it, since nothing references it.
#define KERONIS_REGISTER_STRATEGY(name, binder)                               \
    static const bool keronis_strategy_registered_##binder [[maybe_unused]] = \
        ::Core::StrategyRegistry::add(name, binder)

#define KERONIS_REGISTER_STRATEGY_OWNING_STATE_MACHINE(name, binder)          \
    static const bool keronis_strategy_registered_##binder [[maybe_unused]] = \
        ::Core::StrategyRegistry::add(name, binder, true)

#define KERONIS_REGISTER_STRATEGY_MATCHER(matcher, binder)                    \
    static const bool keronis_strategy_registered_##binder [[maybe_unused]] = \
        ::Core::StrategyRegistry::add_matcher(matcher, binder)
