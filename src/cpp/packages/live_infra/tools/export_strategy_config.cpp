// Prints the `strategies` (and, when needed, `discretionary_accounts`) block
// of a backtest config.json for strategy rows in the DB, through the same
// loader live trading uses.
//
//   DB_CONNECTION_STR=<libpq conninfo or URI> export_strategy_config <id>...

#include <cstdlib>
#include <iostream>
#include <string>

#include "common_types/strategy.h"
#include "postgre_utils.h"

namespace {

std::string conninfo_from_env() {
    const char* value = std::getenv("DB_CONNECTION_STR");
    if (!value || !*value) {
        throw std::runtime_error("DB_CONNECTION_STR is not set");
    }
    std::string conninfo = value;
    if (const auto at = conninfo.find("+psycopg://"); at != std::string::npos) {
        conninfo.erase(at, std::string("+psycopg").size());
    }
    return conninfo;
}

nlohmann::json discretionary_accounts(const std::string& conninfo) {
    const auto rows = PQUtils::fetch_from_postgres(
        conninfo.c_str(),
        "SELECT d.*, a.account_type FROM discretionary_account d JOIN account "
        "a USING(account_id) ORDER BY d.account_id;");
    nlohmann::json accounts = nlohmann::json::array();
    for (const auto& row : rows) {
        accounts.push_back({
            {"account_id", std::stoll(row.at("account_id").get<std::string>())},
            {"paper", row.at("account_type") == "paper"},
            {"enabled", row.at("enabled") == "t"},
            {"max_directional_exposure",
             std::stod(row.at("max_directional_exposure").get<std::string>())},
            {"max_order_notional",
             std::stod(row.at("max_order_notional").get<std::string>())},
            {"num_max_orders_10_sec",
             std::stoi(row.at("num_max_orders_10_sec").get<std::string>())},
            {"num_max_orders_1_min",
             std::stoi(row.at("num_max_orders_1_min").get<std::string>())},
            {"num_max_orders_1_hour",
             std::stoi(row.at("num_max_orders_1_hour").get<std::string>())},
        });
    }
    return accounts;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "usage: export_strategy_config <strategy_id>...\n";
        return 2;
    }
    try {
        const std::string conninfo = conninfo_from_env();
        nlohmann::json out = {{"strategies", nlohmann::json::array()}};
        bool needs_accounts = false;
        for (int i = 1; i < argc; ++i) {
            const int id = std::stoi(argv[i]);
            const auto configs =
                PQUtils::fetch_strategy_configs(conninfo.c_str(), "2.0", id);
            if (configs.size() != 1) {
                throw std::runtime_error("no version 2.0 strategy row with id " +
                                         std::to_string(id));
            }
            needs_accounts |=
                StrategyConfig::from_json(configs[0]).discretionary_multi_account;
            out["strategies"].push_back(configs[0]);
        }
        if (needs_accounts) {
            out["discretionary_accounts"] = discretionary_accounts(conninfo);
        }
        std::cout << out.dump(2) << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "export_strategy_config: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
