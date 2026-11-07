#include <binance_utils.h>
#include <http_reqs.h>

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

std::pair<std::string, std::string> BinaUtils::split_symbol(
    const std::string &symbol) {
    std::vector<std::string> common_quote_assets = {"USDT", "BTC", "ETH",
                                                    "USDC", "BNB"};

    for (const auto &quote : common_quote_assets) {
        if (symbol.size() >= quote.size() &&
            symbol.substr(symbol.size() - quote.size()) == quote) {
            std::string base = symbol.substr(0, symbol.size() - quote.size());
            return {base, quote};
        }
    }

    return {symbol, ""};
}

std::string BinaUtils::convert_interval_ms_to_str(int interval_ms) {
    switch (interval_ms) {
        case 60 * 1000:  // 1 minute
            return "1m";
        case 3 * 60 * 1000:  // 3 minutes
            return "3m";
        case 5 * 60 * 1000:  // 5 minutes
            return "5m";
        case 15 * 60 * 1000:  // 15 minutes
            return "15m";
        case 30 * 60 * 1000:  // 30 minutes
            return "30m";
        case 60 * 60 * 1000:  // 1 hour
            return "1h";
        case 2 * 60 * 60 * 1000:  // 2 hours
            return "2h";
        case 4 * 60 * 60 * 1000:  // 4 hours
            return "4h";
        case 6 * 60 * 60 * 1000:  // 6 hours
            return "6h";
        case 8 * 60 * 60 * 1000:  // 8 hours
            return "8h";
        case 12 * 60 * 60 * 1000:  // 12 hours
            return "12h";
        case 24 * 60 * 60 * 1000:  // 1 day
            return "1d";
        default:
            throw std::invalid_argument("Invalid interval_ms provided");
    }
}

std::string used_weight_1m;
std::string used_weight;

size_t BinaUtils::UsedRequestWeightCallback(char *buffer, size_t size,
                                            size_t nitems, void *userdata) {
    size_t total_size = size * nitems;
    std::unordered_map<std::string, std::string> *headers =
        static_cast<std::unordered_map<std::string, std::string> *>(userdata);

    std::string header(buffer, total_size);

    size_t colon_pos = header.find(":");
    if (colon_pos != std::string::npos) {
        std::string key = header.substr(0, colon_pos);
        std::string value = header.substr(colon_pos + 2);

        value.erase(std::remove(value.begin(), value.end(), '\n'), value.end());
        value.erase(std::remove(value.begin(), value.end(), '\r'), value.end());

        (*headers)[key] = value;
    }

    return total_size;
}

long BinaUtils::convert_interval_str_to_ms(const std::string &interval) {
    static const std::map<std::string, long> interval_map = {
        {"1s", 1000},
        {"1m", 60 * 1000},
        {"3m", 3 * 60 * 1000},
        {"5m", 5 * 60 * 1000},
        {"15m", 15 * 60 * 1000},
        {"30m", 30 * 60 * 1000},
        {"1h", 60 * 60 * 1000},
        {"2h", 2 * 60 * 60 * 1000},
        {"4h", 4 * 60 * 60 * 1000},
        {"6h", 6 * 60 * 60 * 1000},
        {"8h", 8 * 60 * 60 * 1000},
        {"12h", 12 * 60 * 60 * 1000},
        {"1d", 24 * 60 * 60 * 1000},
    };

    auto it = interval_map.find(interval);
    return (it != interval_map.end()) ? it->second : -1;
}

std::pair<MarketData::OhlcvDatasetTick, std::pair<std::string, long>>
BinaUtils::parse_binance_kline(const nlohmann::json &json_data) {
    MarketData::OhlcvDatasetTick kline_data;
    std::string symbol;
    std::string interval_str;
    long interval_ms = -1;

    try {
        if (!json_data.contains("k")) {
            throw std::runtime_error("Invalid Kline message format");
        }

        auto k = json_data["k"];  // Extract "k" Kline object

        kline_data.kline_open_time = k["t"].get<long long>();
        kline_data.open_price = std::stod(k["o"].get<std::string>());
        kline_data.high_price = std::stod(k["h"].get<std::string>());
        kline_data.low_price = std::stod(k["l"].get<std::string>());
        kline_data.close_price = std::stod(k["c"].get<std::string>());
        kline_data.volume = std::stod(k["v"].get<std::string>());
        kline_data.kline_close_time = k["T"].get<long long>();
        kline_data.quote_asset_volume = std::stod(k["q"].get<std::string>());
        kline_data.number_of_trades = k["n"].get<long>();
        kline_data.taker_buy_base_volume = std::stod(k["V"].get<std::string>());
        kline_data.taker_buy_quote_volume =
            std::stod(k["Q"].get<std::string>());

        symbol = json_data["s"].get<std::string>();
        interval_str = k["i"].get<std::string>();
        interval_ms = BinaUtils::convert_interval_str_to_ms(interval_str);

    } catch (const std::exception &e) {
        std::cerr << "Error parsing Binance Kline: " << e.what() << std::endl;
    }

    return {kline_data, {symbol, interval_ms}};
}

BinanceOrderFill BinaUtils::create_order_fill_from_spot_json(
    const nlohmann::json &json) {
    BinanceOrderFill fill;
    fill.status = json["X"].get<std::string>();
    fill.clientOrderId = json["c"].get<std::string>();
    fill.orig_qty = json["q"].get<std::string>();
    fill.exec_qty = json["z"].get<std::string>();
    fill.commission = std::stod(json["n"].get<std::string>());
    return fill;
}