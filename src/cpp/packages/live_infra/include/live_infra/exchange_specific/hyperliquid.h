#pragma once

#include <functional>
#include <string>
#include <vector>

namespace Core {
struct OrderResNormalized;
}

namespace Live::HyperliquidExchange {

// Parses a Hyperliquid `orderUpdates` WS message into order-state updates.
// HL batches many updates per message, hence a vector (empty for other
// channels / subscription acks). filled_quantity is cumulative (origSz - sz).
std::vector<Core::OrderResNormalized> parse_order_trade_update(
    const std::string& msg);

void init_and_register_account_streams(
    std::function<void(const Core::OrderResNormalized&)> on_order_update);

}  // namespace Live::HyperliquidExchange
