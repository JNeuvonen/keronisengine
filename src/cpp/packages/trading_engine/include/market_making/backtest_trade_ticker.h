#pragma once

#include "market_making/dtypes/order.h"
#include "market_making/engine.h"
#include "market_making/ticker.h"

namespace Core {

class BacktestTradeTicker : public TradeTicker {
   public:
    BacktestTradeTicker(std::vector<ExchangeAndMarket> exchange_and_markets,
                        std::string symbol, std::string quote_asset,
                        std::string base_asset, int strategy_id);
    ~BacktestTradeTicker();

    void on_create_order_impl(Engine& engine, Order& order) override;
    void on_cancel_order_impl(Engine& engine, Order& order) override;
};

}  // namespace Core
