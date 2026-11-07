#include "market_making/backtest_trade_ticker.h"

#include "market_making/backtest_state.h"

Core::BacktestTradeTicker::BacktestTradeTicker(
    std::vector<ExchangeAndMarket> exchange_and_markets, std::string symbol,
    std::string quote_asset, std::string base_asset, int strategy_id)
    : TradeTicker(exchange_and_markets, symbol, quote_asset, base_asset,
                  strategy_id) {}

Core::BacktestTradeTicker::~BacktestTradeTicker() {}

void Core::BacktestTradeTicker::on_create_order_impl(Engine& engine,
                                                     Order& order) {
    // in backtest we can simply instantly ack
    // in live we need to create and ack when exchange confirms it

    auto& backtest_state = ::Core::BacktestState::getInstance();

    order.valid_in_backtest_from_timestamp =
        order.create_timestamp + backtest_state.get_latency_to_exchange();

    backtest_state.add_order(order);
    engine.order_created_ack(order);

    if (backtest_state.is_sim_autofilling_orders()) {
        backtest_state.on_order_fill(order, order.price, order.create_timestamp,
                                     order.is_taker_in_simulation);
    }
}

void Core::BacktestTradeTicker::on_cancel_order_impl(Engine& engine,
                                                     Order& order) {
    // in backtest we can simply instantly ack
    // in live we need to create and ack if it is was actually cancelled
    auto& backtest_state = ::Core::BacktestState::getInstance();
    long long latency = backtest_state.get_latency_to_exchange();

    if (latency > 0) {
        backtest_state.mark_order_for_removal(
            order.exchange_name, order.market_type, order.symbol, order.id,
            engine.get_time() + latency);
        if (backtest_state.is_acking_cancels_at_exchange_removal()) {
            backtest_state.defer_cancel_ack(order);
            return;
        }
    } else {
        backtest_state.remove_order(order.exchange_name, order.market_type,
                                    order.symbol, order.id);
    }
    engine.order_canceled_ack(order);
}
