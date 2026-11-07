#pragma once

#include <mpreal.h>
#include <serialize.h>

#include <chrono>
#include <functional>
#include <iostream>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <variant>

using namespace mpfr;

namespace CommonUtils {

std::string generate_random_id(size_t num_bits);

template <typename T, typename VariantType>
T extract_from_variant(const VariantType &variant, const T &fallback_value);

template <typename T, typename VariantType>
T extract_from_variant(const VariantType &variant, const T &fallback_value) {
    if (std::holds_alternative<T>(variant)) {
        return std::get<T>(variant);
    }
    return fallback_value;
}

template <typename T, typename... Types>
T safe_get(const std::variant<Types...> &var, T default_value = T{}) {
    if (auto *val = std::get_if<T>(&var)) {
        return *val;
    }
    return default_value;
}

inline std::string join(const std::vector<std::string> &vec,
                        const std::string &delim) {
    std::string result;
    for (size_t i = 0; i < vec.size(); ++i) {
        result += vec[i];
        if (i < vec.size() - 1) {
            result += delim;
        }
    }
    return result;
}

inline bool is_zero(double value, double epsilon = 1e-2) {
    return std::abs(value) < epsilon;
}

inline bool contains_only_zeros_and_dots(const std::string &str) {
    return std::all_of(str.begin(), str.end(),
                       [](char c) { return c == '0' || c == '.'; });
}

template <typename T>
T parse_safe(const nlohmann::json &json_obj, const std::string &key) {
    if (!json_obj.contains(key)) {
        throw std::runtime_error("Missing key in JSON: " + key);
    }

    if (!json_obj[key].is_string()) {
        throw std::runtime_error("Invalid type for key: " + key +
                                 ", expected string.");
    }

    return Serialize::safe_parse_number_from_string<T>(
        json_obj[key].get<std::string>());
}

std::string generate_unique_id();
std::string generate_unique_id_v2();

void to_lowercase(std::string &str);
void to_uppercase(std::string &str);

std::string truncate_to_precision(double value, int precision);

inline double mpreal_to_double(const mpreal &value) { return value.toDouble(); }

}  // namespace CommonUtils
