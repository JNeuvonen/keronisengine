#pragma once

#include <string>

namespace CommonUtils {

class CachePathUtils {
 public:
  // Directory getters
  static std::string get_live_infra_cache_dir();
  static std::string get_state_cache_dir();
  static std::string get_sqlite_cache_dir();
  static std::string get_symbols_cache_dir();

  // Specific file paths
  static std::string get_trading_state_path();
  static std::string get_prevent_trading_lock_path();
  static std::string get_sqlite_cache_path(const std::string& filename);
  static std::string get_symbols_cache_path(const std::string& filename);

  // Auto-migration helper
  static void migrate_cache_file_if_needed(const std::string& filename,
                                           const std::string& new_path);
};

}  // namespace CommonUtils
