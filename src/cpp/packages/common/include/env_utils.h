#pragma once

#include <cstdlib>
#include <string>

namespace EnvUtils {

inline std::string env_or(const char* name, const char* default_value) {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : std::string(default_value);
}

inline bool env_is_one(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && std::string(v) == "1";
}

}  // namespace EnvUtils
