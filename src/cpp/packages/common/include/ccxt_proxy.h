#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "market_data/normalized_trade.h"
#include "market_making/dtypes/def_only_structs.h"
#include "rate_limiter.h"
#include "market_making/dtypes/inventory.h"
#include "market_making/dtypes/order.h"
#include "market_making/dtypes/order_res.h"
#include "market_making/dtypes/orderbook.h"
#include "market_making/dtypes/quote_asset.h"
#include "trading/aster_client.h"
#include "trading/hyperliquid_client.h"
#include "trading/polymarket_client.h"

namespace MarketData {

struct NormalizedParsedTrade;
struct OhlcvDatasetTick;

};  // namespace MarketData

class CcxtProxy {
   public:
    static CcxtProxy &instance() {
        static CcxtProxy instance;
        return instance;
    }

    CcxtProxy(const CcxtProxy &) = delete;
    CcxtProxy &operator=(const CcxtProxy &) = delete;

    // http_status: -1 not sent, 0 no response, else the proxy's HTTP status.
    std::optional<nlohmann::json> create_order(const Core::Order &order,
                                               int *http_status = nullptr);
    std::vector<Core::PolymarketBulkOrderResult> create_polymarket_orders_bulk(
        const std::vector<Core::Order> &orders);
    void cancel_order_v2(const std::string &client_order_id,
                         const std::string &exchange_name,
                         const std::string &market_type,
                         const std::string &symbol,
                         const long long &account_id);
    // Returns the proxy's structured cancel result ({"status":
    // "cancelled"|"gone"|"error", ...}) so callers can act on the round-trip
    // instead of waiting for the user-data WS. nullopt if the HTTP call failed
    // / was unparseable. Callers may ignore the return.
    std::optional<nlohmann::json> cancel_order(const Core::Order &order);
    std::vector<Core::PolymarketBulkCancelResult> cancel_polymarket_orders_bulk(
        const std::vector<std::string> &order_ids,
        long long account_id = 0);

    std::vector<PolymarketDirect::PolymarketClient::SignedOrderBody>
    sign_polymarket_orders(const std::vector<Core::Order> &orders);

    std::vector<Core::PolymarketBulkOrderResult> submit_signed_polymarket_orders(
        const std::vector<PolymarketDirect::PolymarketClient::SignedOrderBody>
            &signed_orders,
        long long account_id);

    Core::PolymarketBulkOrderResult submit_signed_polymarket_order(
        const PolymarketDirect::PolymarketClient::SignedOrderBody &signed_order,
        long long account_id);

    // Polymarket session heartbeat — delegates to the direct client. 2xx → true.
    bool send_polymarket_heartbeat(long long account_id);

    bool repay_loan(const std::string &exchange_name,
                    const std::string &market_type, const long long &account_id,
                    const std::string &symbol, const std::string &amount);

    std::vector<Core::SymbolExchangeInfo> get_exchange_symbols_info(
        const std::string &exchange_name, const std::string &market_type);

    std::vector<Core::SymbolPriceInfo> fetch_prices(
        const std::string &exchange_name, const std::string &market_type);

    std::optional<
        std::pair<std::vector<Core::QuoteAsset>, std::vector<Core::Inventory>>>
    get_inventory_state(const std::string &exchange_name,
                        const std::string &market_type,
                        const long long &account_id);

    std::vector<Core::OrderResNormalized> get_orders_on_symbol(
        const std::string &exchange_name, const std::string &market_type,
        const long long &account_id, const std::string &symbol);

    std::vector<Core::OrderResNormalized> get_open_orders_on_symbol(
        const std::string &exchange_name, const std::string &market_type,
        const long long &account_id, const std::string &symbol);

    std::vector<Core::OrderResNormalized> get_all_open_orders(
        const std::string &exchange_name, const std::string &market_type,
        long long account_id);

    std::optional<Core::OrderResNormalized> query_order(
        const std::string &exchange_name, const std::string &market_type,
        const long long &account_id, const std::string &symbol,
        const std::string &client_order_id);

    std::optional<Core::OrderResNormalized> query_order(
        const Core::Order &order);

    // NOT_FOUND only when the venue says the order does not exist (proxy 404);
    // transport errors, 5xx and rate limits are UNKNOWN.
    struct OrderLookup {
        enum class Status { FOUND, NOT_FOUND, UNKNOWN };
        Status status = Status::UNKNOWN;
        std::optional<Core::OrderResNormalized> order;
    };
    OrderLookup lookup_order(const std::string &exchange_name,
                             const std::string &market_type,
                             const long long &account_id,
                             const std::string &symbol,
                             const std::string &client_order_id);
    OrderLookup lookup_order(const Core::Order &order);

    std::vector<MarketData::NormalizedParsedTrade> get_historical_trades(
        const std::string &exchange_name, const std::string &market_type,
        const std::string &symbol, const long long &from_timestamp,
        RateLimiter::RequestClass request_class =
            RateLimiter::RequestClass::OTHER);

    std::vector<MarketData::NormalizedParsedTrade> get_historical_trades_by_id(
        const std::string &exchange_name, const std::string &market_type,
        const std::string &symbol, const std::string &trade_id,
        RateLimiter::RequestClass request_class =
            RateLimiter::RequestClass::OTHER);

    std::vector<std::string> get_exchange_symbols(
        const std::string &exchange_name, const std::string &market_type);

    // limit caps how many kline rows the proxy asks for in this request.
    // Default 1000 matches Binance's per-call max (and the historical
    // behavior). Callers doing real-time gap-fill should pass 100 — it
    // drops the per-call weight from 5 to 1, which matters when many
    // symbols fan out in parallel at every kline boundary.
    // An empty result means "no more data" OR a rate limit, a bad status, a
    // parse failure or an exception -- indistinguishable to the caller, which
    // is how a throttled backfill silently ends and is counted complete.
    // Pass `failed` to tell them apart.
    std::vector<MarketData::OhlcvDatasetTick> get_ohlcv_dataset_ticks(
        const std::string &exchange_name, const std::string &market_type,
        const std::string &symbol, const std::string &interval,
        const long long &from_timestamp,
        int limit = 1000,
        RateLimiter::RequestClass request_class =
            RateLimiter::RequestClass::OTHER,
        bool *failed = nullptr);

    std::vector<std::string> get_exchange_active_symbols(
        const std::string &exchange_name, const std::string &market_type);

    bool call_stream_instance_ip_rotation_check(
        const std::string &exchange_name, const std::string &market_type);

    bool call_base_instance_ip_rotation_check(const std::string &exchange_name,
                                              const std::string &market_type);

    bool is_symbol_active(const std::string &exchange_name,
                          const std::string &market_type,
                          const std::string &symbol);

    void update_swaps_leverage_level(const std::string &exchange_name,
                                     const std::string &market_type,
                                     const long long &account_id,
                                     const std::string &symbol,
                                     const int &leverage_level);

    std::vector<Core::AssetBorrowableInfo> fetch_borrowable_inventories(
        const std::string &exchange_name, const std::string &market_type,
        const long long &account_id, const std::vector<std::string> &symbols);

    std::optional<Core::OrderBookLevels> get_full_orderbook(
        const std::string &exchange_name, const std::string &market_type,
        const std::string &symbol, int limit = 1000);

    struct MintContractsResult {
        bool success = false;
        std::string tx_hash;
        std::string error;
        int gas_used = 0;
    };

    MintContractsResult mint_prediction_market_contracts(
        const std::string &condition_id, bool neg_risk, int num_sets,
        long long account_id = 0);

    struct RedeemContractsResult {
        bool success = false;
        std::string status;
        std::string tx_hash;
        std::string error;
        double usdc_gained = 0;
    };

    RedeemContractsResult redeem_prediction_market_contracts(
        const std::string &condition_id, bool neg_risk,
        const std::string &slug, long long account_id = 0);

    void subscribe_polymarket_stream(
        const std::vector<std::string> &condition_ids);

    std::vector<Core::OrderResNormalized> get_polymarket_all_open_orders(
        long long account_id = 0);

    // Initialize direct Polymarket signing (fetches credentials from Python proxy)
    void init_polymarket_direct_signing();

    // Check if direct signing is enabled and initialized
    bool is_polymarket_direct_signing_enabled() const;

    // Check if Polymarket client is in 429 rate-limit cooldown
    bool is_polymarket_rate_limited() const;

    // Accessor for direct client (used by AccountStreamManager for listenKey)
    AsterDirect::AsterClient* get_aster_client();

    // Initialize direct Aster signing (fetches credentials from Python proxy)
    void init_aster_direct_signing();

    // Check if direct Aster signing is enabled
    bool is_aster_direct_signing_enabled() const;

    // Aster order routing
    std::vector<Core::PolymarketBulkOrderResult> create_aster_orders_bulk(
        const std::vector<Core::Order>& orders);
    std::vector<Core::PolymarketBulkCancelResult> cancel_aster_orders_bulk(
        const std::vector<std::string>& order_ids,
        long long account_id, const std::string& symbol);

    // Accessor for direct client (used by AccountStreamManager for WS subs)
    HyperliquidDirect::HyperliquidClient* get_hyperliquid_client();

    // Initialize direct Hyperliquid signing (fetches credentials from proxy)
    void init_hyperliquid_direct_signing();

    bool is_hyperliquid_direct_signing_enabled() const;

    // Hyperliquid order routing
    std::vector<Core::PolymarketBulkOrderResult> create_hyperliquid_orders_bulk(
        const std::vector<Core::Order>& orders);
    std::vector<Core::PolymarketBulkCancelResult>
    cancel_hyperliquid_orders_bulk(const std::vector<std::string>& order_ids,
                                   long long account_id,
                                   const std::string& symbol);

   private:
    CcxtProxy();
    ~CcxtProxy();

    std::string base_url;
    std::string stream_instance_url;
    std::unordered_map<std::string, std::unordered_set<std::string>>
        active_symbols_by_exchange_and_market_type;
    std::chrono::steady_clock::time_point last_active_symbols_update_time{
        std::chrono::steady_clock::now() - std::chrono::minutes(1200)};

    // Direct Polymarket signing client (bypasses Python proxy)
    std::optional<PolymarketDirect::PolymarketClient> polymarket_client_;
    bool polymarket_direct_enabled_ = false;

    // Direct Aster signing client (bypasses Python proxy)
    std::optional<AsterDirect::AsterClient> aster_client_;
    bool aster_direct_enabled_ = false;

    // Direct Hyperliquid signing client (bypasses Python proxy)
    std::optional<HyperliquidDirect::HyperliquidClient> hyperliquid_client_;
    bool hyperliquid_direct_enabled_ = false;

    std::pair<int, std::string> http_req(
        const std::string &url, const std::string &method,
        const std::string &body,
        const std::vector<std::string> &headers = {});
    std::vector<std::string> build_proxy_auth_headers() const;

    // Reactive rate-limit helpers for read/poll endpoints. rate_blocked() is
    // the "is this exchange:market in cooldown — bail" check; note_rate_limit()
    // engages the cooldown when a response is 429/418. Every external GET that
    // can be retried later should call both so a binance:swap ban pauses the
    // whole polling surface instead of hammering through it.
    bool rate_blocked(const std::string &exchange_name,
                      const std::string &market_type) const;
    void note_rate_limit(const std::string &exchange_name,
                         const std::string &market_type, int status,
                         const std::string &body);
};
