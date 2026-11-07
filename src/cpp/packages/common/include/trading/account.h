#pragma once
#include <unordered_map>
#include <unordered_set>

#include "ccxt_proxy.h"
#include "market_making/dtypes/inventory.h"
#include "market_making/dtypes/order.h"
#include "market_making/dtypes/order_res.h"
#include "market_making/dtypes/quote_asset.h"
#include "optional"

namespace Trading {

using LoggingCallback = std::function<void(const std::string&)>;

enum MarketType {
    SPOT_MARKET_TYPE,
    SWAP_MARKET_TYPE,
};

enum ExchangeType {
    BINANCE_ACCOUNT,
    BYBIT_ACCOUNT,
    HYPERLIQUID_ACCOUNT,
    BITGET_ACCOUNT,
    GATEIO_ACCOUNT,
    POLYMARKET_ACCOUNT,
    ASTER_ACCOUNT,
    IBKR_ACCOUNT,
};

class Account {
   public:
    Account(MarketType market, ExchangeType type, long long account_id,
            const std::string& access, const std::string& secret,
            LoggingCallback info_callback, LoggingCallback error_callback);
    ~Account();

    std::optional<nlohmann::json> create_order(const Core::Order& order);

    std::vector<PolymarketDirect::PolymarketClient::SignedOrderBody>
    sign_polymarket_orders(const std::vector<Core::Order>& orders);

    std::vector<Core::PolymarketBulkOrderResult>
    submit_signed_polymarket_orders(
        const std::vector<PolymarketDirect::PolymarketClient::SignedOrderBody>
            &signed_orders,
        long long account_id);

    Core::PolymarketBulkOrderResult submit_signed_polymarket_order(
        const PolymarketDirect::PolymarketClient::SignedOrderBody& signed_order,
        long long account_id);
    std::string exchange_name;
    std::string market_type;

    std::optional<
        std::pair<std::vector<Core::QuoteAsset>, std::vector<Core::Inventory>>>
    get_inventory_state();

    std::vector<Core::OrderResNormalized> get_all_open_orders();

    std::vector<Core::OrderResNormalized> get_orders_on_symbol(
        const std::string& symbol);
    std::vector<Core::OrderResNormalized> get_orders_on_symbol_cached(
        const std::string& symbol);
    std::optional<Core::OrderResNormalized> query_invidual_order(
        const Core::Order& order);

    std::vector<std::string> get_active_symbols();
    std::string get_ohlcv_stream_url(std::vector<std::string> symbols,
                                     const std::string& interval);
    std::optional<std::string> get_user_data_stream_url();
    std::string get_multi_stream_ws_base_url();
    std::string get_ws_base_url();

    void cancel_order_by_client_order_id(const std::string& client_order_id);
    std::optional<nlohmann::json> cancel_order(const Core::Order& order);
    std::vector<Core::PolymarketBulkOrderResult>
    create_polymarket_orders_bulk(const std::vector<Core::Order>& orders);
    std::vector<Core::PolymarketBulkCancelResult>
    cancel_polymarket_orders_bulk(const std::vector<std::string>& order_ids);

    // Aster direct order routing
    std::vector<Core::PolymarketBulkOrderResult>
    create_aster_orders_bulk(const std::vector<Core::Order>& orders);
    std::vector<Core::PolymarketBulkCancelResult>
    cancel_aster_orders_bulk(const std::vector<std::string>& order_ids,
                             const std::string& symbol);

    // Hyperliquid direct order routing
    std::vector<Core::PolymarketBulkOrderResult>
    create_hyperliquid_orders_bulk(const std::vector<Core::Order>& orders);
    std::vector<Core::PolymarketBulkCancelResult>
    cancel_hyperliquid_orders_bulk(const std::vector<std::string>& order_ids,
                                   const std::string& symbol);
    void cancel_order_by_client_order_id(const std::string& client_order_id,
                                         const std::string& exchange_name,
                                         const std::string& market_type,
                                         const std::string& symbol,
                                         const long long& account_id);
    bool symbol_exists(const std::string& symbol);

    std::optional<std::string> get_order_sub_msg();

   private:
    MarketType market;
    ExchangeType type;
    long long account_id;
    std::string access;
    std::string secret;
    std::unordered_map<std::string, std::vector<Core::OrderResNormalized>>
        orders_fetched_cache;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point>
        last_orders_fetched_cache_update;
    std::chrono::steady_clock::time_point last_active_symbols_fetch =
        std::chrono::steady_clock::now() - std::chrono::seconds(60 * 60 * 24);

    std::vector<std::string> active_symbols_cache;
    int order_cache_duration_seconds = 1;
    LoggingCallback info_callback;
    LoggingCallback error_callback;
    CcxtProxy& ccxt_proxy;
    std::vector<Core::SymbolExchangeInfo> symbols_exchange_info;
    std::unordered_set<std::string> active_symbols;
    bool is_testnet;

    void init_account();
};
}  // namespace Trading
