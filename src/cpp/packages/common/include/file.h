#pragma once
#include <string>

namespace CommonUtils {
void write_str_to_disk(const std::string& path, const std::string& str);

std::string read_file(const std::string& path);
}  // namespace CommonUtils
