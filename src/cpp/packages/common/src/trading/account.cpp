#include "trading/account.h"

#include <optional>

#include "env_utils.h"
#include "market_making/dtypes/exchange_info.h"

Trading::Account::Account(MarketType market, ExchangeType type,
                          long long account_id, const std::string& access,
                          const std::string& secret,
                          LoggingCallback info_callback,
                          LoggingCallback error_callback)
    : market(market),
      type(type),
      account_id(account_id),
      access(access),
      secret(secret),
      info_callback(info_callback),
      error_callback(error_callback),
      ccxt_proxy(CcxtProxy::instance()) {
    init_account();
}

Trading::Account::~Account() {}

void Trading::Account::init_account() {
    const char* is_prod_env = std::getenv("IS_PROD");
    bool use_testnet =
        is_prod_env != nullptr && std::strcmp(is_prod_env, "0") == 0;
    this->is_testnet = use_testnet;
    if (is_prod_env == nullptr) {
        this->info_callback(
            "WARNING: IS_PROD is unset; Account is defaulting to production "
            "mode. Set IS_PROD=1 explicitly on live servers before the "
            "default is flipped to testnet-safe.");
    }
    if (this->type == ExchangeType::BINANCE_ACCOUNT) {
        this->exchange_name = "binance";
        this->market_type =
            this->market == MarketType::SWAP_MARKET_TYPE ? "swap" : "spot";
    }

    if (this->type == ExchangeType::BYBIT_ACCOUNT) {
        this->exchange_name = "bybit";
        this->market_type =
            this->market == MarketType::SWAP_MARKET_TYPE ? "swap" : "spot";
    }

    if (this->type == ExchangeType::BITGET_ACCOUNT) {
        this->exchange_name = "bitget";
        this->market_type =
            this->market == MarketType::SWAP_MARKET_TYPE ? "swap" : "spot";
    }

    if (this->type == ExchangeType::GATEIO_ACCOUNT) {
        this->exchange_name = "gateio";
        this->market_type =
            this->market == MarketType::SWAP_MARKET_TYPE ? "swap" : "spot";
    }

    if (this->type == ExchangeType::POLYMARKET_ACCOUNT) {
        this->exchange_name = "polymarket";
        this->market_type = "spot";
    }

    if (this->type == ExchangeType::ASTER_ACCOUNT) {
        this->exchange_name = "aster";
        this->market_type = "swap";
    }

    if (this->type == ExchangeType::HYPERLIQUID_ACCOUNT) {
        this->exchange_name = "hyperliquid";
        this->market_type = "swap";
    }

    if (this->type == ExchangeType::IBKR_ACCOUNT) {
        this->exchange_name = "ibkr";
        this->market_type =
            this->market == MarketType::SWAP_MARKET_TYPE ? "swap" : "spot";
    }
}

std::optional<nlohmann::json> Trading::Account::create_order(
    const Core::Order& order, int* http_status) {
    return this->ccxt_proxy.create_order(order, http_status);
}

std::vector<PolymarketDirect::PolymarketClient::SignedOrderBody>
Trading::Account::sign_polymarket_orders(
    const std::vector<Core::Order>& orders) {
    return this->ccxt_proxy.sign_polymarket_orders(orders);
}

std::vector<Core::PolymarketBulkOrderResult>
Trading::Account::submit_signed_polymarket_orders(
    const std::vector<PolymarketDirect::PolymarketClient::SignedOrderBody>
        &signed_orders,
    long long account_id) {
    return this->ccxt_proxy.submit_signed_polymarket_orders(signed_orders,
                                                             account_id);
}

Core::PolymarketBulkOrderResult Trading::Account::submit_signed_polymarket_order(
    const PolymarketDirect::PolymarketClient::SignedOrderBody& signed_order,
    long long account_id) {
    return this->ccxt_proxy.submit_signed_polymarket_order(signed_order,
                                                            account_id);
}

std::vector<Core::PolymarketBulkOrderResult>
Trading::Account::create_polymarket_orders_bulk(
    const std::vector<Core::Order>& orders) {
    return this->ccxt_proxy.create_polymarket_orders_bulk(orders);
}

std::optional<nlohmann::json> Trading::Account::cancel_order(
    const Core::Order& order) {
    return this->ccxt_proxy.cancel_order(order);
}

std::vector<Core::PolymarketBulkCancelResult>
Trading::Account::cancel_polymarket_orders_bulk(
    const std::vector<std::string>& order_ids) {
    return this->ccxt_proxy.cancel_polymarket_orders_bulk(order_ids, this->account_id);
}

std::vector<Core::PolymarketBulkOrderResult>
Trading::Account::create_aster_orders_bulk(
    const std::vector<Core::Order>& orders) {
    return this->ccxt_proxy.create_aster_orders_bulk(orders);
}

std::vector<Core::PolymarketBulkCancelResult>
Trading::Account::cancel_aster_orders_bulk(
    const std::vector<std::string>& order_ids, const std::string& symbol) {
    return this->ccxt_proxy.cancel_aster_orders_bulk(order_ids,
                                                      this->account_id, symbol);
}

std::vector<Core::PolymarketBulkOrderResult>
Trading::Account::create_hyperliquid_orders_bulk(
    const std::vector<Core::Order>& orders) {
    return this->ccxt_proxy.create_hyperliquid_orders_bulk(orders);
}

std::vector<Core::PolymarketBulkCancelResult>
Trading::Account::cancel_hyperliquid_orders_bulk(
    const std::vector<std::string>& order_ids, const std::string& symbol) {
    return this->ccxt_proxy.cancel_hyperliquid_orders_bulk(
        order_ids, this->account_id, symbol);
}

std::optional<
    std::pair<std::vector<Core::QuoteAsset>, std::vector<Core::Inventory>>>
Trading::Account::get_inventory_state() {
    return this->ccxt_proxy.get_inventory_state(
        this->exchange_name, this->market_type, this->account_id);
}
bool Trading::Account::symbol_exists(const std::string& symbol) {
    return this->active_symbols.find(symbol) != this->active_symbols.end();
}

std::vector<Core::OrderResNormalized>
Trading::Account::get_orders_on_symbol_cached(const std::string& symbol) {
    const auto it = this->last_orders_fetched_cache_update.find(symbol);
    if (it == this->last_orders_fetched_cache_update.end() ||
        std::chrono::steady_clock::now() - it->second >
            std::chrono::seconds(this->order_cache_duration_seconds)) {
        const auto& orders = this->get_orders_on_symbol(symbol);
        this->orders_fetched_cache[symbol] = orders;
        this->last_orders_fetched_cache_update[symbol] =
            std::chrono::steady_clock::now();
        return orders;
    } else {
        const auto it = this->orders_fetched_cache.find(symbol);
        if (it != this->orders_fetched_cache.end()) {
            return it->second;
        }
    }
    return {};
}

std::vector<Core::OrderResNormalized> Trading::Account::get_orders_on_symbol(
    const std::string& symbol) {
    const auto& orders = this->ccxt_proxy.get_orders_on_symbol(
        this->exchange_name, this->market_type, this->account_id, symbol);
    return orders;
}

std::vector<std::string> Trading::Account::get_active_symbols() {
    if (std::chrono::steady_clock::now() - this->last_active_symbols_fetch >
        std::chrono::seconds(60 * 5)) {
        const auto& infos =
            Core::ExchangeInfo::getInstance().get_exchange_symbols_info(
                this->exchange_name, this->market_type);

        std::cout << "Exchange name: " << this->exchange_name
                  << " Market type: " << this->market_type << std::endl;
        std::cout << "Infos size: " << infos.size() << std::endl;

        this->symbols_exchange_info = infos;
        this->active_symbols.clear();
        for (const auto& info : infos) {
            if (info.is_active) {
                this->active_symbols.insert(info.symbol);
            }
        }

        std::vector<std::string> symbols;
        for (const auto& info : infos) {
            if (info.is_active) {
                symbols.push_back(info.symbol);
            }
        }
        this->last_active_symbols_fetch = std::chrono::steady_clock::now();
        this->active_symbols_cache = symbols;
        return symbols;
    }
    return this->active_symbols_cache;
}

std::string Trading::Account::get_ohlcv_stream_url(
    std::vector<std::string> symbols, const std::string& interval) {
    const std::string& base_url = this->get_multi_stream_ws_base_url();

    if (this->type == ExchangeType::BINANCE_ACCOUNT) {
        std::ostringstream stream_path;
        for (size_t i = 0; i < symbols.size(); ++i) {
            std::string symbol_lower = symbols[i];
            std::transform(symbol_lower.begin(), symbol_lower.end(),
                           symbol_lower.begin(), ::tolower);
            stream_path << symbol_lower << "@kline_" << interval;
            if (i != symbols.size() - 1) {
                stream_path << "/";
            }
        }
        return base_url + "?streams=" + stream_path.str();
    }

    return {};
}

std::string Trading::Account::get_ws_base_url() {
    if (this->type == ExchangeType::BINANCE_ACCOUNT) {
        if (this->market_type == "swap") {
            return is_testnet
                       ? EnvUtils::env_or(
                             "BINANCE_ACCOUNT_WS_FUTURES_TESTNET_BASE_URL",
                             "wss://stream.binancefuture.com")
                       : EnvUtils::env_or("BINANCE_ACCOUNT_WS_FUTURES_BASE_URL",
                                          "wss://fstream.binance.com");
        } else {
            return is_testnet
                       ? EnvUtils::env_or(
                             "BINANCE_ACCOUNT_WS_TESTNET_BASE_URL",
                             "wss://testnet.binance.vision")
                       : EnvUtils::env_or("BINANCE_ACCOUNT_WS_BASE_URL",
                                          "wss://stream.binance.com:9443");
        }
    }

    return "";
}

std::string Trading::Account::get_multi_stream_ws_base_url() {
    if (this->type == ExchangeType::BINANCE_ACCOUNT) {
        return this->get_ws_base_url() + "/stream";
    }

    return "";
}

CcxtProxy::OrderLookup Trading::Account::query_invidual_order(
    const Core::Order& order) {
    return this->ccxt_proxy.lookup_order(order);
}

std::optional<std::string> Trading::Account::get_user_data_stream_url() {
    if (this->type == ExchangeType::BINANCE_ACCOUNT) {
    }
    return std::nullopt;
}

std::optional<std::string> Trading::Account::get_order_sub_msg() {
    if (this->type == ExchangeType::BINANCE_ACCOUNT) {
        return R"({"method": "SUBSCRIBE", "params": ["ORDER_TRADE_UPDATE"], "id": 1})";
    }
    return std::nullopt;
}

std::vector<Core::OrderResNormalized> Trading::Account::get_all_open_orders() {
    if (this->type == ExchangeType::BINANCE_ACCOUNT) {
    }
    return {};
}

void Trading::Account::cancel_order_by_client_order_id(
    const std::string& client_order_id) {
    if (this->type == ExchangeType::BINANCE_ACCOUNT) {
    }
}

void Trading::Account::cancel_order_by_client_order_id(
    const std::string& client_order_id, const std::string& exchange_name,
    const std::string& market_type, const std::string& symbol,
    const long long& account_id) {
    this->ccxt_proxy.cancel_order_v2(client_order_id, exchange_name,
                                     market_type, symbol, account_id);
}