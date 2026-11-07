#include "market_data/config.h"

#include <stdexcept>
namespace MarketData {

static bool env_bool(const char* name, bool default_value) {
    const char* v = std::getenv(name);
    if (v == nullptr) return default_value;
    return std::string(v) == "1";
}

Config::Config() {
    const char* env = std::getenv("DATA_DIR_ROOT");
    if (!env) {
        throw std::invalid_argument("DATA_DIR_ROOT is not set");
    }

    is_state_machines_enabled = env_bool("MD_ENABLE_STATE_MACHINES", true);

    if (const char* key = std::getenv("POLYGON_API_KEY")) polygon_api_key = key;
    if (const char* path = std::getenv("MARKET_DATA_COVERAGE_PATH"))
        coverage_path = path;

    data_dir_root = env;
}

Config& Config::getInstance() {
    static Config instance;
    return instance;
}

const std::string Config::get_data_dir_root() const { return data_dir_root; }

const std::string Config::get_raw_binance_swap_trades_dir() const {
    return data_dir_root + "/raw/trades/binance/swap";
}

const std::string Config::get_raw_binance_spot_trades_dir() const {
    return data_dir_root + "/raw/trades/binance/spot";
}

const std::string Config::get_raw_gateio_spot_trades_dir() const {
    return data_dir_root + "/raw/trades/gateio/spot";
}

const std::string Config::get_raw_hyperliquid_swap_trades_dir() const {
    return data_dir_root + "/raw/trades/hyperliquid/swap";
}

const std::string Config::get_raw_polygon_us_stock_trades_dir() const {
    return data_dir_root + "/raw/trades/polygon/stock";
}

const std::string Config::get_polygon_api_key() const {
    return polygon_api_key;
}

const std::string Config::get_coverage_path() const {
    if (coverage_path.empty()) {
        throw std::invalid_argument("MARKET_DATA_COVERAGE_PATH is not set");
    }
    return coverage_path;
}

bool Config::get_is_state_machines_enabled() const {
    return is_state_machines_enabled;
}

}  // namespace MarketData
