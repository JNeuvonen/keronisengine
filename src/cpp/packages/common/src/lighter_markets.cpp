#include "lighter_markets.h"

#include <cmath>

#include <nlohmann/json.hpp>

#include "curl_multi_manager.h"
#include "env_utils.h"
#include "str_math.h"

namespace LighterDirect {

namespace {

double number_field(const nlohmann::json& item, const char* key,
                    double fallback) {
    if (!item.contains(key) || item[key].is_null()) return fallback;
    const auto& value = item[key];
    if (value.is_string()) return std::stod(value.get<std::string>());
    return value.get<double>();
}

}  // namespace

LighterMarkets& LighterMarkets::instance() {
    static LighterMarkets markets;
    return markets;
}

std::string LighterMarkets::rest_base_url() {
    return EnvUtils::env_or("LIGHTER_API_BASE_URL",
                            "https://mainnet.zklighter.elliot.ai");
}

std::string LighterMarkets::public_ws_url() {
    return EnvUtils::env_or("LIGHTER_PUBLIC_WS_BASE_URL",
                            "wss://mainnet.zklighter.elliot.ai/stream?readonly=true");
}

std::vector<LighterMarket> LighterMarkets::parse_order_books(
    const std::string& body) {
    std::vector<LighterMarket> out;
    const auto json = nlohmann::json::parse(body, nullptr, false);
    if (json.is_discarded() || !json.contains("order_books") ||
        !json["order_books"].is_array()) {
        return out;
    }
    for (const auto& item : json["order_books"]) {
        if (item.value("market_type", "") != "perp") continue;
        LighterMarket market;
        market.venue_symbol = item.at("symbol").get<std::string>();
        market.symbol = market.venue_symbol + "USDC";
        market.market_id = item.at("market_id").get<long long>();
        market.size_decimals = item.value("supported_size_decimals", 0);
        market.price_decimals = item.value("supported_price_decimals", 0);
        market.min_base_amount = number_field(item, "min_base_amount", 0);
        market.min_quote_amount = number_field(item, "min_quote_amount", 0);
        market.is_active = item.value("status", "") == "active" &&
                           !item.value("is_frozen", false);
        out.push_back(std::move(market));
    }
    return out;
}

Core::SymbolExchangeInfo LighterMarkets::to_symbol_info(
    const LighterMarket& market) {
    Core::SymbolExchangeInfo info;
    info.symbol = market.symbol;
    info.base = market.venue_symbol;
    info.quote = "USDC";
    info.type = "swap";
    info.is_active = market.is_active;
    info.limits.amount.min = market.min_base_amount;
    info.limits.amount.max = 1e12;
    info.limits.price.min = 0;
    info.limits.price.max = 1e12;
    info.limits.cost.min = market.min_quote_amount;
    info.limits.cost.max = 1e12;
    info.precision.price = std::pow(10.0, -market.price_decimals);
    info.precision.amount = std::pow(10.0, -market.size_decimals);
    info.precision.base = 0;
    info.precision.quote = 0;
    info.precision_int.price = StrMath::step_size_to_precision(info.precision.price);
    info.precision_int.amount =
        StrMath::step_size_to_precision(info.precision.amount);
    return info;
}

bool LighterMarkets::refresh() {
    HttpRequest req;
    req.url = rest_base_url() + "/api/v1/orderBooks";
    req.method = "GET";
    req.timeout_ms = 10000;
    const auto [status_code, body] = CurlMultiManager::instance().execute(req);
    if (status_code != 200) return false;
    auto markets = parse_order_books(body);
    if (markets.empty()) return false;
    set_markets(std::move(markets));
    return true;
}

bool LighterMarkets::ensure_loaded() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!markets_.empty()) return true;
    }
    return refresh();
}

void LighterMarkets::set_markets(std::vector<LighterMarket> markets) {
    std::lock_guard<std::mutex> lock(mutex_);
    markets_ = std::move(markets);
}

std::optional<LighterMarket> LighterMarkets::by_symbol(const std::string& symbol) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& market : markets_) {
        if (market.symbol == symbol) return market;
    }
    return std::nullopt;
}

std::optional<LighterMarket> LighterMarkets::by_market_id(long long market_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& market : markets_) {
        if (market.market_id == market_id) return market;
    }
    return std::nullopt;
}

std::vector<LighterMarket> LighterMarkets::all() {
    std::lock_guard<std::mutex> lock(mutex_);
    return markets_;
}

}  // namespace LighterDirect
