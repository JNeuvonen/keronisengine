#include "strategies/common/order_posting_policy.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace OrderPosting {

namespace {

constexpr double kQtyEpsilon = 1e-12;
constexpr std::uint64_t kGapSalt = 1;
constexpr std::uint64_t kClipSalt = 2;
constexpr std::uint64_t kTtlSalt = 3;

std::uint64_t splitmix64(std::uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

bool same_price(double a, double b) {
    return std::abs(a - b) <= 1e-9 * std::max(std::abs(a), std::abs(b));
}

std::optional<double> touch_of(Side side, const Quote& q) {
    const auto price = side == Side::BUY ? q.ask : q.bid;
    return price ? price : q.last;
}

std::optional<double> join_of(Side side, const Quote& q) {
    const auto price = side == Side::BUY ? q.bid : q.ask;
    return price ? price : q.last;
}

// A post at `price` would take liquidity.
bool crosses(Side side, double price, const Quote& q) {
    const auto opposite = touch_of(side, q);
    if (!opposite) return false;
    return side == Side::BUY ? price >= *opposite : price <= *opposite;
}

double moved(Side side, double price, double bps) {
    return side == Side::BUY ? price * (1 + bps / 1e4) : price * (1 - bps / 1e4);
}

double tighter(Side side, double price, std::optional<double> limit) {
    if (!limit) return price;
    return side == Side::BUY ? std::min(price, *limit) : std::max(price, *limit);
}

bool within(Side side, double price, double bound) {
    return side == Side::BUY ? price <= bound : price >= bound;
}

}  // namespace

double OrderPostingPolicy::draw(std::uint64_t seed, std::uint64_t a,
                                std::uint64_t b, double lo, double hi) {
    if (hi <= lo) return hi;
    const auto bits = splitmix64(seed ^ splitmix64(a) ^ splitmix64(b * 0x632be59bd9b4e019ULL));
    const double unit = static_cast<double>(bits >> 11) * 0x1.0p-53;
    return lo + unit * (hi - lo);
}

double OrderPostingPolicy::ladder_price(Side side, const Ladder& ladder, int rung) {
    const double offset = ladder.step_bp * rung / 1e4;
    return side == Side::SELL ? ladder.first * (1 + offset) : ladder.first * (1 - offset);
}

namespace {

// What every resting rung sits at is fixed by the ladder; a live order is the
// rung whose price it rests at, so a move by whole steps keeps the overlap.
Plan plan_ladder(const Policy& policy, const Ladder& ladder, const Input& in) {
    Plan plan;
    plan.next_rung = in.next_rung;
    const int rungs = std::max(1, ladder.rungs);
    std::vector<double> prices(rungs);
    for (int k = 0; k < rungs; ++k) prices[k] = in.round(OrderPostingPolicy::ladder_price(in.side, ladder, k));

    std::set<unsigned long long> cancelled;
    auto cancel = [&](const LiveOrder& order) {
        if (order.cancel_sent || !cancelled.insert(order.id).second) return;
        plan.cancels.push_back(order.id);
    };
    std::vector<const LiveOrder*> at(rungs, nullptr);
    for (const auto& order : in.live) {
        if (order.cancel_sent) continue;
        const auto rung = std::find_if(prices.begin(), prices.end(),
                                       [&](double price) { return same_price(order.price, price); });
        const auto k = static_cast<int>(rung - prices.begin());
        if (rung == prices.end() || at[k]) {
            cancel(order);
        } else {
            at[k] = &order;
        }
    }

    // Shed the farthest rungs when the program shrank under them.
    double resting = 0;
    for (const auto* order : at) {
        if (order) resting += order->qty_left;
    }
    for (int k = rungs - 1; k >= 0 && resting > in.remaining_qty + kQtyEpsilon; --k) {
        if (!at[k]) continue;
        cancel(*at[k]);
        resting -= at[k]->qty_left;
        at[k] = nullptr;
    }

    while (plan.next_rung < rungs && at[plan.next_rung]) ++plan.next_rung;

    // Everything on the book can still fill, cancels in flight included.
    double exposed = 0;
    for (const auto& order : in.live) exposed += order.qty_left;
    double available = in.remaining_qty - exposed;
    double planned = available + in.incoming_qty;
    if (available <= kQtyEpsilon || !in.quote.last) return plan;
    int on_book = static_cast<int>(in.live.size() - cancelled.size());

    const bool opening = in.next_rung == 0;
    const bool paced = !opening && policy.gap_hi_ms > 0;
    if (paced && in.last_post_ms > 0) {
        const auto gap = OrderPostingPolicy::draw(in.seed, static_cast<std::uint64_t>(in.last_post_ms), kGapSalt,
                                                  static_cast<double>(policy.gap_lo_ms),
                                                  static_cast<double>(policy.gap_hi_ms));
        if (in.now_ms - in.last_post_ms < gap) return plan;
    }
    int unposted = 0;
    const int first = plan.next_rung;
    for (int k = first; k < rungs; ++k) unposted += at[k] ? 0 : 1;
    for (int k = first; k < rungs; ++k) {
        if (at[k]) {
            plan.next_rung = k + 1;
            continue;
        }
        if (on_book >= ladder.visible) break;
        // The market is through this rung: its share moves to the deeper ones.
        if (crosses(in.side, prices[k], in.quote)) {
            plan.next_rung = k + 1;
            --unposted;
            continue;
        }
        double want = planned;
        if (unposted > 1) {
            const auto salt = static_cast<std::uint64_t>(first) * 1024 + static_cast<std::uint64_t>(k);
            const double jitter = ladder.size_jitter * (2 * OrderPostingPolicy::draw(in.seed, salt, kClipSalt, 0, 1) - 1);
            want = std::max(planned / unposted * (1 + jitter), in.min_qty);
        }
        // A rung waits for the entry to fill enough to cover it.
        if (want > available + kQtyEpsilon && in.incoming_qty > kQtyEpsilon) break;
        const double qty = std::min(want, available);
        if (qty < in.min_qty - kQtyEpsilon) break;
        plan.posts.push_back({prices[k], qty, k, 0});
        plan.next_rung = k + 1;
        available -= qty;
        planned -= qty;
        --unposted;
        ++on_book;
        if (paced || available <= kQtyEpsilon) break;
    }

    // Size beyond the laid-out rungs (an add after the last was posted) goes to
    // the farthest, re-posted larger.
    if (plan.posts.empty() && plan.next_rung >= rungs && available > kQtyEpsilon) {
        const int last = rungs - 1;
        if (at[last]) {
            if (!crosses(in.side, prices[last], in.quote)) cancel(*at[last]);
        } else if (!crosses(in.side, prices[last], in.quote) &&
                   available >= in.min_qty - kQtyEpsilon) {
            plan.posts.push_back({prices[last], available, last, 0});
        }
    }
    return plan;
}

}  // namespace

Plan OrderPostingPolicy::plan(const Policy& policy, const Input& in) {
    if (const auto* ladder = std::get_if<Ladder>(&policy.price)) return plan_ladder(policy, *ladder, in);
    Plan plan;
    const auto* touch = std::get_if<Touch>(&policy.price);
    const auto* marketable = std::get_if<Marketable>(&policy.price);

    // The price every order should sit at now; nullopt holds posting.
    std::optional<double> target;
    if (touch) {
        target = join_of(in.side, in.quote);
        if (target && touch->limit && !within(in.side, *target, *touch->limit)) target.reset();
    } else if (marketable) {
        const auto reference = touch_of(in.side, in.quote);
        double anchor = marketable->anchor;
        if (reference && !within(in.side, *reference, moved(in.side, anchor, marketable->cap_bps))) {
            if (marketable->reanchor) {
                anchor = *reference;
                plan.anchor = anchor;
            } else {
                plan.beyond_cap = true;
            }
        }
        const double bound = tighter(in.side, moved(in.side, anchor, marketable->cap_bps), marketable->limit);
        if (!plan.beyond_cap && reference && within(in.side, *reference, bound)) target = bound;
    }
    if (target) target = in.round(*target);

    std::set<unsigned long long> cancelled;
    auto cancel = [&](const LiveOrder& order) {
        if (order.cancel_sent || !cancelled.insert(order.id).second) return;
        plan.cancels.push_back(order.id);
    };
    for (const auto& order : in.live) {
        if (order.expires_ms > 0 && in.now_ms >= order.expires_ms) {
            cancel(order);
        } else if (touch) {
            if (!target || (!same_price(order.price, *target) &&
                            in.now_ms - order.placed_ms >= touch->reprice_ms)) {
                cancel(order);
            }
        } else if (!target || !same_price(order.price, *target)) {
            cancel(order);
        }
    }

    // Shed the newest orders when the program shrank under them.
    double resting = 0;
    for (const auto& order : in.live) {
        if (!cancelled.contains(order.id) && !order.cancel_sent) resting += order.qty_left;
    }
    if (resting > in.remaining_qty + kQtyEpsilon) {
        std::vector<const LiveOrder*> newest;
        for (const auto& order : in.live) {
            if (!cancelled.contains(order.id) && !order.cancel_sent) newest.push_back(&order);
        }
        std::sort(newest.begin(), newest.end(), [](const auto* a, const auto* b) { return a->placed_ms > b->placed_ms; });
        for (const auto* order : newest) {
            if (resting <= in.remaining_qty + kQtyEpsilon) break;
            cancel(*order);
            resting -= order->qty_left;
        }
    }

    // Everything on the book can still fill, cancels in flight included.
    double exposed = 0;
    for (const auto& order : in.live) exposed += order.qty_left;
    double available = in.remaining_qty - exposed;
    if (available <= kQtyEpsilon || plan.beyond_cap || !target) return plan;

    const bool paced = policy.gap_hi_ms > 0;
    if (paced && in.last_post_ms > 0) {
        const auto gap = draw(in.seed, static_cast<std::uint64_t>(in.last_post_ms), kGapSalt,
                              static_cast<double>(policy.gap_lo_ms), static_cast<double>(policy.gap_hi_ms));
        if (in.now_ms - in.last_post_ms < gap) return plan;
    }

    const bool occupied = std::any_of(in.live.begin(), in.live.end(), [&](const LiveOrder& order) {
        return same_price(order.price, *target);
    });
    // The engine refuses a second order at a price we already rest at.
    if (occupied || policy.max_live - static_cast<int>(in.live.size()) <= 0) return plan;

    const auto salt = static_cast<std::uint64_t>(in.now_ms) * 64;
    double qty = available;
    if (policy.clip_hi_usd > 0) {
        qty = std::min(qty, draw(in.seed, salt, kClipSalt, policy.clip_lo_usd, policy.clip_hi_usd) / *target);
    }
    const long long expires = policy.ttl_hi_ms > 0
        ? in.now_ms + static_cast<long long>(draw(in.seed, salt, kTtlSalt, static_cast<double>(policy.ttl_lo_ms),
                                                  static_cast<double>(policy.ttl_hi_ms)))
        : 0;
    plan.posts.push_back({*target, qty, 0, expires});
    return plan;
}

}  // namespace OrderPosting
