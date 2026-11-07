#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace Live {

struct AccessAndSecretPair {
    std::string access;
    std::string secret;
};

class Config {
   public:
    std::string db_connection_string;
    std::unordered_map<long long, Live::AccessAndSecretPair>
        binance_futures_accounts;
    std::unordered_map<long long, Live::AccessAndSecretPair>
        binance_spot_accounts;
    std::unordered_map<long long, Live::AccessAndSecretPair>
        hyperliquid_futures_accounts;

    std::unordered_map<long long, Live::AccessAndSecretPair>
        bybit_futures_accounts;
    std::unordered_map<long long, Live::AccessAndSecretPair>
        bybit_spot_accounts;

    std::unordered_map<long long, Live::AccessAndSecretPair>
        bitget_futures_accounts;
    std::unordered_map<long long, Live::AccessAndSecretPair>
        bitget_spot_accounts;

    std::unordered_map<long long, Live::AccessAndSecretPair>
        gateio_spot_accounts;

    std::unordered_map<long long, Live::AccessAndSecretPair>
        polymarket_accounts;

    std::unordered_map<long long, Live::AccessAndSecretPair>
        aster_futures_accounts;

    std::unordered_map<long long, Live::AccessAndSecretPair>
        ibkr_spot_accounts;

    static Config& getInstance() {
        static Config instance;
        if (instance.config_unread) {
            instance.read_data_dir_root();
            instance.read_db_connection_string();
            instance.read_accounts();
            instance.config_unread = false;
        }
        return instance;
    }
    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;

    const char* get_db_connection_string() {
        return db_connection_string.c_str();
    }

    const std::string& get_data_dir_root() const { return data_dir_root; }
    std::string get_polymarket_series_base_path() const {
        return data_dir_root + "/prediction_markets/polymarket/series/";
    }

   private:
    std::string data_dir_root;
    bool config_unread = true;
    void read_db_connection_string();
    void read_accounts();
    void read_data_dir_root();
    Config();
    ~Config();
};
}  // namespace Live