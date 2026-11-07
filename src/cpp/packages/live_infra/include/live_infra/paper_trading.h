#pragma once

#include <optional>

#include "market_making/dtypes/enums.h"
#include "market_making/dtypes/trade.h"

namespace Live {

// Prices a paper order is judged against. Unset fields are unavailable or
// stale.
struct PaperQuote {
    std::optional<double> best_bid;
    std::optional<double> best_ask;
    std::optional<double> last;
    // Trade prices since the order's previous check.
    std::optional<double> trade_low;
    std::optional<double> trade_high;
};

struct PaperFill {
    double price;
    bool is_maker;
};

class PaperTrading {
   public:
    // An order fills only when the market crosses its limit. On the first
    // check after its ack it is the aggressor: it fills at the touch (or last
    // price plus slippage) if that is at or inside its limit, as a taker.
    // Later it is resting: it fills at its limit, as a maker, only once the
    // touch, the last price or a trade since the previous check is strictly
    // through it; a touch is not a fill.
    static std::optional<PaperFill> evaluate(Core::OrderSide side,
                                             double limit_price,
                                             bool is_first_check,
                                             const PaperQuote &quote,
                                             double slippage_bps);

    // A post-only order the exchange would reject: it would take on arrival.
    static bool rejects_post_only(Core::OrderSide side, double limit_price,
                                  const PaperQuote &quote);

    // Realized PnL of the closed part plus the remaining quantity marked at
    // `price`, before fees.
    static double mark_to_market_pnl(const Core::Trade &trade, double price);
};

}  // namespace Live
