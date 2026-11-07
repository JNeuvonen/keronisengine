#pragma once
#include <string>

namespace MarketData {

class Config {
   public:
    static Config& getInstance();
    [[nodiscard]] const std::string get_data_dir_root() const;
    [[nodiscard]] const std::string get_raw_binance_swap_trades_dir() const;
    [[nodiscard]] const std::string get_raw_binance_spot_trades_dir() const;
    [[nodiscard]] const std::string get_raw_gateio_spot_trades_dir() const;
    [[nodiscard]] const std::string get_raw_hyperliquid_swap_trades_dir() const;
    [[nodiscard]] const std::string get_raw_polygon_us_stock_trades_dir() const;
    [[nodiscard]] const std::string get_polygon_api_key() const;
    // MARKET_DATA_COVERAGE_PATH; throws when unset, since a process that
    // streams market data has no safe default coverage.
    [[nodiscard]] const std::string get_coverage_path() const;
    [[nodiscard]] bool get_is_state_machines_enabled() const;

   private:
    Config();

    std::string data_dir_root;
    std::string polygon_api_key;
    std::string coverage_path;
    bool is_state_machines_enabled = true;
};

}  // namespace MarketData
