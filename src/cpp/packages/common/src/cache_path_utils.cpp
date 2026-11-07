#include "cache_path_utils.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

namespace CommonUtils {

std::string CachePathUtils::get_live_infra_cache_dir() {
  const char* data_dir_root = std::getenv("DATA_DIR_ROOT");
  if (!data_dir_root || data_dir_root[0] == '\0') {
    std::cerr << "WARNING: DATA_DIR_ROOT not set, using project root for cache "
                 "files"
              << std::endl;
    return ".";
  }
  std::string cache_dir =
      std::string(data_dir_root) + "/caches/live_infra";
  fs::create_directories(cache_dir);
  return cache_dir;
}

std::string CachePathUtils::get_state_cache_dir() {
  std::string dir = get_live_infra_cache_dir() + "/state";
  fs::create_directories(dir);
  return dir;
}

std::string CachePathUtils::get_sqlite_cache_dir() {
  std::string dir = get_live_infra_cache_dir() + "/sqlite";
  fs::create_directories(dir);
  return dir;
}

std::string CachePathUtils::get_symbols_cache_dir() {
  std::string dir = get_live_infra_cache_dir() + "/symbols";
  fs::create_directories(dir);
  return dir;
}

std::string CachePathUtils::get_trading_state_path() {
  return get_state_cache_dir() + "/trading_state.json";
}

std::string CachePathUtils::get_prevent_trading_lock_path() {
  return get_state_cache_dir() + "/prevent_trading_lock";
}

std::string CachePathUtils::get_sqlite_cache_path(
    const std::string& filename) {
  return get_sqlite_cache_dir() + "/" + filename;
}

std::string CachePathUtils::get_symbols_cache_path(
    const std::string& filename) {
  return get_symbols_cache_dir() + "/" + filename;
}

void CachePathUtils::migrate_cache_file_if_needed(
    const std::string& filename, const std::string& new_path) {
  try {
    // Check if old file exists in project root
    std::string old_path = "./" + filename;
    if (fs::exists(old_path) && !fs::exists(new_path)) {
      // Move file from old location to new location
      fs::rename(old_path, new_path);
      std::cerr << "INFO: Migrated cache file from " << old_path << " to "
                << new_path << std::endl;
    }
  } catch (const std::exception& e) {
    std::cerr << "WARNING: Failed to migrate cache file " << filename << ": "
              << e.what() << std::endl;
  }
}

}  // namespace CommonUtils
