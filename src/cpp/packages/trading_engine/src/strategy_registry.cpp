#include "market_making/strategy_registry.h"

#include <map>
#include <stdexcept>
#include <utility>

namespace {

struct Entry {
    Core::StrategyBinder binder;
    bool owns_state_machine;
};

struct Registry {
    std::map<std::string, Entry> by_name;
    std::vector<std::pair<Core::StrategyNameMatcher, Core::StrategyBinder>>
        matchers;
};

// Function-local static: registrations run during static initialization of
// other translation units, before any namespace-scope object here exists.
Registry& registry() {
    static Registry instance;
    return instance;
}

}  // namespace

bool Core::StrategyRegistry::add(const std::string& name,
                                 StrategyBinder binder,
                                 bool owns_state_machine) {
    if (!registry()
             .by_name.emplace(name, Entry{binder, owns_state_machine})
             .second) {
        throw std::logic_error("Strategy registered twice: " + name);
    }
    return true;
}

bool Core::StrategyRegistry::add_matcher(StrategyNameMatcher matcher,
                                         StrategyBinder binder) {
    registry().matchers.emplace_back(matcher, binder);
    return true;
}

Core::StrategyBinder Core::StrategyRegistry::find(const std::string& name) {
    if (const auto it = registry().by_name.find(name);
        it != registry().by_name.end()) {
        return it->second.binder;
    }
    for (const auto& [matcher, binder] : registry().matchers) {
        if (matcher(name)) return binder;
    }
    return nullptr;
}

bool Core::StrategyRegistry::owns_state_machine(const std::string& name) {
    const auto it = registry().by_name.find(name);
    return it != registry().by_name.end() && it->second.owns_state_machine;
}

std::vector<std::string> Core::StrategyRegistry::names() {
    std::vector<std::string> out;
    for (const auto& [name, entry] : registry().by_name) out.push_back(name);
    return out;
}
