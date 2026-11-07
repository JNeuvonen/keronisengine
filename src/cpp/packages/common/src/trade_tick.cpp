#include "trade_tick.h"

nlohmann::json TradeTick::to_json() const {
    return {{"timestamp", timestamp},
            {"price", price},
            {"quantity", quantity},
            {"trade_id", trade_id},
            {"is_buyer_maker", is_buyer_maker}};
}