#include "market_data/alt_data_registry.h"

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace MarketData {

namespace {

std::mutex& registry_mutex() {
    static std::mutex m;
    return m;
}

std::unordered_map<std::string, AltDataFactory>& registry_map() {
    static std::unordered_map<std::string, AltDataFactory> map;
    return map;
}

}  // namespace

bool AltDataRegistry::add(const std::string& name, AltDataFactory factory) {
    if (name.empty() || factory == nullptr) return false;
    std::lock_guard<std::mutex> lock(registry_mutex());
    registry_map()[name] = factory;
    return true;
}

std::unique_ptr<AltDataModule> AltDataRegistry::create(const std::string& name) {
    std::lock_guard<std::mutex> lock(registry_mutex());
    const auto it = registry_map().find(name);
    if (it == registry_map().end()) {
        throw std::runtime_error("AltDataRegistry: unregistered module '" +
                                 name + "'");
    }
    return it->second();
}

bool AltDataRegistry::contains(const std::string& name) {
    std::lock_guard<std::mutex> lock(registry_mutex());
    return registry_map().count(name) > 0;
}

std::vector<std::string> AltDataRegistry::names() {
    std::lock_guard<std::mutex> lock(registry_mutex());
    std::vector<std::string> out;
    out.reserve(registry_map().size());
    for (const auto& [name, _] : registry_map()) {
        out.push_back(name);
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace MarketData
