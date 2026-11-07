#pragma once
//
// Desired-state order posting for one program: a take-profit ladder, a
// stop-out, an execution's slices. Given what is resting and what is left to
// do, it returns the cancels and posts that converge on the policy. It is pure
// and seeded, so a backtest runs exactly what live runs; the caller turns the
// plan into engine orders.
//
#include <cstdint>
#include <functional>
#include <optional>
#include <variant>
#include <vector>

namespace OrderPosting {

enum class Side { BUY, SELL };

struct Quote {
    std::optional<double> bid;
    std::optional<double> ask;
    std::optional<double> last;
};

// An exit spread over `rungs` resting limits on the far side of the market:
// rung k sits k * step_bp past `first`, up for a sell and down for a buy. At
// most `visible` rest at once, nearest first, however far from the price: a
// venue's price band bounds only the aggressive side. Each rung draws its even share of what is left, times
// 1 +- size_jitter. Rungs before the input's next_rung that are not live are
// done and never posted again.
struct Ladder {
    double first = 0;
    double step_bp = 0;
    int rungs = 1;
    int visible = 50;
    double size_jitter = 0;
};

// Rests on our own side of the book, re-priced once an order is reprice_ms
// old and the touch has moved.
struct Touch {
    long long reprice_ms = 5000;
    std::optional<double> limit;
};

// Takes liquidity up to `anchor` moved cap_bps against us, tightened by
// `limit`. When the market leaves the cap, a re-anchoring rule follows it;
// otherwise the plan reports beyond_cap.
struct Marketable {
    double anchor = 0;
    double cap_bps = 50;
    bool reanchor = false;
    std::optional<double> limit;
};

struct Policy {
    std::variant<Ladder, Touch, Marketable> price;
    // Notional of one order, drawn per order; a zero upper bound sends the
    // whole remainder as one order.
    double clip_lo_usd = 0;
    double clip_hi_usd = 0;
    // Resting orders at once, for touch and marketable rules.
    int max_live = 1;
    // Wait between new posts, drawn per post; a ladder's opening batch posts at once.
    long long gap_lo_ms = 0;
    long long gap_hi_ms = 0;
    // Lifetime of a resting order, drawn per order; zero rests until filled
    // or re-priced.
    long long ttl_lo_ms = 0;
    long long ttl_hi_ms = 0;
};

struct LiveOrder {
    unsigned long long id = 0;
    double price = 0;
    double qty_left = 0;
    int level = 0;
    long long placed_ms = 0;
    long long expires_ms = 0;
    bool cancel_sent = false;
};

struct Input {
    Side side = Side::SELL;
    // What this program still has to work, before subtracting live orders.
    double remaining_qty = 0;
    // Ladder: size still arriving (an entry filling), laid out but posted
    // only once filled; the first rung not yet posted or skipped; and the
    // venue's smallest order.
    double incoming_qty = 0;
    int next_rung = 0;
    double min_qty = 0;
    Quote quote;
    long long now_ms = 0;
    long long last_post_ms = 0;
    std::uint64_t seed = 0;
    std::vector<LiveOrder> live;
    // The venue's price rounding; targets are compared after it, so a ladder
    // never churns over a sub-tick difference.
    std::function<double(double)> round = [](double price) { return price; };
};

struct Post {
    double price = 0;
    double qty = 0;
    int level = 0;
    long long expires_ms = 0;
};

struct Plan {
    std::vector<Post> posts;
    std::vector<unsigned long long> cancels;
    // A re-anchoring Marketable moved its anchor here.
    std::optional<double> anchor;
    // A ladder's first rung not yet posted or skipped.
    int next_rung = 0;
    bool beyond_cap = false;
};

class OrderPostingPolicy {
   public:
    static Plan plan(const Policy& policy, const Input& input);

    static double ladder_price(Side side, const Ladder& ladder, int rung);

    // Deterministic draw in [lo, hi] from the seed and two salts.
    static double draw(std::uint64_t seed, std::uint64_t a, std::uint64_t b,
                       double lo, double hi);
};

}  // namespace OrderPosting
