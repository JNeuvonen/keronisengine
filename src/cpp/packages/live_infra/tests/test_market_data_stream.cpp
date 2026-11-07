#include <catch2/catch_all.hpp>

#include "generic_utils.h"
#include "live_infra/config.h"
#include "trading/account.h"
#include "ws_stream.h"

void message_handler(const std::string& message) {
    std::cout << "Received message: " << message << std::endl;
}

std::string create_kline_subscribe_message(std::vector<std::string> symbols,
                                           const std::string& interval = "1m") {
    std::string subscribe_message = "{\"method\": \"SUBSCRIBE\", \"params\": [";
    bool first = true;

    for (auto& symbol : symbols) {
        CommonUtils::to_lowercase(symbol);
        if (!first) {
            subscribe_message += ", ";
        }
        // Format: <symbol>@kline_<interval>
        subscribe_message += "\"" + symbol + "@kline_" + interval + "\"";
        first = false;
    }

    subscribe_message += "], \"id\": 1}";
    return subscribe_message;
}

TEST_CASE("MarketDataStream", "[market_data]") {
    const auto& test_key = std::getenv("TESTNET_BINANCE_FUTURES_API_KEY");
    const auto& test_secret = std::getenv("TESTNET_BINANCE_FUTURES_API_SECRET");

    if (test_key == nullptr || test_secret == nullptr) {
        throw std::runtime_error(
            "TESTNET_BINANCE_FUTURES_API_KEY and "
            "TESTNET_BINANCE_FUTURES_API_SECRET must be set");
    }

    Trading::Account account(Trading::MarketType::SWAP_MARKET_TYPE,
                             Trading::ExchangeType::BINANCE_ACCOUNT, 1,
                             test_key, test_secret, message_handler,
                             message_handler);

    const auto& active_symbols = account.get_active_symbols();

    const auto& ws_url = "wss://fstream.binance.com/ws";
    WsStream stream(
        ws_url,
        [](const std::string& message) {
            std::cout << "Received message: " << message << std::endl;
        },
        "",
        [](const std::string& message) {
            std::cout << "Info: " << message << std::endl;
        },
        [](const std::string& message) {
            std::cout << "Error: " << message << std::endl;
        });

    // For debugging, print the URL
    std::cout << "Connecting to: " << ws_url << std::endl;
    stream.connect();
    std::this_thread::sleep_for(std::chrono::seconds(3));
    const auto& sub_message = create_kline_subscribe_message(active_symbols);
    stream.send_message(sub_message);
    std::this_thread::sleep_for(std::chrono::seconds(60));
    stream.disconnect();
}

TEST_CASE("MarketDataStream", "[market_data_ticker_arr]") {
    const auto& test_key = std::getenv("TESTNET_BINANCE_FUTURES_API_KEY");
    const auto& test_secret = std::getenv("TESTNET_BINANCE_FUTURES_API_SECRET");

    if (test_key == nullptr || test_secret == nullptr) {
        throw std::runtime_error(
            "TESTNET_BINANCE_FUTURES_API_KEY and "
            "TESTNET_BINANCE_FUTURES_API_SECRET must be set");
    }

    Trading::Account account(Trading::MarketType::SWAP_MARKET_TYPE,
                             Trading::ExchangeType::BINANCE_ACCOUNT, 1,
                             test_key, test_secret, message_handler,
                             message_handler);

    const auto& ws_url = "wss://fstream.binance.com/ws/!ticker@arr";
    WsStream stream(
        ws_url,
        [](const std::string& message) {
            std::cout << "Received message: " << message << std::endl;
        },
        "",
        [](const std::string& message) {
            std::cout << "Info: " << message << std::endl;
        },
        [](const std::string& message) {
            std::cout << "Error: " << message << std::endl;
        });

    // For debugging, print the URL
    std::cout << "Connecting to: " << ws_url << std::endl;
    stream.connect();
    std::this_thread::sleep_for(std::chrono::seconds(3));
    std::this_thread::sleep_for(std::chrono::seconds(60));
    stream.disconnect();
}

TEST_CASE("MarketDataStream", "[market_data_ohlcv_arr]") {
    const auto& test_key = std::getenv("TESTNET_BINANCE_FUTURES_API_KEY");
    const auto& test_secret = std::getenv("TESTNET_BINANCE_FUTURES_API_SECRET");

    if (test_key == nullptr || test_secret == nullptr) {
        throw std::runtime_error(
            "TESTNET_BINANCE_FUTURES_API_KEY and "
            "TESTNET_BINANCE_FUTURES_API_SECRET must be set");
    }

    Trading::Account account(Trading::MarketType::SPOT_MARKET_TYPE,
                             Trading::ExchangeType::BINANCE_ACCOUNT, 1,
                             test_key, test_secret, message_handler,
                             message_handler);

    const auto& active_symbols = account.get_active_symbols();
    const auto& ws_url = account.get_ohlcv_stream_url(active_symbols, "1m");

    WsStream stream(
        ws_url,
        [](const std::string& message) {
            std::cout << "Received message: " << message << std::endl;
        },
        "",
        [](const std::string& message) {
            std::cout << "Info: " << message << std::endl;
        },
        [](const std::string& message) {
            std::cout << "Error: " << message << std::endl;
        });

    // For debugging, print the URL
    std::cout << "Connecting to: " << ws_url << std::endl;
    stream.connect();
    std::this_thread::sleep_for(std::chrono::seconds(60));
    stream.disconnect();
}