#pragma once

#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "market_data/depth_history.h"
#include "market_making/dtypes/enums.h"
#include "market_making/dtypes/order.h"
#include "market_making/dtypes/trade.h"
#include "nlohmann/json.hpp"

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

enum class PaperDepthOutcome {
    FILLED,
    PARTIAL_CANCELED,   // IOC/market remainder canceled
    PARTIAL_RESTING,    // GTC remainder rests
    RESTING,            // GTC/post-only nothing crossed
    NO_LIQUIDITY,       // IOC/market nothing within the limit; canceled
    FOK_UNFILLABLE,     // FOK not fully walkable; canceled
    POST_ONLY_REJECTED, // post-only would have crossed; canceled
    STALE_BOOK,         // no trustworthy book at the fill time
};

struct PaperFillSlice {
    double price;
    double quantity;
};

struct PaperDepthFill {
    PaperDepthOutcome outcome = PaperDepthOutcome::RESTING;
    std::vector<PaperFillSlice> slices;  // one per book level taken
    double filled_quantity = 0;
    double vwap = 0;
    int levels_consumed = 0;
    // Ran out of published levels with quantity left inside the limit; the
    // real book may have had more.
    bool depth_exhausted = false;
    // The unfilled remainder is canceled rather than left resting.
    bool cancels_remainder = false;
};

struct PaperModeSwitch {
    std::vector<int> refused;      // live -> paper with saved orders or trades
    std::set<int> paper_to_live;   // paper -> live with saved orders or trades
    nlohmann::json modes;          // every strategy's mode to keep for the next start
};

class PaperTrading {
   public:
    // The trade fully closed at `price`: the open remainder at that price,
    // pnl net of the fees it already paid. Closes the paper book of a strategy
    // switched to live.
    static Core::Trade close_at(const Core::Trade &trade, double price,
                                long long now_ms);
    // Compares each strategy's mode (id, is_paper) with the one kept from the
    // last start. A switch matters only when saved state holds its orders or trades.
    static PaperModeSwitch plan_mode_switches(
        const nlohmann::json &kept_modes,
        const std::vector<std::pair<int, bool>> &strategies,
        const std::vector<Core::Order> &orders,
        const std::vector<Core::Trade> &trades);
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

    // The aggressor check against a depth snapshot: walks the opposite side
    // up to `limit_price` (no limit for MARKET). IOC/MARKET cancel any
    // remainder, FOK fills fully or not at all, post-only rests or is
    // rejected if it would cross, GTC rests the remainder.
    static PaperDepthFill evaluate_against_depth(
        Core::OrderSide side, Core::OrderType type, bool post_only,
        double limit_price, double quantity,
        const MarketData::DepthSnapshot &book);

    // Outcome when there is no usable book: orders that must not rest are
    // canceled, the rest keep resting.
    static PaperDepthFill stale_book(Core::OrderType type, bool post_only);

    static const char *outcome_name(PaperDepthOutcome outcome);

    // Realized PnL of the closed part plus the remaining quantity marked at
    // `price`, before fees.
    static double mark_to_market_pnl(const Core::Trade &trade, double price);
};

}  // namespace Live
