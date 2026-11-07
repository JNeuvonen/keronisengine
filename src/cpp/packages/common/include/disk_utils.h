#pragma once

#include <filesystem>
#include <string>

namespace DiskUtils {

/**
 * @brief Get free disk space in bytes at the specified path
 * @param path The path to check disk space at
 * @return Free space in bytes
 * @throws std::runtime_error if the disk space check fails
 */
long long get_free_disk_space_mb(const std::filesystem::path& path);

}  // namespace DiskUtils