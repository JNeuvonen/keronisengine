#include "live_infra/config.h"

#include <fstream>
#include <nlohmann/json.hpp>

Live::Config::Config() {}

Live::Config::~Config() {}

void Live::Config::read_data_dir_root() {
    const char* env = std::getenv("DATA_DIR_ROOT");
    if (!env || env[0] == '\0') {
        throw std::runtime_error(
            "DATA_DIR_ROOT environment variable is not set");
    }
    data_dir_root = std::string(env);
}

void Live::Config::read_db_connection_string() {
    const char* host = std::getenv("DB_HOST");
    const char* port = std::getenv("DB_PORT");
    const char* dbname = std::getenv("DB_NAME");
    const char* user = std::getenv("DB_USER");
    const char* password = std::getenv("DB_PASSWORD");

    if (!host || !port || !dbname || !user || !password) {
        throw std::runtime_error(
            "Missing required environment variables for database connection.");
    }

    db_connection_string =
        "host=" + std::string(host) + " port=" + std::string(port) +
        " dbname=" + std::string(dbname) + " user=" + std::string(user) +
        " password=" + std::string(password);
}

void Live::Config::read_accounts() {
    const char* accounts_file = std::getenv("ACCOUNTS_FILE");
    if (!accounts_file) {
        throw std::runtime_error("ACCOUNTS_FILE is not set");
    }

    std::ifstream file(accounts_file);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open accounts file");
    }

    nlohmann::json accounts_json;
    file >> accounts_json;

    auto parse_accounts = [&](const std::string& key, auto& target) {
        if (!accounts_json.contains(key)) return;
        for (const auto& account : accounts_json[key]) {
            target[account["account_id"]] = {account.value("access", ""),
                                             account.value("secret", "")};
        }
    };

    parse_accounts("binance_futures_accounts", binance_futures_accounts);
    parse_accounts("binance_spot_accounts", binance_spot_accounts);
    parse_accounts("hyperliquid_futures_accounts", hyperliquid_futures_accounts);
    parse_accounts("bybit_futures_accounts", bybit_futures_accounts);
    parse_accounts("bybit_spot_accounts", bybit_spot_accounts);
    parse_accounts("bitget_futures_accounts", bitget_futures_accounts);
    parse_accounts("bitget_spot_accounts", bitget_spot_accounts);
    parse_accounts("gateio_spot_accounts", gateio_spot_accounts);
    parse_accounts("polymarket_accounts", polymarket_accounts);
    parse_accounts("aster_futures_accounts", aster_futures_accounts);
    parse_accounts("ibkr_spot_accounts", ibkr_spot_accounts);
}
