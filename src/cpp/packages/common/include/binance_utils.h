#pragma once
#include <curl/curl.h>
#include <simdjson.h>

#include <iostream>

#include "common_types/trade_types.h"
#include "ohlcv_dataset_tick.h"

namespace BinaUtils {

struct AggTrade {
    std::string symbol;
    double price;
    double quantity;
    long long trade_time;
    bool is_buyer_maker;
};

constexpr int INTERVAL_1M = 60 * 1000;          // 1 minute
constexpr int INTERVAL_3M = 3 * INTERVAL_1M;    // 3 minutes
constexpr int INTERVAL_5M = 5 * INTERVAL_1M;    // 5 minutes
constexpr int INTERVAL_15M = 15 * INTERVAL_1M;  // 15 minutes
constexpr int INTERVAL_30M = 30 * INTERVAL_1M;  // 30 minutes
constexpr int INTERVAL_1H = 60 * INTERVAL_1M;   // 1 hour
constexpr int INTERVAL_2H = 2 * INTERVAL_1H;    // 2 hours
constexpr int INTERVAL_4H = 4 * INTERVAL_1H;    // 4 hours
constexpr int INTERVAL_6H = 6 * INTERVAL_1H;    // 6 hours
constexpr int INTERVAL_8H = 8 * INTERVAL_1H;    // 8 hours
constexpr int INTERVAL_12H = 12 * INTERVAL_1H;  // 12 hours
constexpr int INTERVAL_1D = 24 * INTERVAL_1H;   // 1 day
//

constexpr const char *ORDER_STATUS_NEW = "NEW";
constexpr const char *ORDER_STATUS_PARTIALLY_FILLED = "PARTIALLY_FILLED";
constexpr const char *ORDER_STATUS_FILLED = "FILLED";
constexpr const char *ORDER_STATUS_CANCELED = "CANCELED";
constexpr const char *ORDER_STATUS_EXPIRED = "EXPIRED";
constexpr const char *ORDER_STATUS_EXPIRED_IN_MATCH = "EXPIRED_IN_MATCH";
constexpr const char *ORDER_STATUS_PENDING_NEW = "PENDING_NEW";
constexpr const char *ORDER_STATUS_PENDING_CANCEL = "PENDING_CANCEL";
constexpr const char *ORDER_STATUS_REJECTED = "REJECTED";

constexpr const char *TIME_IN_FORCE_GTC = "GTC";  // Good Till Canceled
constexpr const char *TIME_IN_FORCE_IOC = "IOC";  // Immediate Or Cancel
constexpr const char *TIME_IN_FORCE_FOK = "FOK";  // Fill Or Kill
constexpr const char *TIME_IN_FORCE_GTX = "GTX";  // Good Till Crossing
constexpr const char *TIME_IN_FORCE_GTD = "GTD";  // Good Till Date

constexpr const char *ORDER_TYPE_MARKET = "MARKET";
constexpr const char *ORDER_TYPE_LIMIT = "LIMIT";
constexpr const char *ORDER_TYPE_STOP_LIMIT = "STOP_LIMIT";
constexpr const char *ORDER_TYPE_STOP_MARKET = "STOP_MARKET";
constexpr const char *ORDER_TYPE_TRAILING_STOP = "TRAILING_STOP";
constexpr const char *ORDER_TYPE_OCO = "OCO";
constexpr const char *ORDER_TYPE_LIMIT_MAKER = "LIMIT_MAKER";
constexpr const char *ORDER_TYPE_ICEBERG = "ICEBERG";

constexpr const char *TRADE_SIDE_BUY = "BUY";
constexpr const char *TRADE_SIDE_SELL = "SELL";

constexpr const char *WEBSOCKET_METHOD_ORDER_PLACE = "order.place";

std::pair<std::string, std::string> split_symbol(const std::string &symbol);
std::string convert_interval_ms_to_str(int interval_ms);
long convert_interval_str_to_ms(const std::string &interval);

bool is_order_on_working_state(BinanceOrderFill &order);

std::pair<MarketData::OhlcvDatasetTick, std::pair<std::string, long>>
parse_binance_kline(const nlohmann::json &json_data);

size_t UsedRequestWeightCallback(char *buffer, size_t size, size_t nitems,
                                 void *userdata);

inline long long get_latest_fully_completed_kline_open_ms(
    long long interval_ms) {
    using namespace std::chrono;
    auto now_ms =
        duration_cast<milliseconds>(system_clock::now().time_since_epoch())
            .count();
    return now_ms - (now_ms % interval_ms) -
           (now_ms % interval_ms == 0 ? interval_ms : 0) - interval_ms;
}

inline bool is_order_on_working_state(const BinanceOrderFill &order) {
    return order.status == "NEW" || order.status == "PARTIALLY_FILLED";
}

inline bool isKlineClosedFastSearch(const std::string &json) {
    size_t start = json.find("\"x\":");
    if (start != std::string::npos) {
        return json[start + 4] == 't';
    }
    return false;
}

static simdjson::dom::parser parser;

inline std::optional<AggTrade> parse_aggtrade(const std::string &msg) {
    try {
        simdjson::dom::element doc = parser.parse(msg);
        auto data = doc["data"];

        AggTrade trade;
        trade.symbol = std::string(data["s"].get_string().value());
        std::string_view price_str = data["p"].get_string().value();
        std::string_view qty_str = data["q"].get_string().value();

        trade.price = std::stod(std::string(price_str));
        trade.quantity = std::stod(std::string(qty_str));
        trade.trade_time = data["T"].get_int64();
        trade.is_buyer_maker = data["m"].get_bool();

        return trade;
    } catch (const simdjson::simdjson_error &e) {
        std::cerr << "simdjson error: " << e.what() << std::endl;
        return std::nullopt;
    }
}

BinanceOrderFill create_order_fill_from_spot_json(const nlohmann::json &json);

}  // namespace BinaUtils
