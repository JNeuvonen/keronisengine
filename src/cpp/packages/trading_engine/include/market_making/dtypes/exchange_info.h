#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "market_making/dtypes/def_only_structs.h"

namespace Core {

class ExchangeInfo {
   public:
    static ExchangeInfo& getInstance();

    std::vector<std::string> get_symbols(const std::string& exchange_name,
                                         const std::string& market_type,
                                         bool active_only);

    std::vector<std::string> get_active_symbols(
        const std::string& exchange_name, const std::string& market_type);

    std::vector<Core::SymbolExchangeInfo> get_exchange_symbols_info(
        const std::string& exchange_name, const std::string& market_type);

    bool is_symbol_active(const std::string& symbol,
                          const std::string& exchange_name,
                          const std::string& market_type);
    bool is_symbol_allowing_margin_trade(const std::string& symbol,
                                         const std::string& exchange_name,
                                         const std::string& market_type);

    void update_symbols_for_exchange_and_market_type(
        const std::string& exchange_name, const std::string& market_type);

   private:
    ExchangeInfo() = default;
    ExchangeInfo(const ExchangeInfo&) = delete;
    ExchangeInfo& operator=(const ExchangeInfo&) = delete;
    std::mutex exchange_info_mutex_;

    std::unordered_map<std::string, std::vector<std::string>> symbols_cache_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point>
        symbols_cache_timestamps_;

    std::unordered_map<std::string, bool> allows_margin_symbols_cache_;
    std::unordered_map<std::string, bool> active_symbols_cache_;

    std::vector<std::string> get_all_symbols_internal(
        const std::string& exchange_name, const std::string& market_type);

    void sync_symbols_to_disk(const std::string& exchange_name,
                              const std::string& market_type);

    void sync_symbol_info_to_disk(
        const std::string& exchange_name, const std::string& market_type,
        const std::vector<Core::SymbolExchangeInfo>& infos);

    void load_symbols_from_disk(const std::string& exchange_name,
                                const std::string& market_type);

    std::vector<Core::SymbolExchangeInfo> load_symbol_info_from_disk(
        const std::string& exchange_name, const std::string& market_type);

    void update_symbols_lookup_cache(
        const std::string& exchange_name, const std::string& market_type,
        const std::vector<Core::SymbolExchangeInfo>& infos);
};

}  // namespace Core