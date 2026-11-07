#include <iostream>

#include "catch2/catch_all.hpp"
#include "market_making/utils.h"

TEST_CASE("Test get_symbol_all_trade_open_quantity_combinations",
          "[test_get_symbol_all_trade_open_quantity_combinations]") {
    std::vector<Core::Trade> trades;

    Core::Trade trade1;
    trade1.id = 1;
    trade1.symbol = "BTCUSDT";
    trade1.exchange_name = "binance";
    trade1.market_type = "spot";
    trade1.account_id = 1;
    trade1.open_quantity = 1;
    trade1.closed_quantity = 0;

    Core::Trade trade2;
    trade2.id = 2;
    trade2.symbol = "BTCUSDT";
    trade2.exchange_name = "binance";
    trade2.market_type = "spot";
    trade2.account_id = 1;
    trade2.open_quantity = 2;
    trade2.closed_quantity = 0;

    Core::Trade trade3;
    trade3.id = 3;
    trade3.symbol = "BTCUSDT";
    trade3.exchange_name = "binance";
    trade3.market_type = "spot";
    trade3.account_id = 1;
    trade3.open_quantity = 3;
    trade3.closed_quantity = 2;

    trades.push_back(trade1);
    trades.push_back(trade2);
    trades.push_back(trade3);

    auto combinations =
        Core::Utils::get_symbol_all_trade_open_quantity_combinations(
            trades, "BTCUSDT", "binance", "spot", 1);

    std::cout << "Combinations: " << combinations.size() << std::endl;

    for (const auto& combination : combinations) {
        std::cout << "Combination: " << combination.first << std::endl;
        for (const auto& trade : combination.second) {
            std::cout << "Trade: " << trade.id << std::endl;
        }
    }
}
