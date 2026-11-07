#pragma once

#include "datetime_utils.h"
#include "market_making/dtypes/order.h"
#include "market_making/dtypes/order_fill_info.h"
#include "market_making/dtypes/trade.h"
#include "uuid.h"

namespace Core::PolymarketOrderUtils {

inline Order create_polymarket_mint_order(const std::string& condition_id,
                                          const std::string& clob_id,
                                          int strategy_id, long long account_id,
                                          long long timestamp, double price,
                                          double quantity,
                                          double sizing_coeff_delta = 1.0) {
    Order order;
    order.type = OrderType::MARKET;
    order.side = OrderSide::BUY;
    order.symbol = condition_id;
    order.exchange_name = "polymarket";
    order.market_type = "spot";
    order.quote_asset = "USDC";
    order.base_asset = "OUTCOME";
    order.price = price;
    order.quantity = quantity;
    order.filled_quantity = 0;
    order.sizing_coeff_delta = sizing_coeff_delta;
    order.id = Common::generate_timestamp_uuid();
    order.trade_id = std::nullopt;
    order.strategy_id = strategy_id;
    order.account_id = account_id;
    order.create_timestamp = timestamp;
    order.expires_on_ms = DatetimeUtils::get_current_time_ms() + 86400000;
    order.is_trade_close_order = false;
    order.is_taker_in_simulation = true;
    order.is_prediction_market_order = true;
    order.is_order_creation_acknowledged = true;
    order.clob_id = clob_id;
    order.type_internal = OrderTypeInternal::SYNTHETIC_CONTRACT_MINT;
    return order;
}

inline Order create_polymarket_settlement_order(const Trade& trade,
                                                double settlement_price,
                                                long long settlement_time_ms) {
    mpfr::mpreal remaining = trade.open_quantity - trade.closed_quantity;

    OrderSide exit_side =
        (trade.type == TradeType::LONG) ? OrderSide::SELL : OrderSide::BUY;

    Order order;
    order.type = OrderType::MARKET;
    order.side = exit_side;
    order.symbol = trade.symbol;
    order.exchange_name = trade.exchange_name;
    order.market_type = trade.market_type;
    order.quote_asset = "USDC";
    order.base_asset = "OUTCOME";
    order.price = settlement_price;
    order.quantity = remaining;
    order.filled_quantity = 0;
    order.sizing_coeff_delta = trade.sizing_coeff_delta;
    order.id = Common::generate_timestamp_uuid();
    order.trade_id = trade.id;
    order.strategy_id = trade.strategy_id;
    order.account_id = trade.account_id;
    order.create_timestamp = settlement_time_ms;
    order.expires_on_ms = 0;
    order.is_trade_close_order = true;
    order.is_taker_in_simulation = true;
    order.is_prediction_market_order = true;
    order.clob_id = trade.clob_token_id;
    order.type_internal = OrderTypeInternal::SYNTHETIC_CONTRACT_SETTLEMENT;
    return order;
}

inline OrderFillInfo create_polymarke_synthethic_fill(const Order& order,
                                                      double price,
                                                      long long timestamp) {
    OrderFillInfo fill;
    fill.symbol = order.symbol;
    fill.exchange_name = order.exchange_name;
    fill.market_type = order.market_type;
    fill.price = price;
    fill.fill_amount = order.quantity;
    fill.orig_order_full_size = order.quantity;
    fill.orig_order_sizing_coeff_delta = order.sizing_coeff_delta;
    fill.account_id = order.account_id;
    fill.side = order.side;
    fill.strategy_id = order.strategy_id;
    fill.order_id = order.id;
    fill.order_fill_id = Common::generate_timestamp_uuid();
    fill.order_created_timestamp = order.create_timestamp;
    fill.order_fill_timestamp = timestamp;
    fill.sizing_coeff_delta = order.sizing_coeff_delta;
    fill.prediction = order.prediction;
    fill.trade_id = order.trade_id;
    fill.clob_token_id = order.clob_id;
    fill.kv_cache = order.kv_cache;
    fill.trade_creation_type = trade_creation_type_for(order.type_internal);
    return fill;
}

}  // namespace Core::PolymarketOrderUtils
