#pragma once
#include <string>
#include <vector>

namespace StrUtils {
std::vector<std::string> split_str(const std::string& str,
                                   const std::string& delimiter);
}  // namespace StrUtils