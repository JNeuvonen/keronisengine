#pragma once

#include <chrono>
#include <functional>
#include <unordered_map>
#include <vector>

#include "market_making/dtypes/def_only_structs.h"

namespace MarketData {

class BarsLivestreamer {
   public:
    BarsLivestreamer(
        const std::function<void(const std::string&)>& error_log_callback,
        const std::function<void(const std::string&)>& info_log_callback,
        const std::chrono::system_clock::time_point data_start_date);
    ~BarsLivestreamer() = default;
    void start_stream();

   private:
    void init_state();
    void disconnect_streams();
    bool is_initialized = false;

    std::unordered_map<std::string, std::vector<std::string>>
        exchange_and_market_to_unique_symbols;
    std::unordered_map<std::string, Core::ExchangeAndMarket>
        exchange_and_market_util;

    std::function<void(const std::string&)> error_log_callback;
    std::function<void(const std::string&)> info_log_callback;
    std::chrono::system_clock::time_point data_start_date;
};

}  // namespace MarketData