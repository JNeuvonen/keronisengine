#pragma once

#include <memory>
#include <string>
#include <vector>

#include "market_data/alt_data_module.h"

namespace MarketData {

using AltDataFactory = std::unique_ptr<AltDataModule> (*)();

// Name -> factory for out-of-tree AltDataModule subclasses linked into the
// binary (including via -DKERONIS_STRATEGY_DIRS).
class AltDataRegistry {
   public:
    static bool add(const std::string& name, AltDataFactory factory);
    // Throws std::runtime_error for an unregistered name.
    static std::unique_ptr<AltDataModule> create(const std::string& name);
    static bool contains(const std::string& name);
    static std::vector<std::string> names();
};

}  // namespace MarketData

#define KERONIS_REGISTER_ALT_DATA(name, Type)                               \
    static const bool keronis_alt_data_registered_##Type [[maybe_unused]] = \
        ::MarketData::AltDataRegistry::add(                                 \
            name, []() -> std::unique_ptr<::MarketData::AltDataModule> {    \
                return std::make_unique<Type>();                            \
            })
