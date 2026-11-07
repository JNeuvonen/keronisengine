#pragma once

#include <libpq-fe.h>

#include <chrono>
#include <ctime>    // for std::tm, std::mktime
#include <iomanip>  // for std::get_time
#include <iostream>
#include <sstream>  // for std::istringstream

#include "common_types/strategy.h"

namespace Serialize {

std::string escape_pg_literal(PGconn* conn, const std::string& input);

template <typename T>
T get_value_from_json(const json& json_obj, const std::string& key,
                      const T& default_value = T()) {
    try {
        if (json_obj.contains(key)) {
            if constexpr (std::is_same_v<T, int>) {
                if (json_obj.at(key).is_number_integer()) {
                    return json_obj.at(key).get<int>();
                } else if (json_obj.at(key).is_string()) {
                    return std::stoi(json_obj.at(key).get<std::string>());
                }
            } else if constexpr (std::is_same_v<T, long>) {
                if (json_obj.at(key).is_number_integer()) {
                    return json_obj.at(key).get<long>();
                } else if (json_obj.at(key).is_string()) {
                    return std::stol(json_obj.at(key).get<std::string>());
                }
            } else if constexpr (std::is_same_v<T, long long>) {
                if (json_obj.at(key).is_number_integer()) {
                    return json_obj.at(key).get<long long>();
                } else if (json_obj.at(key).is_string()) {
                    return std::stoll(json_obj.at(key).get<std::string>());
                }
            } else if constexpr (std::is_same_v<T, double>) {
                if (json_obj.at(key).is_number()) {
                    return json_obj.at(key).get<double>();
                } else if (json_obj.at(key).is_string()) {
                    return std::stod(json_obj.at(key).get<std::string>());
                }
            } else if constexpr (std::is_same_v<T, bool>) {
                if (json_obj.at(key).is_boolean()) {
                    return json_obj.at(key).get<bool>();
                } else if (json_obj.at(key).is_string()) {
                    std::string value = json_obj.at(key).get<std::string>();
                    return (value == "true" || value == "t" || value == "1");
                }
            } else if constexpr (std::is_same_v<T, std::string>) {
                return json_obj.at(key).get<std::string>();
            } else {
                return json_obj.at(key).get<T>();
            }
        } else {
            std::cout << "Json object did not contain key " << key << std::endl;
        }
    } catch (const std::exception& e) {
    }
    return default_value;
}

template <typename T>
T safe_parse_number_from_string(const std::string& str) {
    if constexpr (std::is_same_v<T, int>) {
        size_t pos = 0;
        int value = std::stoi(str, &pos);
        if (pos == str.length()) {
            return value;
        }
    } else if constexpr (std::is_same_v<T, long long>) {
        size_t pos = 0;
        long long value = std::stoll(str, &pos);
        if (pos == str.length()) {
            return value;
        }
    } else if constexpr (std::is_same_v<T, double>) {
        size_t pos = 0;
        double value = std::stod(str, &pos);
        if (pos == str.length()) {
            return value;
        }
    }
    throw std::runtime_error("Invalid type for safe_parse");
}

inline std::chrono::system_clock::time_point parse_timestamp(
    const json& timestamp_value) {
    if (timestamp_value.is_string()) {
        std::string timestamp_str = timestamp_value.get<std::string>();
        std::tm tm = {};
        std::istringstream ss(timestamp_str);

        // Parse ISO 8601 format: YYYY-MM-DD HH:MM:SS
        ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");

        if (ss.fail()) {
            // Try alternative format with 'T' separator: YYYY-MM-DDTHH:MM:SS
            ss.clear();
            ss.str(timestamp_str);
            ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
        }

        if (!ss.fail()) {
            return std::chrono::system_clock::from_time_t(std::mktime(&tm));
        }
    }

    // Return epoch time if parsing fails
    return std::chrono::system_clock::from_time_t(0);
}

}  // namespace Serialize
