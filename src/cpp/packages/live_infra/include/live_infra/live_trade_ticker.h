#pragma once

#include "live_infra/state_updater.h"
#include "market_making/dtypes/def_only_structs.h"
#include "market_making/dtypes/order.h"
#include "market_making/ticker.h"

namespace Live {

struct OrderFrequencyLimits {
    int num_max_orders_10_sec;
    int num_max_orders_1_min;
    int num_max_orders_1_hour;
};

class LiveTradeTicker : public Core::TradeTicker {
   public:
    LiveTradeTicker(std::vector<Core::ExchangeAndMarket> exchange_and_markets,
                    std::string symbol, std::string quote_asset,
                    std::string base_asset, int strategy_id, StateUpdater& core,
                    OrderFrequencyLimits order_frequency_limits);

    ~LiveTradeTicker() = default;

    void on_create_order_impl(Core::Engine& engine,
                              Core::Order& order) override;
    void on_create_orders_bulk(Core::Engine& engine,
                               std::vector<Core::Order*>& orders) override;
    void on_cancel_orders_bulk(Core::Engine& engine,
                               std::vector<Core::Order*>& orders) override;
    void on_cancel_order_impl(Core::Engine& engine,
                              Core::Order& order) override;

   private:
    void handle_polymarket_order_result(
        Core::Engine& engine, Core::Order& order,
        const Core::PolymarketBulkOrderResult& result, long long now_ms);

    void create_polymarket_order_via_proxy(Core::Engine& engine,
                                           Core::Order& order);
    void create_polymarket_orders_bulk_via_proxy(
        Core::Engine& engine, std::vector<Core::Order*>& valid_orders,
        long long now_ms);

    void handle_aster_order_result(
        Core::Engine& engine, Core::Order& order,
        const Core::PolymarketBulkOrderResult& result, long long now_ms);

    // Generic ccxt (binance/bybit/gateio/etc.) order-creation response
    // handler. Uses the normalized OrderCreationResult shape — venue-specific
    // fields stay inside the ccxt wrapper layer.
    void handle_ccxt_order_result(Core::Engine& engine, Core::Order& order,
                                  const Core::OrderCreationResult& result);
    void create_aster_orders_bulk(
        Core::Engine& engine, std::vector<Core::Order*>& valid_orders,
        long long now_ms);
    void cancel_aster_orders_bulk(
        Core::Engine& engine, std::vector<Core::Order*>& orders);

    void handle_hyperliquid_order_result(
        Core::Engine& engine, Core::Order& order,
        const Core::PolymarketBulkOrderResult& result, long long now_ms);
    void create_hyperliquid_orders_bulk(
        Core::Engine& engine, std::vector<Core::Order*>& valid_orders,
        long long now_ms);
    void cancel_hyperliquid_orders_bulk(
        Core::Engine& engine, std::vector<Core::Order*>& orders);

    std::vector<std::chrono::steady_clock::time_point> new_orders_timestamps;
    StateUpdater& core;
    OrderFrequencyLimits order_frequency_limits;
};

}  // namespace Live