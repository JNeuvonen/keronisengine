#include "ccxt_proxy.h"

#include <curl/curl.h>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <nlohmann/json.hpp>
#include <thread>

#include "ccxt_utils.h"
#include "chrono_utils.h"
#include "common_types/log_event_types.h"
#include "crypto/secp256k1_signer.h"
#include "curl_multi_manager.h"
#include "disk_logging/log.h"
#include "market_data/utils.h"
#include "ohlcv_dataset_tick.h"
#include "rate_limiter.h"
#include "str_math.h"

CcxtProxy::CcxtProxy() {
    const char *base_env = std::getenv("CCXT_PROXY_BASE_URL");
    this->base_url =
        base_env ? std::string(base_env) : "http://localhost:8000/api/v1/ccxt";

    const char *stream_env = std::getenv("CCXT_PROXY_STREAM_URL");
    this->stream_instance_url = stream_env
                                    ? std::string(stream_env)
                                    : "http://localhost:8040/api/v1/ccxt";
}

CcxtProxy::~CcxtProxy() {}

// Read once, cache. Honored by every exchange-write path (create_order,
// cancel_order, cancel_order_v2). The existing TradeTicker-level gate
// only catches strategy-initiated orders; the engine and state_updater
// also create + cancel via direct CcxtProxy calls (e.g. nontracked
// inventory close in crypto_margin.cpp, misc-order watchdog in
// state_updater.cpp). Gating at this layer makes IS_ORDER_CREATION_DISABLED
// the single switch for "no exchange writes" — safe for local runs
// against real live_infra state.
static bool exchange_writes_disabled() {
    static const bool v = []() {
        const char *e = std::getenv("IS_ORDER_CREATION_DISABLED");
        bool disabled = (e != nullptr && std::string(e) == "1");
        if (disabled) {
            std::cerr << "[ccxt_proxy] IS_ORDER_CREATION_DISABLED=1 — "
                         "all create_order / cancel_order / cancel_order_v2 "
                         "calls will return without hitting the exchange"
                      << std::endl;
        }
        return disabled;
    }();
    return v;
}

std::optional<nlohmann::json> CcxtProxy::create_order(
    const Core::Order &order) {
    if (exchange_writes_disabled()) {
        return std::nullopt;
    }
    if (order.is_prediction_market_order) {
        if (polymarket_direct_enabled_ && polymarket_client_.has_value() &&
            polymarket_client_->has_account(order.account_id)) {
            return polymarket_client_->create_order(order);
        }

        // Proxy path: POST to Python proxy
        auto res = http_req(this->base_url + "/create-order", "POST",
                            order.to_ccxt_proxy_format().dump());
        if (res.first >= 200 && res.first < 300 && !res.second.empty()) {
            try {
                return nlohmann::json::parse(res.second);
            } catch (...) {
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    // Aster direct path
    if (order.exchange_name == "aster" && aster_direct_enabled_ &&
        aster_client_.has_value() &&
        aster_client_->has_account(order.account_id)) {
        return aster_client_->create_order(order);
    }

    // Hyperliquid direct path
    if (order.exchange_name == "hyperliquid" && hyperliquid_direct_enabled_ &&
        hyperliquid_client_.has_value() &&
        hyperliquid_client_->has_account(order.account_id)) {
        return hyperliquid_client_->create_order(order);
    }

    // Non-prediction-market orders go through Python proxy. Synchronous —
    // wait for the ccxt result so the engine can dispatch on success/failure.
    auto res = http_req(this->base_url + "/create-order", "POST",
                        order.to_ccxt_proxy_format().dump());
    if (res.first >= 200 && res.first < 300 && !res.second.empty()) {
        try {
            return nlohmann::json::parse(res.second);
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::vector<Core::PolymarketBulkOrderResult>
CcxtProxy::create_polymarket_orders_bulk(
    const std::vector<Core::Order> &orders) {
    if (polymarket_direct_enabled_ && polymarket_client_.has_value() &&
        !orders.empty() &&
        polymarket_client_->has_account(orders[0].account_id)) {
        return polymarket_client_->create_orders_bulk(orders);
    }

    if (orders.empty()) {
        return {};
    }

    // Proxy path: POST to Python proxy
    nlohmann::json body = nlohmann::json::array();
    for (const auto &order : orders) {
        body.push_back(order.to_ccxt_proxy_format());
    }

    auto res =
        http_req(this->base_url + "/create-orders-bulk", "POST", body.dump());

    std::vector<Core::PolymarketBulkOrderResult> results;
    if (res.first >= 200 && res.first < 300 && !res.second.empty()) {
        try {
            auto json = nlohmann::json::parse(res.second);
            if (json.is_array()) {
                for (const auto &item : json) {
                    Core::PolymarketBulkOrderResult r;
                    r.success = item.value("success", false);
                    r.order_id = item.value("orderID", "");
                    r.error_msg = item.value("errorMsg", "");
                    r.status = item.value("status", "");
                    r.taking_amount = item.value("takingAmount", "");
                    r.making_amount = item.value("makingAmount", "");
                    results.push_back(r);
                }
            }
        } catch (...) {
            LOG_ERROR(
                Core::TRADING_LOG_EVENT_TYPE_CCXT_PROXY_RESPONSE_PARSE_FAILED,
                "create_polymarket_orders_bulk proxy parse failed",
                nlohmann::json{});
        }
    }

    // Pad with failures if needed
    while (results.size() < orders.size()) {
        Core::PolymarketBulkOrderResult r;
        r.error_msg = "proxy request failed";
        results.push_back(r);
    }

    return results;
}

std::vector<PolymarketDirect::PolymarketClient::SignedOrderBody>
CcxtProxy::sign_polymarket_orders(const std::vector<Core::Order> &orders) {
    if (polymarket_client_.has_value() && !orders.empty() &&
        polymarket_client_->has_account(orders[0].account_id)) {
        return polymarket_client_->sign_orders(orders);
    }
    return {};
}

std::vector<Core::PolymarketBulkOrderResult>
CcxtProxy::submit_signed_polymarket_orders(
    const std::vector<PolymarketDirect::PolymarketClient::SignedOrderBody>
        &signed_orders,
    long long account_id) {
    if (polymarket_client_.has_value() && !signed_orders.empty() &&
        polymarket_client_->has_account(account_id)) {
        return polymarket_client_->submit_signed_orders(signed_orders,
                                                        account_id);
    }
    return std::vector<Core::PolymarketBulkOrderResult>(signed_orders.size());
}

Core::PolymarketBulkOrderResult CcxtProxy::submit_signed_polymarket_order(
    const PolymarketDirect::PolymarketClient::SignedOrderBody &signed_order,
    long long account_id) {
    if (polymarket_client_.has_value() &&
        polymarket_client_->has_account(account_id)) {
        return polymarket_client_->submit_signed_order(signed_order,
                                                       account_id);
    }
    Core::PolymarketBulkOrderResult result;
    result.error_msg = "polymarket client not available";
    return result;
}

bool CcxtProxy::send_polymarket_heartbeat(long long account_id) {
    if (polymarket_client_.has_value() &&
        polymarket_client_->has_account(account_id)) {
        return polymarket_client_->send_heartbeat(account_id);
    }
    return false;
}

bool CcxtProxy::repay_loan(const std::string &exchange_name,
                           const std::string &market_type,
                           const long long &account_id,
                           const std::string &symbol,
                           const std::string &amount) {
    auto res =
        http_req(this->base_url + "/repay-loan?exchange_name=" + exchange_name +
                     "&market_type=" + market_type +
                     "&account_id=" + std::to_string(account_id) +
                     "&symbol=" + symbol + "&amount=" + amount,
                 "POST", "");

    if (res.first > 299) {
        return false;
    }
    return true;
}

std::optional<nlohmann::json> CcxtProxy::cancel_order(const Core::Order &order) {
    if (exchange_writes_disabled()) {
        return std::nullopt;
    }
    // Synchronous — wait for the cancel round-trip so the caller can resolve
    // engine state (cancelled/gone → drop) without depending on the user-data
    // WS. Mirrors create_order.
    auto res = http_req(this->base_url + "/cancel-order", "POST",
                        order.to_ccxt_proxy_format().dump());
    if (res.first >= 200 && res.first < 300 && !res.second.empty()) {
        try {
            return nlohmann::json::parse(res.second);
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::vector<Core::PolymarketBulkCancelResult>
CcxtProxy::cancel_polymarket_orders_bulk(
    const std::vector<std::string> &order_ids, long long account_id) {
    if (polymarket_direct_enabled_ && polymarket_client_.has_value() &&
        !order_ids.empty() && polymarket_client_->has_account(account_id)) {
        return polymarket_client_->cancel_orders(order_ids, account_id);
    }

    if (order_ids.empty()) {
        return {};
    }

    // Proxy path: POST to Python proxy
    nlohmann::json body = order_ids;
    auto res = http_req(this->base_url + "/cancel-orders-bulk?account_id=" +
                            std::to_string(account_id),
                        "POST", body.dump());

    std::vector<Core::PolymarketBulkCancelResult> results;
    if (res.first >= 200 && res.first < 300 && !res.second.empty()) {
        try {
            auto json = nlohmann::json::parse(res.second);
            if (json.is_array()) {
                for (const auto &item : json) {
                    Core::PolymarketBulkCancelResult r;
                    r.success = item.value("success", false);
                    r.order_id = item.value("orderID", "");
                    r.error_msg = item.value("errorMsg", "");

                    if (!r.success) {
                        std::cout << "Cancel failed: " << r.error_msg
                                  << " order_id=" << r.order_id << std::endl;
                    }

                    results.push_back(r);
                }
            }
        } catch (...) {
            LOG_ERROR(
                Core::TRADING_LOG_EVENT_TYPE_CCXT_PROXY_RESPONSE_PARSE_FAILED,
                "cancel_polymarket_orders_bulk proxy parse failed",
                nlohmann::json{});
        }
    }

    // Pad with failures if needed
    while (results.size() < order_ids.size()) {
        Core::PolymarketBulkCancelResult r;
        r.error_msg = "proxy request failed";
        results.push_back(r);
    }

    return results;
}

std::vector<std::string> CcxtProxy::build_proxy_auth_headers() const {
    const char *proxy_auth_token = std::getenv("PROXY_AUTH_TOKEN");
    return {"Authorization: Bearer " +
            std::string(proxy_auth_token ? proxy_auth_token : "")};
}

std::pair<int, std::string> CcxtProxy::http_req(
    const std::string &url, const std::string &method, const std::string &body,
    const std::vector<std::string> &headers) {
    HttpRequest req;
    req.url = url;
    req.method = method;
    req.body = body;
    req.timeout_ms = 120000;
    req.connect_timeout_ms = 5000;
    req.headers.push_back("Content-Type: application/json");
    for (const auto &header : headers) {
        req.headers.push_back(header);
    }
    return CurlMultiManager::instance().execute(std::move(req));
}

bool CcxtProxy::rate_blocked(const std::string &exchange_name,
                             const std::string &market_type) const {
    return RateLimiter::instance().is_limited(exchange_name + ":" +
                                              market_type);
}

void CcxtProxy::note_rate_limit(const std::string &exchange_name,
                                const std::string &market_type, int status,
                                const std::string &body) {
    if (status != 429 && status != 418) return;
    int retry_after = 60;
    try {
        auto j = nlohmann::json::parse(body);
        if (j.contains("retry_after_s"))
            retry_after = j["retry_after_s"].get<int>();
    } catch (...) {
    }
    RateLimiter::instance().record_limit(exchange_name + ":" + market_type,
                                         retry_after);
}

std::vector<Core::SymbolExchangeInfo> CcxtProxy::get_exchange_symbols_info(
    const std::string &exchange_name, const std::string &market_type) {
    if (rate_blocked(exchange_name, market_type)) return {};
    try {
        auto res = http_req(this->base_url + "/exchange-info?exchange_name=" +
                                exchange_name + "&market_type=" + market_type,
                            "GET", "");
        note_rate_limit(exchange_name, market_type, res.first, res.second);

        if (res.first > 299) {
            return {};
        }

        auto json = nlohmann::json::parse(res.second);
        std::vector<Core::SymbolExchangeInfo> infos;

        for (const auto &item : json) {
            Core::SymbolExchangeInfo info;
            info.symbol = item.at("symbol").get<std::string>();
            info.base = item.at("base").get<std::string>();
            info.quote = item.at("quote").get<std::string>();
            info.is_active = item.at("is_active").get<bool>();
            info.type = item.at("type").get<std::string>();

            if (item.at("limits").at("amount").at("min").is_null()) {
                info.limits.amount.min = 0;
            } else {
                info.limits.amount.min =
                    item.at("limits").at("amount").at("min").get<double>();
            }

            if (item.at("limits").at("amount").at("max").is_null()) {
                info.limits.amount.max = 1000000;
            } else {
                info.limits.amount.max =
                    item.at("limits").at("amount").at("max").get<double>();
            }

            if (item.at("limits").at("price").at("min").is_null()) {
                info.limits.price.min = -10;
            } else {
                info.limits.price.min =
                    item.at("limits").at("price").at("min").get<double>();
            }

            if (item.at("limits").at("price").at("max").is_null()) {
                info.limits.price.max = 100000000;
            } else {
                info.limits.price.max =
                    item.at("limits").at("price").at("max").get<double>();
            }

            if (item.at("limits").at("cost").at("min").is_null()) {
                info.limits.cost.min = 0;
            } else {
                info.limits.cost.min =
                    item.at("limits").at("cost").at("min").get<double>();
            }

            if (item.at("limits").at("cost").at("max").is_null()) {
                info.limits.cost.max = 1000000;
            } else {
                info.limits.cost.max =
                    item.at("limits").at("cost").at("max").get<double>();
            }

            info.precision.price =
                item.at("precision").at("price").get<double>();
            info.precision.amount =
                item.at("precision").at("amount").get<double>();

            if (item.at("precision").at("base").is_null()) {
                info.precision.base = 0;
            } else {
                info.precision.base =
                    item.at("precision").at("base").get<double>();
            }

            if (item.at("precision").at("quote").is_null()) {
                info.precision.quote = 0;
            } else {
                info.precision.quote =
                    item.at("precision").at("quote").get<double>();
            }

            info.precision_int.price =
                StrMath::step_size_to_precision(info.precision.price);
            info.precision_int.amount =
                StrMath::step_size_to_precision(info.precision.amount);

            infos.push_back(info);
        }
        return infos;
    } catch (...) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_CCXT_PROXY_RESPONSE_PARSE_FAILED,
                  "get_exchange_symbols_info failed", nlohmann::json{});
        return {};
    }
}

std::optional<
    std::pair<std::vector<Core::QuoteAsset>, std::vector<Core::Inventory>>>
CcxtProxy::get_inventory_state(const std::string &exchange_name,
                               const std::string &market_type,
                               const long long &account_id) {
    std::string rate_key = exchange_name + ":" + market_type;
    // Respect the reactive rate-limit block — pause inventory fetching while
    // the key is in cooldown. No pacing here; inventory is not on the budget.
    if (RateLimiter::instance().is_limited(rate_key)) return std::nullopt;
    try {
        auto res =
            http_req(this->base_url + "/get-inventory-state?exchange_name=" +
                         exchange_name + "&market_type=" + market_type +
                         "&account_id=" + std::to_string(account_id),
                     "GET", "");

        if (res.first == 429 || res.first == 418) {
            int retry_after = 60;
            try {
                auto j = nlohmann::json::parse(res.second);
                if (j.contains("retry_after_s"))
                    retry_after = j["retry_after_s"].get<int>();
            } catch (...) {
            }
            RateLimiter::instance().record_limit(rate_key, retry_after);
            return std::nullopt;
        }

        if (res.first > 299) {
            LOG_ERROR(
                Core::TRADING_LOG_EVENT_TYPE_CCXT_PROXY_REQUEST_FAILED,
                "get_inventory_state HTTP " + std::to_string(res.first) +
                    ": exchange=" + exchange_name,
                (nlohmann::json{{"endpoint", "get-inventory-state"},
                                {"http_status", res.first},
                                {"exchange_name", exchange_name},
                                {"market_type", market_type},
                                {"account_id", account_id},
                                {"body_head", res.second.substr(0, 200)}}));
            return std::nullopt;
        }

        auto json = nlohmann::json::parse(res.second);
        std::vector<Core::QuoteAsset> quote_assets;
        std::vector<Core::Inventory> inventories;

        const auto &inventory_state = json.at("inventory_state");

        for (const auto &item : inventory_state.at("quote_assets")) {
            Core::QuoteAsset quote_asset;
            quote_asset.asset = item.at("asset").get<std::string>();
            quote_asset.quantity = item.at("quantity").get<std::string>();
            quote_asset.used = item.at("used").get<std::string>();
            quote_asset.free = item.at("free").get<std::string>();
            quote_asset.price = item.at("price").get<double>();
            quote_asset.account_id = account_id;
            quote_asset.exchange_name = exchange_name;
            quote_asset.market_type = market_type;
            quote_assets.push_back(quote_asset);
        }

        for (const auto &item : inventory_state.at("inventory")) {
            Core::Inventory inventory = Core::Inventory::from_json(item);
            inventory.account_id = account_id;
            inventories.push_back(inventory);
        }

        return std::make_pair(quote_assets, inventories);
    } catch (const std::exception &e) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_CCXT_PROXY_RESPONSE_PARSE_FAILED,
                  std::string("get_inventory_state failed: ") + e.what(),
                  (nlohmann::json{{"endpoint", "get-inventory-state"},
                                  {"exchange_name", exchange_name},
                                  {"market_type", market_type},
                                  {"account_id", account_id},
                                  {"what", e.what()}}));
        return std::nullopt;
    } catch (...) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_CCXT_PROXY_RESPONSE_PARSE_FAILED,
                  "get_inventory_state failed: unknown",
                  (nlohmann::json{{"endpoint", "get-inventory-state"},
                                  {"exchange_name", exchange_name},
                                  {"market_type", market_type},
                                  {"account_id", account_id},
                                  {"what", "unknown"}}));
        return std::nullopt;
    }
}

std::vector<Core::OrderResNormalized> CcxtProxy::get_orders_on_symbol(
    const std::string &exchange_name, const std::string &market_type,
    const long long &account_id, const std::string &symbol) {
    if (rate_blocked(exchange_name, market_type)) return {};
    try {
        auto res = http_req(
            this->base_url + "/orders-on-symbol?exchange_name=" +
                exchange_name + "&market_type=" + market_type +
                "&account_id=" + std::to_string(account_id) + "&symbol=" +
                CCXTUtils::symbol_to_ccxt_format(symbol, market_type),
            "GET", "");
        note_rate_limit(exchange_name, market_type, res.first, res.second);

        if (res.first > 299) {
            return {};
        }

        auto json = nlohmann::json::parse(res.second);

        if (!json.contains("data")) {
            return {};
        }

        const auto &data = json.at("data");
        std::vector<Core::OrderResNormalized> orders;
        for (const auto &item : data) {
            orders.push_back(
                Core::OrderResNormalized::from_ccxt_user_data_stream_msg(item));
        }
        return orders;
    } catch (...) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_CCXT_PROXY_RESPONSE_PARSE_FAILED,
                  "get_orders_on_symbol failed: " + exchange_name + " " +
                      market_type + " " + symbol,
                  (nlohmann::json{{"endpoint", "orders"},
                                  {"exchange_name", exchange_name},
                                  {"market_type", market_type},
                                  {"symbol", symbol}}));
        return {};
    }
}

std::vector<Core::OrderResNormalized> CcxtProxy::get_open_orders_on_symbol(
    const std::string &exchange_name, const std::string &market_type,
    const long long &account_id, const std::string &symbol) {
    if (rate_blocked(exchange_name, market_type)) return {};
    try {
        auto res = http_req(
            this->base_url + "/open-orders?exchange_name=" + exchange_name +
                "&market_type=" + market_type +
                "&account_id=" + std::to_string(account_id) + "&symbol=" +
                CCXTUtils::symbol_to_ccxt_format(symbol, market_type),
            "GET", "");
        note_rate_limit(exchange_name, market_type, res.first, res.second);

        if (res.first > 299) {
            return {};
        }

        auto json = nlohmann::json::parse(res.second);

        if (!json.contains("data")) {
            return {};
        }

        const auto &data = json.at("data");
        std::vector<Core::OrderResNormalized> orders;
        for (const auto &item : data) {
            orders.push_back(
                Core::OrderResNormalized::from_ccxt_user_data_stream_msg(item));
        }
        return orders;
    } catch (...) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_CCXT_PROXY_RESPONSE_PARSE_FAILED,
                  "get_open_orders_on_symbol failed",
                  (nlohmann::json{{"endpoint", "open-orders"},
                                  {"exchange_name", exchange_name},
                                  {"market_type", market_type},
                                  {"symbol", symbol}}));
        return {};
    }
}

std::vector<Core::OrderResNormalized> CcxtProxy::get_all_open_orders(
    const std::string &exchange_name, const std::string &market_type,
    long long account_id) {
    if (rate_blocked(exchange_name, market_type)) return {};
    try {
        auto res = http_req(this->base_url + "/open-orders?exchange_name=" +
                                exchange_name + "&market_type=" + market_type +
                                "&account_id=" + std::to_string(account_id),
                            "GET", "");
        note_rate_limit(exchange_name, market_type, res.first, res.second);

        if (res.first > 299) {
            LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_CCXT_PROXY_REQUEST_FAILED,
                      "get_all_open_orders HTTP " + std::to_string(res.first) +
                          ": exchange=" + exchange_name,
                      (nlohmann::json{{"endpoint", "open-orders"},
                                      {"http_status", res.first},
                                      {"exchange_name", exchange_name},
                                      {"market_type", market_type},
                                      {"account_id", account_id}}));
            return {};
        }

        auto json = nlohmann::json::parse(res.second);
        if (!json.contains("data")) {
            return {};
        }

        const auto &data = json.at("data");
        std::vector<Core::OrderResNormalized> orders;
        for (const auto &item : data) {
            orders.push_back(
                Core::OrderResNormalized::from_ccxt_user_data_stream_msg(item));
        }
        return orders;
    } catch (const std::exception &e) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_CCXT_PROXY_RESPONSE_PARSE_FAILED,
                  std::string("get_all_open_orders failed: ") + e.what(),
                  (nlohmann::json{{"endpoint", "open-orders"},
                                  {"exchange_name", exchange_name},
                                  {"market_type", market_type},
                                  {"account_id", account_id},
                                  {"what", e.what()}}));
        return {};
    }
}

std::optional<Core::OrderResNormalized> CcxtProxy::query_order(
    const std::string &exchange_name, const std::string &market_type,
    const long long &account_id, const std::string &symbol,
    const std::string &client_order_id) {
    if (rate_blocked(exchange_name, market_type)) return std::nullopt;
    try {
        if (client_order_id.empty()) {
            return std::nullopt;
        }

        auto res = http_req(
            this->base_url + "/query-order?exchange_name=" + exchange_name +
                "&market_type=" + market_type +
                "&account_id=" + std::to_string(account_id) + "&symbol=" +
                CCXTUtils::symbol_to_ccxt_format(symbol, market_type) +
                "&client_order_id=" + client_order_id,
            "GET", "");
        note_rate_limit(exchange_name, market_type, res.first, res.second);
        if (res.first > 299) {
            return std::nullopt;
        }
        auto json = nlohmann::json::parse(res.second);
        if (!json.contains("data")) {
            return std::nullopt;
        }
        const auto &data = json.at("data");
        return Core::OrderResNormalized::from_ccxt_user_data_stream_msg(data);
    } catch (...) {
        nlohmann::json error_json;
        error_json["endpoint"] = "query-order";
        error_json["exchange_name"] = exchange_name;
        error_json["market_type"] = market_type;
        error_json["account_id"] = account_id;
        error_json["symbol"] = symbol;
        error_json["client_order_id"] = client_order_id;
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_CCXT_PROXY_RESPONSE_PARSE_FAILED,
                  "query_order failed", error_json);
        return std::nullopt;
    }
}

std::optional<Core::OrderResNormalized> CcxtProxy::query_order(
    const Core::Order &order) {
    return this->query_order(order.exchange_name, order.market_type,
                             order.account_id, order.get_order_api_symbol(),
                             order.get_order_lookup_id());
}

std::vector<MarketData::NormalizedParsedTrade> CcxtProxy::get_historical_trades(
    const std::string &exchange_name, const std::string &market_type,
    const std::string &symbol, const long long &from_timestamp,
    RateLimiter::RequestClass request_class) {
    std::string rate_key = exchange_name + ":" + market_type;
    // Reactive cooldown → bail; paced gate → spin-wait for a fair slot. The
    // limiter never blocks, so the brief backoff lives here.
    while (true) {
        if (RateLimiter::instance().is_limited(rate_key)) return {};
        if (!RateLimiter::instance().should_throttle(rate_key, request_class))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    try {
        auto res = http_req(
            this->base_url +
                "/historical-data/trades?exchange_name=" + exchange_name +
                "&market_type=" + market_type + "&symbol=" + symbol +
                "&from_timestamp=" + std::to_string(from_timestamp),
            "GET", "");
        if (res.first == 429 || res.first == 418) {
            int retry_after = 60;
            try {
                auto j = nlohmann::json::parse(res.second);
                if (j.contains("retry_after_s"))
                    retry_after = j["retry_after_s"].get<int>();
            } catch (...) {
            }
            RateLimiter::instance().record_limit(rate_key, retry_after);
            LOG_WARNING(Core::TRADING_LOG_EVENT_TYPE_CCXT_PROXY_RATE_LIMITED,
                        "Rate limited " + rate_key + " for " +
                            std::to_string(retry_after) + "s",
                        (nlohmann::json{{"rate_key", rate_key},
                                        {"retry_after_s", retry_after}}));
            return {};
        }
        if (res.first > 299) {
            return {};
        }
        auto json = nlohmann::json::parse(res.second);
        if (exchange_name == "gateio" && market_type == "spot") {
            return MarketData::NormalizedParsedTrade::
                from_list_of_gateio_trades(json, symbol, exchange_name,
                                           market_type);
        }
        // The proxy now serves RAW individual trades (Binance
        // /historicalTrades) for both by-id AND by-timestamp paths — strategies
        // need tick-level data, not aggregates. Use the same parser as
        // get_historical_trades_by_id.
        if (exchange_name == "binance") {
            return MarketData::NormalizedParsedTrade::
                from_list_of_binance_individual_trades(
                    json, symbol, exchange_name, market_type);
        }
        return MarketData::NormalizedParsedTrade::
            from_list_of_aggregated_trades(json, symbol, exchange_name,
                                           market_type);
    } catch (...) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_CCXT_PROXY_RESPONSE_PARSE_FAILED,
                  "get_historical_trades failed",
                  (nlohmann::json{{"endpoint", "historical-data/trades"},
                                  {"exchange_name", exchange_name},
                                  {"market_type", market_type},
                                  {"symbol", symbol},
                                  {"from_timestamp", from_timestamp}}));
        return {};
    }
}

std::vector<MarketData::NormalizedParsedTrade>
CcxtProxy::get_historical_trades_by_id(
    const std::string &exchange_name, const std::string &market_type,
    const std::string &symbol, const std::string &trade_id,
    RateLimiter::RequestClass request_class) {
    std::string rate_key = exchange_name + ":" + market_type;
    while (true) {
        if (RateLimiter::instance().is_limited(rate_key)) return {};
        if (!RateLimiter::instance().should_throttle(rate_key, request_class))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    try {
        auto res =
            http_req(this->base_url + "/historical-data/trades?exchange_name=" +
                         exchange_name + "&market_type=" + market_type +
                         "&symbol=" + symbol + "&trade_id=" + trade_id,
                     "GET", "");
        if (res.first == 429 || res.first == 418) {
            int retry_after = 60;
            try {
                auto j = nlohmann::json::parse(res.second);
                if (j.contains("retry_after_s"))
                    retry_after = j["retry_after_s"].get<int>();
            } catch (...) {
            }
            RateLimiter::instance().record_limit(rate_key, retry_after);
            LOG_WARNING(Core::TRADING_LOG_EVENT_TYPE_CCXT_PROXY_RATE_LIMITED,
                        "Rate limited " + rate_key + " for " +
                            std::to_string(retry_after) + "s",
                        (nlohmann::json{{"rate_key", rate_key},
                                        {"retry_after_s", retry_after}}));
            return {};
        }
        if (res.first > 299) {
            LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_CCXT_PROXY_REQUEST_FAILED,
                      "get_historical_trades_by_id HTTP " +
                          std::to_string(res.first) + " for " + symbol,
                      (nlohmann::json{{"endpoint", "historical-data/trades"},
                                      {"http_status", res.first},
                                      {"exchange_name", exchange_name},
                                      {"market_type", market_type},
                                      {"symbol", symbol},
                                      {"from_trade_id", trade_id}}));
            return {};
        }
        auto json = nlohmann::json::parse(res.second);
        if (exchange_name == "gateio" && market_type == "spot") {
            return MarketData::NormalizedParsedTrade::
                from_list_of_gateio_trades(json, symbol, exchange_name,
                                           market_type);
        }
        if (exchange_name == "binance") {
            return MarketData::NormalizedParsedTrade::
                from_list_of_binance_individual_trades(
                    json, symbol, exchange_name, market_type);
        }
        return MarketData::NormalizedParsedTrade::
            from_list_of_aggregated_trades(json, symbol, exchange_name,
                                           market_type);
    } catch (const std::exception &e) {
        LOG_ERROR(
            Core::TRADING_LOG_EVENT_TYPE_CCXT_PROXY_RESPONSE_PARSE_FAILED,
            std::string("get_historical_trades_by_id failed: ") + e.what(),
            (nlohmann::json{{"endpoint", "historical-data/trades"},
                            {"exchange_name", exchange_name},
                            {"market_type", market_type},
                            {"symbol", symbol},
                            {"from_trade_id", trade_id},
                            {"what", e.what()}}));
        return {};
    }
}

std::vector<std::string> CcxtProxy::get_exchange_symbols(
    const std::string &exchange_name, const std::string &market_type) {
    std::string rate_key = exchange_name + ":" + market_type;
    if (RateLimiter::instance().is_limited(rate_key)) return {};
    try {
        auto res = http_req(this->base_url + "/get-symbols?exchange_name=" +
                                exchange_name + "&market_type=" + market_type,
                            "GET", "");
        if (res.first == 429 || res.first == 418) {
            int retry_after = 60;
            try {
                auto j = nlohmann::json::parse(res.second);
                if (j.contains("retry_after_s"))
                    retry_after = j["retry_after_s"].get<int>();
            } catch (...) {
            }
            RateLimiter::instance().record_limit(rate_key, retry_after);
            return {};
        }
        if (res.first > 299) {
            return {};
        }
        auto json = nlohmann::json::parse(res.second);
        if (!json.contains("data")) {
            return {};
        }
        return json.at("data").get<std::vector<std::string>>();
    } catch (...) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_CCXT_PROXY_RESPONSE_PARSE_FAILED,
                  "get_exchange_symbols failed",
                  (nlohmann::json{{"endpoint", "get-symbols"},
                                  {"exchange_name", exchange_name},
                                  {"market_type", market_type}}));
        return {};
    }
}

std::vector<std::string> CcxtProxy::get_exchange_active_symbols(
    const std::string &exchange_name, const std::string &market_type) {
    if (rate_blocked(exchange_name, market_type)) return {};
    try {
        auto res =
            http_req(this->base_url + "/get-active-symbols?exchange_name=" +
                         exchange_name + "&market_type=" + market_type,
                     "GET", "");
        note_rate_limit(exchange_name, market_type, res.first, res.second);
        if (res.first > 299) {
            return {};
        }
        auto json = nlohmann::json::parse(res.second);
        if (!json.contains("data")) {
            nlohmann::json error_json;
            error_json["endpoint"] = "get-active-symbols";
            error_json["exchange_name"] = exchange_name;
            error_json["market_type"] = market_type;
            error_json["error"] = res.second;
            error_json["http_status"] = res.first;
            LOG_ERROR(
                Core::TRADING_LOG_EVENT_TYPE_CCXT_PROXY_RESPONSE_PARSE_FAILED,
                "get_exchange_active_symbols failed", error_json);
            return {};
        }
        return json.at("data").get<std::vector<std::string>>();
    } catch (...) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_CCXT_PROXY_RESPONSE_PARSE_FAILED,
                  "get_exchange_active_symbols failed",
                  (nlohmann::json{{"endpoint", "get-active-symbols"},
                                  {"exchange_name", exchange_name},
                                  {"market_type", market_type}}));
        return {};
    }
}

std::vector<MarketData::OhlcvDatasetTick> CcxtProxy::get_ohlcv_dataset_ticks(
    const std::string &exchange_name, const std::string &market_type,
    const std::string &symbol, const std::string &interval,
    const long long &from_timestamp, int limit,
    RateLimiter::RequestClass request_class, bool *failed) {
    const auto fail = [failed]() {
        if (failed != nullptr) *failed = true;
        return std::vector<MarketData::OhlcvDatasetTick>{};
    };
    if (failed != nullptr) *failed = false;
    std::string rate_key = exchange_name + ":" + market_type;
    while (true) {
        if (RateLimiter::instance().is_limited(rate_key)) return fail();
        if (!RateLimiter::instance().should_throttle(rate_key, request_class))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    try {
        const std::unique_ptr<char, decltype(&curl_free)> encoded_symbol(
            curl_easy_escape(nullptr, symbol.c_str(),
                             static_cast<int>(symbol.size())),
            &curl_free);
        if (!encoded_symbol) return fail();
        auto res =
            http_req(this->base_url + "/historical-data/bars?exchange_name=" +
                         exchange_name + "&market_type=" + market_type +
                         "&symbol=" + encoded_symbol.get() + "&interval=" + interval +
                         "&from_timestamp=" + std::to_string(from_timestamp) +
                         "&limit=" + std::to_string(limit),
                     "GET", "");
        if (res.first == 429 || res.first == 418) {
            int retry_after = 60;
            try {
                auto j = nlohmann::json::parse(res.second);
                if (j.contains("retry_after_s"))
                    retry_after = j["retry_after_s"].get<int>();
            } catch (...) {
            }
            RateLimiter::instance().record_limit(rate_key, retry_after);
            return fail();
        }
        if (res.first > 299) {
            return fail();
        }
        auto json = nlohmann::json::parse(res.second);

        if (!json.contains("data")) {
            LOG_ERROR(
                Core::TRADING_LOG_EVENT_TYPE_CCXT_PROXY_RESPONSE_PARSE_FAILED,
                "get_ohlcv_dataset_ticks missing data field",
                (nlohmann::json{{"endpoint", "historical-data/bars"},
                                {"exchange_name", exchange_name},
                                {"market_type", market_type},
                                {"symbol", symbol},
                                {"interval", interval},
                                {"from_timestamp", from_timestamp}}));
            return fail();
        }
        return MarketData::OhlcvDatasetTick::from_cpp_proxy_res(
            json, exchange_name, market_type,
            MarketData::convert_interval_str_to_ms(interval));
    } catch (...) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_CCXT_PROXY_RESPONSE_PARSE_FAILED,
                  "get_ohlcv_dataset_ticks failed",
                  (nlohmann::json{{"endpoint", "historical-data/bars"},
                                  {"exchange_name", exchange_name},
                                  {"market_type", market_type},
                                  {"symbol", symbol},
                                  {"interval", interval}}));
        return fail();
    }
}

bool CcxtProxy::is_symbol_active(const std::string &exchange_name,
                                 const std::string &market_type,
                                 const std::string &symbol) {
    try {
        const std::string &key = exchange_name + ":" + market_type;
        if (this->last_active_symbols_update_time + std::chrono::minutes(60) <
            std::chrono::steady_clock::now()) {
            this->last_active_symbols_update_time =
                std::chrono::steady_clock::now();

            std::unordered_set<std::string> active_symbols{};

            for (const auto &symbol : this->get_exchange_active_symbols(
                     exchange_name, market_type)) {
                active_symbols.insert(symbol);
            }
            this->active_symbols_by_exchange_and_market_type[key] =
                active_symbols;
        }
        return this->active_symbols_by_exchange_and_market_type[key].contains(
            symbol);
    } catch (...) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_CCXT_PROXY_RESPONSE_PARSE_FAILED,
                  "is_symbol_active failed",
                  (nlohmann::json{{"endpoint", "get-active-symbols"},
                                  {"exchange_name", exchange_name},
                                  {"market_type", market_type},
                                  {"symbol", symbol}}));
        return false;
    }
}

void CcxtProxy::cancel_order_v2(const std::string &client_order_id,
                                const std::string &exchange_name,
                                const std::string &market_type,
                                const std::string &symbol,
                                const long long &account_id) {
    if (exchange_writes_disabled()) {
        return;
    }
    http_req(
        this->base_url + "/cancel-order-v2?client_order_id=" + client_order_id +
            "&exchange_name=" + exchange_name + "&market_type=" + market_type +
            "&symbol=" + symbol + "&account_id=" + std::to_string(account_id),
        "POST", "");
}

std::vector<Core::SymbolPriceInfo> CcxtProxy::fetch_prices(
    const std::string &exchange_name, const std::string &market_type) {
    if (rate_blocked(exchange_name, market_type)) return {};
    try {
        auto res = http_req(this->base_url + "/fetch-prices?exchange_name=" +
                                exchange_name + "&market_type=" + market_type,
                            "GET", "");
        note_rate_limit(exchange_name, market_type, res.first, res.second);
        if (res.first > 299) {
            return {};
        }
        auto json = nlohmann::json::parse(res.second);
        if (!json.contains("data")) {
            LOG_ERROR(
                Core::TRADING_LOG_EVENT_TYPE_CCXT_PROXY_RESPONSE_PARSE_FAILED,
                "fetch_prices missing data field",
                (nlohmann::json{{"endpoint", "fetch-prices"},
                                {"exchange_name", exchange_name},
                                {"market_type", market_type}}));
            return {};
        }
        std::vector<Core::SymbolPriceInfo> prices;
        for (const auto &item : json.at("data")) {
            Core::SymbolPriceInfo price;
            price.exchange_name = exchange_name;
            price.market_type = market_type;
            price.symbol = CCXTUtils::convert_ccxt_symbol(
                item.at("symbol").get<std::string>());
            price.price = item.at("price").get<double>();
            prices.push_back(price);
        }
        return prices;
    } catch (...) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_CCXT_PROXY_RESPONSE_PARSE_FAILED,
                  "fetch_prices failed",
                  (nlohmann::json{{"endpoint", "fetch-prices"},
                                  {"exchange_name", exchange_name},
                                  {"market_type", market_type}}));
        return {};
    }
}

bool CcxtProxy::call_stream_instance_ip_rotation_check(
    const std::string &exchange_name, const std::string &market_type) {
    auto res = http_req(this->stream_instance_url +
                            "/test-ip-rotation?exchange_name=" + exchange_name +
                            "&market_type=" + market_type +
                            "&account_id=-1&is_public=False",
                        "GET", "");
    if (res.first > 299) {
        return false;
    }
    return true;
}

bool CcxtProxy::call_base_instance_ip_rotation_check(
    const std::string &exchange_name, const std::string &market_type) {
    auto res = http_req(
        this->base_url + "/test-ip-rotation?exchange_name=" + exchange_name +
            "&market_type=" + market_type + "&account_id=-1&is_public=False",
        "GET", "");
    if (res.first > 299) {
        return false;
    }
    return true;
}

void CcxtProxy::update_swaps_leverage_level(const std::string &exchange_name,
                                            const std::string &market_type,
                                            const long long &account_id,
                                            const std::string &symbol,
                                            const int &leverage_level) {
    http_req(this->base_url + "/swaps-default-leverage-level?exchange_name=" +
                 exchange_name + "&market_type=" + market_type +
                 "&account_id=" + std::to_string(account_id) + "&symbol=" +
                 symbol + "&leverage_level=" + std::to_string(leverage_level),
             "POST", "");
}

std::optional<Core::OrderBookLevels> CcxtProxy::get_full_orderbook(
    const std::string &exchange_name, const std::string &market_type,
    const std::string &symbol, int limit) {
    if (rate_blocked(exchange_name, market_type)) return std::nullopt;
    try {
        auto res = http_req(
            this->base_url + "/full-orderbook?exchange_name=" + exchange_name +
                "&market_type=" + market_type + "&symbol=" +
                CCXTUtils::symbol_to_ccxt_format(symbol, market_type) +
                "&limit=" + std::to_string(limit),
            "GET", "");
        note_rate_limit(exchange_name, market_type, res.first, res.second);

        if (res.first > 299) {
            return std::nullopt;
        }

        auto json = nlohmann::json::parse(res.second);

        if (!json.contains("data")) {
            return std::nullopt;
        }

        const auto &data = json.at("data");
        Core::OrderBookLevels levels;

        if (data.contains("nonce") && !data["nonce"].is_null()) {
            levels.last_sequence_number = data["nonce"].get<long long>();
        }

        auto parse_level =
            [](const nlohmann::json &level) -> std::pair<double, double> {
            double price = level[0].is_string()
                               ? std::stod(level[0].get<std::string>())
                               : level[0].get<double>();
            double amount = level[1].is_string()
                                ? std::stod(level[1].get<std::string>())
                                : level[1].get<double>();
            return {price, amount};
        };

        long long ts_ms = 0;
        if (data.contains("timestamp") && !data["timestamp"].is_null()) {
            ts_ms = data["timestamp"].get<long long>();
        }
        levels.timestamp_ms = ts_ms;

        if (data.contains("bids")) {
            for (const auto &bid : data["bids"]) {
                auto [price, amount] = parse_level(bid);
                levels.bids.emplace_back(price, amount);
            }
        }

        if (data.contains("asks")) {
            for (const auto &ask : data["asks"]) {
                auto [price, amount] = parse_level(ask);
                levels.asks.emplace_back(price, amount);
            }
        }
        return levels;
    } catch (const std::exception &e) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_CCXT_PROXY_RESPONSE_PARSE_FAILED,
                  std::string("get_full_orderbook failed: ") + e.what(),
                  (nlohmann::json{{"endpoint", "orderbook"},
                                  {"exchange_name", exchange_name},
                                  {"market_type", market_type},
                                  {"symbol", symbol},
                                  {"what", e.what()}}));
        return std::nullopt;
    }
}

std::vector<Core::AssetBorrowableInfo> CcxtProxy::fetch_borrowable_inventories(
    const std::string &exchange_name, const std::string &market_type,
    const long long &account_id, const std::vector<std::string> &symbols) {
    if (rate_blocked(exchange_name, market_type)) return {};
    try {
        std::string url =
            this->base_url +
            "/borrowable-inventories?exchange_name=" + exchange_name +
            "&market_type=" + market_type +
            "&account_id=" + std::to_string(account_id);

        for (const auto &symbol : symbols) {
            url += "&symbols=" + symbol;
        }

        auto res = http_req(url, "GET", "");
        note_rate_limit(exchange_name, market_type, res.first, res.second);
        if (res.first > 299) {
            return {};
        }

        auto json = nlohmann::json::parse(res.second);
        if (!json.contains("data")) {
            LOG_ERROR(
                Core::TRADING_LOG_EVENT_TYPE_CCXT_PROXY_RESPONSE_PARSE_FAILED,
                "fetch_borrowable_inventories missing data field",
                (nlohmann::json{{"endpoint", "borrowable-inventories"},
                                {"exchange_name", exchange_name},
                                {"market_type", market_type},
                                {"account_id", account_id}}));
            return {};
        }

        std::vector<Core::AssetBorrowableInfo> result;
        for (const auto &item : json.at("data")) {
            Core::AssetBorrowableInfo info;
            info.exchange_name = exchange_name;
            info.market_type = market_type;

            if (item.contains("currency")) {
                info.asset = item.at("currency").get<std::string>();
            } else if (item.contains("symbol")) {
                info.asset = item.at("symbol").get<std::string>();
            }

            if (item.contains("available")) {
                std::string available_str =
                    item.at("available").get<std::string>();
                info.available_for_borrow = std::stod(available_str);
            } else if (item.contains("available_for_borrow")) {
                if (item.at("available_for_borrow").is_string()) {
                    std::string available_str =
                        item.at("available_for_borrow").get<std::string>();
                    info.available_for_borrow = std::stod(available_str);
                } else {
                    info.available_for_borrow =
                        item.at("available_for_borrow").get<double>();
                }
            } else {
                info.available_for_borrow = 0.0;
            }

            result.push_back(info);
        }
        return result;
    } catch (...) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_CCXT_PROXY_RESPONSE_PARSE_FAILED,
                  "fetch_borrowable_inventories failed",
                  (nlohmann::json{{"endpoint", "borrowable-inventories"},
                                  {"exchange_name", exchange_name},
                                  {"market_type", market_type},
                                  {"account_id", account_id}}));
        return {};
    }
}

CcxtProxy::MintContractsResult CcxtProxy::mint_prediction_market_contracts(
    const std::string &condition_id, bool neg_risk, int num_sets,
    long long account_id) {
    MintContractsResult result;

    nlohmann::json body;
    body["condition_id"] = condition_id;
    body["neg_risk"] = neg_risk;
    body["num_sets"] = num_sets;
    body["account_id"] = account_id;

    auto res =
        http_req(this->base_url + "/mint-contracts", "POST", body.dump());

    if (res.first < 200 || res.first >= 300) {
        result.error = "HTTP " + std::to_string(res.first) + ": " + res.second;
        return result;
    }

    try {
        auto json = nlohmann::json::parse(res.second);
        result.tx_hash = json.value("tx_hash", "");
        result.gas_used = json.value("gas_used", 0);
        result.success = json.value("status", "") == "success";
        if (!result.success) {
            result.error = json.value("status", "unknown");
        }
    } catch (const std::exception &e) {
        result.error = std::string("JSON parse error: ") + e.what();
    }

    return result;
}

void CcxtProxy::subscribe_polymarket_stream(
    const std::vector<std::string> &condition_ids) {
    try {
        nlohmann::json body;
        body["condition_ids"] = condition_ids;
        http_req(this->stream_instance_url + "/polymarket-stream/subscribe",
                 "POST", body.dump());
    } catch (...) {
        LOG_ERROR(
            Core::TRADING_LOG_EVENT_TYPE_CCXT_PROXY_REQUEST_FAILED,
            "subscribe_polymarket_stream failed",
            (nlohmann::json{{"endpoint", "polymarket-stream/subscribe"},
                            {"num_condition_ids", condition_ids.size()}}));
    }
}

std::vector<Core::OrderResNormalized> CcxtProxy::get_polymarket_all_open_orders(
    long long account_id) {
    try {
        auto res = http_req(this->base_url +
                                "/polymarket-all-open-orders?account_id=" +
                                std::to_string(account_id),
                            "GET", "");
        if (res.first < 200 || res.first >= 300) {
            return {};
        }
        auto json = nlohmann::json::parse(res.second);
        if (!json.contains("data") || !json["data"].is_array()) {
            return {};
        }
        std::vector<Core::OrderResNormalized> orders;
        orders.reserve(json["data"].size());
        for (const auto &item : json["data"]) {
            if (!item.contains("id") || !item["id"].is_string()) continue;
            auto order_res =
                Core::OrderResNormalized::from_ccxt_user_data_stream_msg(item);
            order_res.normalize_status();
            orders.push_back(std::move(order_res));
        }
        return orders;
    } catch (...) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_CCXT_PROXY_RESPONSE_PARSE_FAILED,
                  "get_polymarket_all_open_orders failed",
                  (nlohmann::json{{"endpoint", "polymarket-all-open-orders"},
                                  {"account_id", account_id}}));
        return {};
    }
}

CcxtProxy::RedeemContractsResult CcxtProxy::redeem_prediction_market_contracts(
    const std::string &condition_id, bool neg_risk, const std::string &slug,
    long long account_id) {
    RedeemContractsResult result;

    nlohmann::json body;
    body["condition_id"] = condition_id;
    body["neg_risk"] = neg_risk;
    body["slug"] = slug;
    body["account_id"] = account_id;

    auto res =
        http_req(this->base_url + "/redeem-winnings", "POST", body.dump());

    if (res.first < 200 || res.first >= 300) {
        result.error = "HTTP " + std::to_string(res.first) + ": " + res.second;
        return result;
    }

    try {
        auto json = nlohmann::json::parse(res.second);
        result.status = json.value("status", "");
        result.tx_hash = json.value("tx_hash", "");
        result.usdc_gained = json.value("usdc_gained", 0.0);
        result.success =
            result.status == "success" || result.status == "no_tokens";
        if (!result.success) {
            result.error = json.value("error", result.status);
        }
    } catch (const std::exception &e) {
        result.error = std::string("JSON parse error: ") + e.what();
    }

    return result;
}

void CcxtProxy::init_polymarket_direct_signing() {
    const char *env_val = std::getenv("POLYMARKET_DIRECT_SIGNING");
    if (env_val && std::string(env_val) == "0") {
        std::cout << "[POLYMARKET_DIRECT] Direct signing explicitly disabled"
                  << std::endl;
        return;
    }

    std::cout << "[POLYMARKET_DIRECT] Fetching credentials from Python proxy..."
              << std::endl;

    auto res = http_req(this->base_url + "/get-credentials", "GET", "",
                        this->build_proxy_auth_headers());
    if (res.first < 200 || res.first >= 300) {
        std::cout << "[POLYMARKET_DIRECT] Failed to fetch credentials: status="
                  << res.first << std::endl;
        return;
    }

    try {
        auto json = nlohmann::json::parse(res.second);
        if (!json.contains("accounts")) {
            std::cout << "[POLYMARKET_DIRECT] No 'accounts' in response"
                      << std::endl;
            return;
        }

        polymarket_client_.emplace();

        for (auto &[account_id_str, acct_json] : json["accounts"].items()) {
            long long account_id = std::stoll(account_id_str);

            PolymarketDirect::AccountCredentials creds;
            creds.private_key = Secp256k1Signer::parse_private_key(
                acct_json["private_key"].get<std::string>());
            creds.api_key = acct_json["api_key"].get<std::string>();
            creds.api_secret = acct_json["api_secret"].get<std::string>();
            creds.api_passphrase =
                acct_json["api_passphrase"].get<std::string>();
            creds.maker_address = acct_json["maker_address"].get<std::string>();
            creds.signer_address =
                acct_json["signer_address"].get<std::string>();

            if (acct_json.contains("builder_api_key") &&
                !acct_json["builder_api_key"].get<std::string>().empty()) {
                creds.builder_api_key =
                    acct_json["builder_api_key"].get<std::string>();
                creds.builder_secret =
                    acct_json["builder_secret"].get<std::string>();
                creds.builder_passphrase =
                    acct_json["builder_passphrase"].get<std::string>();
            }

            polymarket_client_->add_account(account_id, creds);
            std::cout << "[POLYMARKET_DIRECT] Loaded account_id=" << account_id
                      << " signer=" << creds.signer_address << std::endl;
        }

        polymarket_direct_enabled_ = true;
        polymarket_client_->warmup_connection();
        std::cout << "[POLYMARKET_DIRECT] Direct signing enabled" << std::endl;
    } catch (const std::exception &e) {
        std::cout << "[POLYMARKET_DIRECT] Failed to parse credentials: "
                  << e.what() << std::endl;
        polymarket_client_.reset();
    }
}

bool CcxtProxy::is_polymarket_direct_signing_enabled() const {
    return polymarket_direct_enabled_;
}

bool CcxtProxy::is_polymarket_rate_limited() const {
    return polymarket_client_.has_value() &&
           polymarket_client_->is_rate_limited();
}

AsterDirect::AsterClient *CcxtProxy::get_aster_client() {
    return aster_client_.has_value() ? &aster_client_.value() : nullptr;
}

// ── Hyperliquid direct signing ──────────────────────────────────────────

HyperliquidDirect::HyperliquidClient *CcxtProxy::get_hyperliquid_client() {
    return hyperliquid_client_.has_value() ? &hyperliquid_client_.value()
                                           : nullptr;
}

void CcxtProxy::init_hyperliquid_direct_signing() {
    const char *env_val = std::getenv("HYPERLIQUID_DIRECT_SIGNING");
    if (env_val && std::string(env_val) == "0") {
        std::cout << "[HYPERLIQUID_DIRECT] Direct signing explicitly disabled"
                  << std::endl;
        return;
    }

    std::cout
        << "[HYPERLIQUID_DIRECT] Fetching credentials from Python proxy..."
        << std::endl;

    auto res = http_req(this->base_url + "/get-hyperliquid-credentials", "GET",
                        "", this->build_proxy_auth_headers());
    if (res.first < 200 || res.first >= 300) {
        std::cout << "[HYPERLIQUID_DIRECT] Failed to fetch credentials: status="
                  << res.first << std::endl;
        return;
    }

    try {
        auto json = nlohmann::json::parse(res.second);
        if (!json.contains("accounts")) {
            std::cout << "[HYPERLIQUID_DIRECT] No 'accounts' in response"
                      << std::endl;
            return;
        }

        hyperliquid_client_.emplace();

        const char *testnet_env = std::getenv("HYPERLIQUID_USE_TESTNET");
        bool use_testnet = testnet_env && std::string(testnet_env) == "1";

        const char *base_url_env = std::getenv("HYPERLIQUID_API_BASE_URL");
        std::string hl_base_url;
        if (base_url_env) {
            hl_base_url = std::string(base_url_env);
        } else {
            hl_base_url = use_testnet ? "https://api.hyperliquid-testnet.xyz"
                                      : "https://api.hyperliquid.xyz";
        }

        hyperliquid_client_->set_base_url(hl_base_url);
        hyperliquid_client_->set_mainnet(!use_testnet);

        std::cout << "[HYPERLIQUID_DIRECT] mode="
                  << (use_testnet ? "testnet" : "mainnet")
                  << " url=" << hl_base_url << std::endl;

        for (auto &[account_id_str, acct_json] : json["accounts"].items()) {
            long long account_id = std::stoll(account_id_str);

            HyperliquidDirect::AccountCredentials creds;
            creds.agent_private_key = Secp256k1Signer::parse_private_key(
                acct_json["private_key"].get<std::string>());
            creds.account_address =
                acct_json["account_address"].get<std::string>();

            hyperliquid_client_->add_account(account_id, creds);
            std::cout << "[HYPERLIQUID_DIRECT] Loaded account_id=" << account_id
                      << " address=" << creds.account_address << std::endl;
        }

        if (!hyperliquid_client_->load_meta()) {
            std::cout << "[HYPERLIQUID_DIRECT] Failed to load meta — disabling"
                      << std::endl;
            hyperliquid_client_.reset();
            return;
        }

        hyperliquid_direct_enabled_ = true;
        hyperliquid_client_->warmup_connection();
        std::cout << "[HYPERLIQUID_DIRECT] Direct signing enabled" << std::endl;
    } catch (const std::exception &e) {
        std::cout << "[HYPERLIQUID_DIRECT] Failed to parse credentials: "
                  << e.what() << std::endl;
        hyperliquid_client_.reset();
    }
}

bool CcxtProxy::is_hyperliquid_direct_signing_enabled() const {
    return hyperliquid_direct_enabled_;
}

std::vector<Core::PolymarketBulkOrderResult>
CcxtProxy::create_hyperliquid_orders_bulk(
    const std::vector<Core::Order> &orders) {
    if (hyperliquid_direct_enabled_ && hyperliquid_client_.has_value() &&
        !orders.empty() &&
        hyperliquid_client_->has_account(orders[0].account_id)) {
        return hyperliquid_client_->create_orders_bulk(orders);
    }

    // Fail fast — proxy fallback would misroute Hyperliquid orders.
    std::vector<Core::PolymarketBulkOrderResult> results;
    for (size_t i = 0; i < orders.size(); ++i) {
        Core::PolymarketBulkOrderResult r;
        r.error_msg = "hyperliquid direct client unavailable";
        results.push_back(r);
    }
    return results;
}

std::vector<Core::PolymarketBulkCancelResult>
CcxtProxy::cancel_hyperliquid_orders_bulk(
    const std::vector<std::string> &order_ids, long long account_id,
    const std::string &symbol) {
    if (hyperliquid_direct_enabled_ && hyperliquid_client_.has_value() &&
        !order_ids.empty() && hyperliquid_client_->has_account(account_id)) {
        return hyperliquid_client_->cancel_orders(order_ids, account_id,
                                                  symbol);
    }

    // Fail fast — proxy fallback would misroute Hyperliquid cancels.
    std::vector<Core::PolymarketBulkCancelResult> results;
    for (size_t i = 0; i < order_ids.size(); ++i) {
        Core::PolymarketBulkCancelResult r;
        r.error_msg = "hyperliquid direct client unavailable";
        results.push_back(r);
    }
    return results;
}

// ── Aster direct signing ────────────────────────────────────────────────

void CcxtProxy::init_aster_direct_signing() {
    const char *env_val = std::getenv("ASTER_DIRECT_SIGNING");
    if (env_val && std::string(env_val) == "0") {
        std::cout << "[ASTER_DIRECT] Direct signing explicitly disabled"
                  << std::endl;
        return;
    }

    std::cout << "[ASTER_DIRECT] Fetching credentials from Python proxy..."
              << std::endl;

    auto res = http_req(this->base_url + "/get-aster-credentials", "GET", "",
                        this->build_proxy_auth_headers());
    if (res.first < 200 || res.first >= 300) {
        std::cout << "[ASTER_DIRECT] Failed to fetch credentials: status="
                  << res.first << std::endl;
        return;
    }

    try {
        auto json = nlohmann::json::parse(res.second);
        if (!json.contains("accounts")) {
            std::cout << "[ASTER_DIRECT] No 'accounts' in response"
                      << std::endl;
            return;
        }

        aster_client_.emplace();

        // Testnet: ASTER_USE_TESTNET=1 → chainId 714, testnet URL
        // Default: mainnet chainId 1666
        const char *testnet_env = std::getenv("ASTER_USE_TESTNET");
        bool use_testnet = testnet_env && std::string(testnet_env) == "1";

        const char *base_url_env = std::getenv("ASTER_API_BASE_URL");
        std::string aster_base_url;
        if (base_url_env) {
            aster_base_url = std::string(base_url_env);
        } else {
            aster_base_url = use_testnet ? "https://www.asterdex-testnet.com"
                                         : "https://fapi.asterdex.com";
        }

        aster_client_->set_base_url(aster_base_url);
        aster_client_->set_chain_id(use_testnet ? 714 : 1666);

        std::cout << "[ASTER_DIRECT] mode="
                  << (use_testnet ? "testnet" : "mainnet")
                  << " chain_id=" << (use_testnet ? 714 : 1666)
                  << " url=" << aster_base_url << std::endl;

        for (auto &[account_id_str, acct_json] : json["accounts"].items()) {
            long long account_id = std::stoll(account_id_str);

            AsterDirect::AccountCredentials creds;
            creds.private_key = Secp256k1Signer::parse_private_key(
                acct_json["private_key"].get<std::string>());
            creds.user_address = acct_json["user_address"].get<std::string>();
            creds.signer_address =
                acct_json["signer_address"].get<std::string>();

            aster_client_->add_account(account_id, creds);
            std::cout << "[ASTER_DIRECT] Loaded account_id=" << account_id
                      << " signer=" << creds.signer_address << std::endl;
        }

        aster_direct_enabled_ = true;
        aster_client_->warmup_connection();
        std::cout << "[ASTER_DIRECT] Direct signing enabled" << std::endl;
    } catch (const std::exception &e) {
        std::cout << "[ASTER_DIRECT] Failed to parse credentials: " << e.what()
                  << std::endl;
        aster_client_.reset();
    }
}

bool CcxtProxy::is_aster_direct_signing_enabled() const {
    return aster_direct_enabled_;
}

std::vector<Core::PolymarketBulkOrderResult>
CcxtProxy::create_aster_orders_bulk(const std::vector<Core::Order> &orders) {
    if (aster_direct_enabled_ && aster_client_.has_value() && !orders.empty() &&
        aster_client_->has_account(orders[0].account_id)) {
        return aster_client_->create_orders_bulk(orders);
    }

    // Fail fast — proxy fallback would misroute Aster orders to Polymarket
    std::vector<Core::PolymarketBulkOrderResult> results;
    for (size_t i = 0; i < orders.size(); ++i) {
        Core::PolymarketBulkOrderResult r;
        r.error_msg = "aster direct client unavailable";
        results.push_back(r);
    }
    return results;
}

std::vector<Core::PolymarketBulkCancelResult>
CcxtProxy::cancel_aster_orders_bulk(const std::vector<std::string> &order_ids,
                                    long long account_id,
                                    const std::string &symbol) {
    if (aster_direct_enabled_ && aster_client_.has_value() &&
        !order_ids.empty() && aster_client_->has_account(account_id)) {
        return aster_client_->cancel_orders(order_ids, account_id, symbol);
    }

    // Fail fast — proxy fallback would misroute Aster cancels to Polymarket
    std::vector<Core::PolymarketBulkCancelResult> results;
    for (size_t i = 0; i < order_ids.size(); ++i) {
        Core::PolymarketBulkCancelResult r;
        r.error_msg = "aster direct client unavailable";
        results.push_back(r);
    }
    return results;
}
