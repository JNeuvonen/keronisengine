#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

#include "strategies/common/order_posting_policy.h"
#include "strategies/discretionary/commands.h"

namespace Discretionary {

struct Quote {
    std::optional<double> bid;
    std::optional<double> ask;
    std::optional<double> last;
};

struct AccumulationPlan {
    long long start_ms = 0;
    long long ttl_ms = 0;
    double clip_usd = 0;
    double clip_jitter = 0;
    long long every_ms = 0;
    double every_jitter = 0;
    std::uint64_t seed = 0;
};

class ExecutionMath {
    static constexpr std::uint64_t kClipSalt = 21;
    static constexpr std::uint64_t kGapSalt = 22;

    static double unit(std::uint64_t seed, int k, std::uint64_t salt) {
        return OrderPosting::OrderPostingPolicy::draw(seed, static_cast<std::uint64_t>(k), salt, 0, 1);
    }

    static std::string dollars(double value) {
        auto digits = std::to_string(static_cast<long long>(std::round(value)));
        for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3) digits.insert(i, ",");
        return digits;
    }

   public:
    // A quantity under half the venue's size step rounds to nothing, so it is
    // flat: summed fills leave float dust like 7e-12 that no order can carry.
    // The floor is the engine's own tolerance for closing a trade.
    static bool is_dust(double qty, double step_size) {
        return qty <= std::max(1e-10, step_size / 2);
    }

    // The price a buyer pays now (ask, else last) or a seller gets (bid,
    // else last).
    static std::optional<double> touch(Side side, const Quote& q) {
        const auto price = side == Side::BUY ? q.ask : q.bid;
        return price ? price : q.last;
    }

    // Worst acceptable price for a taking order: the arrival price moved by
    // the cap, tightened by the operator's limit.
    static double bound(Side side, double arrival, double cap_bps,
                        std::optional<double> limit) {
        const double capped = side == Side::BUY ? arrival * (1 + cap_bps / 1e4)
                                                : arrival * (1 - cap_bps / 1e4);
        if (!limit) return capped;
        return side == Side::BUY ? std::min(capped, *limit)
                                 : std::max(capped, *limit);
    }

    static bool within(Side side, double price, double bound) {
        return side == Side::BUY ? price <= bound : price >= bound;
    }

    // Passive orders rest on our own side of the book.
    static std::optional<double> join(Side side, const Quote& q) {
        const auto price = side == Side::BUY ? q.bid : q.ask;
        return price ? price : q.last;
    }

    // True when a limit at `price` would take liquidity.
    static bool crosses(Side side, double price, const Quote& q) {
        const auto opposite = side == Side::BUY ? q.ask : q.bid;
        const auto reference = opposite ? opposite : q.last;
        if (!reference) return false;
        return side == Side::BUY ? price >= *reference : price <= *reference;
    }

    // Clip 0 goes at the start; each later clip follows the one before by
    // every_ms * [1 - every_jitter, 1 + every_jitter]. Draws are seeded, so a
    // resumed program keeps its rhythm.
    static long long accumulate_release_ms(const AccumulationPlan& p, int k) {
        double t = static_cast<double>(p.start_ms);
        for (int i = 1; i <= k; ++i) t += p.every_ms * (1 + p.every_jitter * (2 * unit(p.seed, i, kGapSalt) - 1));
        return static_cast<long long>(t);
    }

    // The latest clip released at or before `now_ms` and before the program
    // ends; -1 before the start.
    static int accumulate_clip_at(const AccumulationPlan& p, long long now_ms) {
        const long long end = std::min(now_ms, p.start_ms + p.ttl_ms - 1);
        if (end < p.start_ms) return -1;
        int k = 0;
        double t = static_cast<double>(p.start_ms);
        while (true) {
            t += p.every_ms * (1 + p.every_jitter * (2 * unit(p.seed, k + 1, kGapSalt) - 1));
            if (static_cast<long long>(t) > end) return k;
            ++k;
        }
    }

    static double clip_factor(const AccumulationPlan& p, int k) {
        return 1 + p.clip_jitter * (2 * unit(p.seed, k, kClipSalt) - 1);
    }

    static double accumulate_clip_usd(const AccumulationPlan& p, int k) { return p.clip_usd * clip_factor(p, k); }

    static std::optional<std::string> clip_problem(double clip_usd, double clip_jitter, double min_notional) {
        const double smallest = clip_usd * (1 - clip_jitter);
        if (min_notional <= 0 || smallest >= min_notional) return std::nullopt;
        return "smallest clip $" + dollars(smallest) + " is under the venue minimum $" + dollars(min_notional);
    }

    // Fat-finger checks: why a command is refused, or nullopt.
    static std::optional<std::string> notional_problem(double qty, double price) {
        const double notional = qty * price;
        if (notional <= kMaxCommandNotionalUsd) return std::nullopt;
        return "$" + dollars(notional) + " is above the $" + dollars(kMaxCommandNotionalUsd) + " per-command limit";
    }

    static std::optional<std::string> cross_problem(Side side, double price, const Quote& q) {
        const auto touch_price = touch(side, q);
        if (!touch_price) return std::nullopt;
        const double through = (side == Side::BUY ? price / *touch_price - 1 : 1 - price / *touch_price) * 100;
        if (through <= kMaxCrossPct) return std::nullopt;
        return "limit " + std::to_string(static_cast<int>(through)) + "% through the market (at most " +
               std::to_string(static_cast<int>(kMaxCrossPct)) + "%)";
    }

    static bool bracket_near(double level, double price) {
        return std::abs(level / price - 1) * 100 <= kMaxBracketPct;
    }

    // Base quantity of an entry; nullopt when the size cannot be resolved.
    static std::optional<double> entry_qty(const Size& size, double price,
                                           double nav, std::optional<double> stop) {
        if (price <= 0) return std::nullopt;
        if (size.qty) return *size.qty;
        if (size.notional) return *size.notional / price;
        if (size.risk_pct && stop && nav > 0 && *stop != price) {
            return nav * *size.risk_pct / 100 / std::abs(price - *stop);
        }
        return std::nullopt;
    }

    static double reduce_qty(const Size& size, double position_qty) {
        if (size.fraction) return position_qty * *size.fraction;
        return std::min(size.qty.value_or(position_qty), position_qty);
    }

    // A stop must sit on the losing side of the price.
    static bool stop_valid(bool is_long, double stop, double price) {
        return is_long ? stop < price : stop > price;
    }

    static bool stop_hit(bool is_long, double stop, double price) {
        return is_long ? price <= stop : price >= stop;
    }
};

}  // namespace Discretionary
