#include "market_making/dtypes/exchange_info.h"

#include <fstream>
#include <nlohmann/json.hpp>
#include <random>

#include "cache_path_utils.h"
#include "ccxt_proxy.h"
#include "common_types/log_event_types.h"
#include "disk_logging/log.h"
#include "market_making/utils.h"

namespace Core {

ExchangeInfo& ExchangeInfo::getInstance() {
    static ExchangeInfo instance;
    return instance;
}

std::vector<std::string> ExchangeInfo::get_all_symbols_internal(
    const std::string& exchange_name, const std::string& market_type) {
    return get_symbols(exchange_name, market_type, false);
}

std::vector<std::string> ExchangeInfo::get_active_symbols(
    const std::string& exchange_name, const std::string& market_type) {
    return get_symbols(exchange_name, market_type, true);
}

std::vector<std::string> ExchangeInfo::get_symbols(
    const std::string& exchange_name, const std::string& market_type,
    bool active_only) {
    std::lock_guard<std::mutex> lock(exchange_info_mutex_);
    std::string cache_key = exchange_name + ":" + market_type + ":" +
                            (active_only ? "active" : "all");

    if (symbols_cache_.find(cache_key) != symbols_cache_.end()) {
        auto now = std::chrono::steady_clock::now();
        auto cache_time = symbols_cache_timestamps_[cache_key];

        if (now - cache_time < std::chrono::hours(1)) {
            return symbols_cache_[cache_key];
        }
    }

    try {
        std::vector<std::string> symbols;
        if (active_only) {
            symbols = CcxtProxy::instance().get_exchange_active_symbols(
                exchange_name, market_type);
        } else {
            symbols = CcxtProxy::instance().get_exchange_symbols(exchange_name,
                                                                 market_type);
        }

        if (!symbols.empty()) {
            LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_EXCHANGE_INFO_PRELOADED,
                     "ExchangeInfo::get_symbols: HTTP returned " +
                         std::to_string(symbols.size()) + " symbols for " +
                         exchange_name + " " + market_type,
                     nlohmann::json{});
            symbols_cache_[cache_key] = symbols;
            symbols_cache_timestamps_[cache_key] =
                std::chrono::steady_clock::now();
            sync_symbols_to_disk(exchange_name, market_type);
        } else {
            LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_EXCHANGE_INFO_FALLBACK_TO_DISK,
                        "ExchangeInfo::get_symbols: HTTP returned empty for " +
                            exchange_name + " " + market_type +
                            ", falling back to disk cache",
                        nlohmann::json{});
            load_symbols_from_disk(exchange_name, market_type);
            symbols = symbols_cache_[cache_key];
            if (symbols.empty()) {
                LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_EXCHANGE_INFO_LOAD_FAILED,
                          "ExchangeInfo::get_symbols: disk cache also empty "
                          "for " +
                              exchange_name + " " + market_type,
                          nlohmann::json{});
            } else {
                LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_EXCHANGE_INFO_PRELOADED,
                         "ExchangeInfo::get_symbols: disk cache returned " +
                             std::to_string(symbols.size()) + " symbols for " +
                             exchange_name + " " + market_type,
                         nlohmann::json{});
            }
        }

        return symbols;
    } catch (...) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_EXCHANGE_INFO_FALLBACK_TO_DISK,
                    "ExchangeInfo::get_symbols: exception caught for " +
                        exchange_name + " " + market_type +
                        ", falling back to disk cache",
                    nlohmann::json{});
        load_symbols_from_disk(exchange_name, market_type);
        const auto& symbols = symbols_cache_[cache_key];
        if (symbols.empty()) {
            LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_EXCHANGE_INFO_LOAD_FAILED,
                      "ExchangeInfo::get_symbols: no symbols available from "
                      "any source for " +
                          exchange_name + " " + market_type,
                      nlohmann::json{});
            return {};
        }
        LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_EXCHANGE_INFO_PRELOADED,
                 "ExchangeInfo::get_symbols: disk cache returned " +
                     std::to_string(symbols.size()) + " symbols for " +
                     exchange_name + " " + market_type,
                 nlohmann::json{});
        return symbols;
    }
}

std::vector<Core::SymbolExchangeInfo> ExchangeInfo::get_exchange_symbols_info(
    const std::string& exchange_name, const std::string& market_type) {
    std::lock_guard<std::mutex> lock(exchange_info_mutex_);
    const auto& infos = CcxtProxy::instance().get_exchange_symbols_info(
        exchange_name, market_type);

    if (infos.empty()) {
        const auto& disk_cached_info =
            load_symbol_info_from_disk(exchange_name, market_type);

        if (disk_cached_info.empty()) {
            return {};
        }
        update_symbols_lookup_cache(exchange_name, market_type,
                                    disk_cached_info);
        return disk_cached_info;
    }
    update_symbols_lookup_cache(exchange_name, market_type, infos);
    sync_symbol_info_to_disk(exchange_name, market_type, infos);
    return infos;
}

bool ExchangeInfo::is_symbol_active(const std::string& symbol,
                                    const std::string& exchange_name,
                                    const std::string& market_type) {
    std::lock_guard<std::mutex> lock(exchange_info_mutex_);
    try {
        const std::string key = Utils::get_symbol_exchange_market_key(
            symbol, exchange_name, market_type);
        const auto it = active_symbols_cache_.find(key);
        if (it != active_symbols_cache_.end()) {
            return it->second;
        }
        return false;
    } catch (...) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_SYMBOL_INFO_REFRESH_FAILED,
                  "Failed to check if symbol " + symbol + " is active for " +
                      exchange_name + " " + market_type,
                  nlohmann::json{});
        return false;
    }
}

void ExchangeInfo::sync_symbols_to_disk(const std::string& exchange_name,
                                        const std::string& market_type) {
    try {
        std::string all_basename =
            "symbols_cache_" + exchange_name + "_" + market_type + "_all.json";
        std::string active_basename = "symbols_cache_" + exchange_name + "_" +
                                      market_type + "_active.json";

        std::string all_filename =
            CommonUtils::CachePathUtils::get_symbols_cache_path(all_basename);
        std::string active_filename =
            CommonUtils::CachePathUtils::get_symbols_cache_path(active_basename);

        // Generate random temp filename
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<> dis(100000, 999999);

        std::string all_temp_filename =
            all_filename + "." + std::to_string(dis(gen)) + ".tmp";
        std::string active_temp_filename =
            active_filename + "." + std::to_string(dis(gen)) + ".tmp";

        std::string all_key = exchange_name + ":" + market_type + ":all";
        std::string active_key = exchange_name + ":" + market_type + ":active";

        // Sync all symbols
        if (symbols_cache_.find(all_key) != symbols_cache_.end()) {
            nlohmann::json all_cache_data;
            all_cache_data["exchange_name"] = exchange_name;
            all_cache_data["market_type"] = market_type;
            all_cache_data["symbols"] = symbols_cache_[all_key];

            std::ofstream all_temp_file(all_temp_filename);
            if (all_temp_file.is_open()) {
                all_temp_file << all_cache_data.dump(4);
                all_temp_file.close();

                if (std::rename(all_temp_filename.c_str(),
                                all_filename.c_str()) != 0) {
                    LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_SYMBOL_INFO_PERSIST_FAILED,
                              "Failed to rename all symbols temp file: " +
                                  all_temp_filename + " -> " + all_filename,
                              nlohmann::json{});
                    std::remove(all_temp_filename.c_str());
                }
            }
        }

        // Sync active symbols
        if (symbols_cache_.find(active_key) != symbols_cache_.end()) {
            nlohmann::json active_cache_data;
            active_cache_data["exchange_name"] = exchange_name;
            active_cache_data["market_type"] = market_type;
            active_cache_data["symbols"] = symbols_cache_[active_key];

            std::ofstream active_temp_file(active_temp_filename);
            if (active_temp_file.is_open()) {
                active_temp_file << active_cache_data.dump(4);
                active_temp_file.close();

                if (std::rename(active_temp_filename.c_str(),
                                active_filename.c_str()) != 0) {
                    LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_SYMBOL_INFO_PERSIST_FAILED,
                              "Failed to rename active symbols temp file: " +
                                  active_temp_filename + " -> " +
                                  active_filename,
                              nlohmann::json{});
                    std::remove(active_temp_filename.c_str());
                }
            }
        }
    } catch (...) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_SYMBOL_INFO_PERSIST_FAILED,
                  "Failed to sync symbols to disk for " + exchange_name + " " +
                      market_type,
                  nlohmann::json{});
    }
}

void ExchangeInfo::sync_symbol_info_to_disk(
    const std::string& exchange_name, const std::string& market_type,
    const std::vector<Core::SymbolExchangeInfo>& infos) {
    try {
        std::string filename_basename =
            "symbols_info_cache_" + exchange_name + "_" + market_type + ".json";
        std::string filename =
            CommonUtils::CachePathUtils::get_symbols_cache_path(filename_basename);

        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<> dis(100000, 999999);

        std::string temp_filename =
            filename + "." + std::to_string(dis(gen)) + ".tmp";

        nlohmann::json cache_data;
        cache_data["exchange_name"] = exchange_name;
        cache_data["market_type"] = market_type;
        cache_data["symbols_info"] = nlohmann::json::array();
        for (const auto& info : infos) {
            cache_data["symbols_info"].push_back(info.to_json());
        }

        std::ofstream temp_file(temp_filename);
        if (temp_file.is_open()) {
            temp_file << cache_data.dump(4);
            temp_file.close();

            if (std::rename(temp_filename.c_str(), filename.c_str()) != 0) {
                LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_SYMBOL_INFO_PERSIST_FAILED,
                          "Failed to rename symbol info temp file: " +
                              temp_filename + " -> " + filename,
                          nlohmann::json{});
                std::remove(temp_filename.c_str());
            }
        }
    } catch (...) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_SYMBOL_INFO_PERSIST_FAILED,
                  "Failed to sync symbol info to disk for " + exchange_name +
                      " " + market_type,
                  nlohmann::json{});
    }
}

bool ExchangeInfo::is_symbol_allowing_margin_trade(
    const std::string& symbol, const std::string& exchange_name,
    const std::string& market_type) {
    std::lock_guard<std::mutex> lock(exchange_info_mutex_);
    const std::string key = Utils::get_symbol_exchange_market_key(
        symbol, exchange_name, market_type);
    const auto it = allows_margin_symbols_cache_.find(key);
    if (it != allows_margin_symbols_cache_.end()) {
        return it->second;
    }
    return false;
}

void ExchangeInfo::update_symbols_for_exchange_and_market_type(
    const std::string& exchange_name, const std::string& market_type) {
    std::lock_guard<std::mutex> lock(exchange_info_mutex_);
    std::vector<std::string> symbols =
        CcxtProxy::instance().get_exchange_active_symbols(exchange_name,
                                                          market_type);
    std::vector<Core::SymbolExchangeInfo> infos =
        CcxtProxy::instance().get_exchange_symbols_info(exchange_name,
                                                        market_type);
    update_symbols_lookup_cache(exchange_name, market_type, infos);
}

void ExchangeInfo::update_symbols_lookup_cache(
    const std::string& exchange_name, const std::string& market_type,
    const std::vector<Core::SymbolExchangeInfo>& infos) {
    for (const auto& info : infos) {
        allows_margin_symbols_cache_[Utils::get_symbol_exchange_market_key(
            info.symbol, exchange_name, market_type)] = info.allows_margin;
        active_symbols_cache_[Utils::get_symbol_exchange_market_key(
            info.symbol, exchange_name, market_type)] = info.is_active;
    }
}

void ExchangeInfo::load_symbols_from_disk(const std::string& exchange_name,
                                          const std::string& market_type) {
    try {
        std::string all_key = exchange_name + ":" + market_type + ":all";
        std::string active_key = exchange_name + ":" + market_type + ":active";

        // Load all symbols if requested
        std::string all_basename =
            "symbols_cache_" + exchange_name + "_" + market_type + "_all.json";
        std::string all_filename =
            CommonUtils::CachePathUtils::get_symbols_cache_path(all_basename);

        CommonUtils::CachePathUtils::migrate_cache_file_if_needed(all_basename,
                                                                   all_filename);

        std::ifstream all_file(all_filename);
        if (all_file.is_open()) {
            nlohmann::json all_cache_data;
            all_file >> all_cache_data;
            all_file.close();

            if (all_cache_data.contains("symbols")) {
                symbols_cache_[all_key] =
                    all_cache_data["symbols"].get<std::vector<std::string>>();
                symbols_cache_timestamps_[all_key] =
                    std::chrono::steady_clock::now();
            }
        }

        std::string active_basename = "symbols_cache_" + exchange_name + "_" +
                                      market_type + "_active.json";
        std::string active_filename =
            CommonUtils::CachePathUtils::get_symbols_cache_path(active_basename);

        CommonUtils::CachePathUtils::migrate_cache_file_if_needed(active_basename,
                                                                   active_filename);

        std::ifstream active_file(active_filename);
        if (active_file.is_open()) {
            nlohmann::json active_cache_data;
            active_file >> active_cache_data;
            active_file.close();

            if (active_cache_data.contains("symbols")) {
                symbols_cache_[active_key] =
                    active_cache_data["symbols"]
                        .get<std::vector<std::string>>();
                symbols_cache_timestamps_[active_key] =
                    std::chrono::steady_clock::now();
            }
        }

        LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_EXCHANGE_INFO_PRELOADED,
                 "Loaded symbols from disk cache for " + exchange_name + " " +
                     market_type,
                 nlohmann::json{});
    } catch (...) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_EXCHANGE_INFO_LOAD_FAILED,
                  "Failed to load symbols from disk for " + exchange_name +
                      " " + market_type,
                  nlohmann::json{});
    }
}

std::vector<Core::SymbolExchangeInfo> ExchangeInfo::load_symbol_info_from_disk(
    const std::string& exchange_name, const std::string& market_type) {
    try {
        std::string filename_basename =
            "symbols_info_cache_" + exchange_name + "_" + market_type + ".json";
        std::string filename =
            CommonUtils::CachePathUtils::get_symbols_cache_path(filename_basename);

        CommonUtils::CachePathUtils::migrate_cache_file_if_needed(filename_basename,
                                                                   filename);

        std::ifstream file(filename);
        std::vector<Core::SymbolExchangeInfo> infos;
        if (file.is_open()) {
            nlohmann::json cache_data;
            file >> cache_data;
            file.close();

            if (cache_data.contains("symbols_info")) {
                const auto& symbols_info_array = cache_data["symbols_info"];

                for (const auto& item : symbols_info_array) {
                    infos.push_back(Core::SymbolExchangeInfo::from_json(item));
                }
            }
        }

        LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_EXCHANGE_INFO_PRELOADED,
                 "Loaded symbol info from disk cache for " + exchange_name +
                     " " + market_type,
                 nlohmann::json{});
        return infos;
    } catch (...) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_EXCHANGE_INFO_LOAD_FAILED,
                  "Failed to load symbol info from disk for " + exchange_name +
                      " " + market_type,
                  nlohmann::json{});
        return {};
    }
}
}  // namespace Core
