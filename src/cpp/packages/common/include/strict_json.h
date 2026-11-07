#pragma once

#include <algorithm>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "nlohmann/json.hpp"

// Reader for hand-written config: an unknown key or a wrongly typed value
// throws, so a misspelled key can never fall back to a default unnoticed.
class StrictJson {
   public:
    static void check_keys(const nlohmann::json& object,
                           std::initializer_list<const char*> allowed,
                           const std::string& context) {
        if (!object.is_object()) {
            throw std::runtime_error(context + ": expected an object, got " +
                                     object.type_name());
        }
        for (const auto& [key, value] : object.items()) {
            if (std::none_of(allowed.begin(), allowed.end(),
                             [&](const char* name) { return key == name; })) {
                throw std::runtime_error(context + ": unknown key '" + key +
                                         "'");
            }
        }
    }

    template <typename T>
    static void read(const nlohmann::json& object, const char* key, T& out,
                     const std::string& context) {
        if (const auto it = object.find(key); it != object.end()) {
            out = as<T>(*it, context + "." + key);
        }
    }

    template <typename T>
    static T require(const nlohmann::json& object, const char* key,
                     const std::string& context) {
        const auto it = object.find(key);
        if (it == object.end()) {
            throw std::runtime_error(context + ": missing required key '" +
                                     key + "'");
        }
        return as<T>(*it, context + "." + key);
    }

    static const nlohmann::json& array_at(const nlohmann::json& object,
                                          const char* key,
                                          const std::string& context) {
        static const nlohmann::json empty = nlohmann::json::array();
        const auto it = object.find(key);
        if (it == object.end()) return empty;
        if (!it->is_array()) {
            throw std::runtime_error(context + "." + key +
                                     ": expected an array, got " +
                                     it->type_name());
        }
        return *it;
    }

   private:
    template <typename T>
    static T as(const nlohmann::json& value, const std::string& where) {
        bool matches = true;
        if constexpr (std::is_same_v<T, bool>) {
            matches = value.is_boolean();
        } else if constexpr (std::is_integral_v<T>) {
            matches = value.is_number_integer();
        } else if constexpr (std::is_floating_point_v<T>) {
            matches = value.is_number();
        } else if constexpr (std::is_same_v<T, std::string>) {
            matches = value.is_string();
        } else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
            matches = value.is_array() &&
                      std::all_of(value.begin(), value.end(),
                                  [](const auto& e) { return e.is_string(); });
        }
        if (!matches) {
            throw std::runtime_error(where + ": unexpected " +
                                     value.type_name() + " value " +
                                     value.dump());
        }
        return value.get<T>();
    }
};
