#include "live_infra/paper_trading.h"

#include <algorithm>

namespace {

bool is_post_only(Core::OrderType type, bool post_only) {
    return post_only || type == Core::OrderType::LIMIT_MAKER;
}

bool must_not_rest(Core::OrderType type) {
    return type == Core::OrderType::IMMEDIATE_OR_CANCEL ||
           type == Core::OrderType::FILL_OR_KILL ||
           type == Core::OrderType::MARKET;
}

}  // namespace

Live::PaperDepthFill Live::PaperTrading::stale_book(Core::OrderType type,
                                                    bool post_only) {
    PaperDepthFill out;
    out.outcome = PaperDepthOutcome::STALE_BOOK;
    out.cancels_remainder =
        must_not_rest(type) && !is_post_only(type, post_only);
    return out;
}

Live::PaperDepthFill Live::PaperTrading::evaluate_against_depth(
    Core::OrderSide side, Core::OrderType type, bool post_only,
    double limit_price, double quantity,
    const MarketData::DepthSnapshot &book) {
    PaperDepthFill out;
    const bool is_buy = side == Core::OrderSide::BUY;
    const auto &levels = is_buy ? book.asks : book.bids;
    const bool no_limit = type == Core::OrderType::MARKET;
    auto within_limit = [&](double price) {
        return no_limit ||
               (is_buy ? price <= limit_price : price >= limit_price);
    };

    if (is_post_only(type, post_only)) {
        if (!levels.empty() && within_limit(levels.front().price)) {
            out.outcome = PaperDepthOutcome::POST_ONLY_REJECTED;
            out.cancels_remainder = true;
        } else {
            out.outcome = PaperDepthOutcome::RESTING;
        }
        return out;
    }

    const double epsilon = quantity * 1e-9;
    double remaining = quantity;
    double notional = 0;
    size_t i = 0;
    for (; i < levels.size() && remaining > epsilon; ++i) {
        if (!within_limit(levels[i].price)) break;
        const double take = std::min(remaining, levels[i].quantity);
        if (take <= 0) continue;
        out.slices.push_back({levels[i].price, take});
        notional += take * levels[i].price;
        remaining -= take;
    }
    const bool complete = remaining <= epsilon;
    out.filled_quantity = complete ? quantity : quantity - remaining;
    out.levels_consumed = static_cast<int>(out.slices.size());
    out.depth_exhausted = !complete && i == levels.size();
    if (!out.slices.empty()) out.vwap = notional / (quantity - remaining);

    if (type == Core::OrderType::FILL_OR_KILL && !complete) {
        out = PaperDepthFill{};
        out.outcome = PaperDepthOutcome::FOK_UNFILLABLE;
        out.cancels_remainder = true;
        return out;
    }
    if (complete) {
        out.outcome = PaperDepthOutcome::FILLED;
        return out;
    }
    if (must_not_rest(type)) {
        out.cancels_remainder = true;
        out.outcome = out.slices.empty() ? PaperDepthOutcome::NO_LIQUIDITY
                                         : PaperDepthOutcome::PARTIAL_CANCELED;
        return out;
    }
    out.outcome = out.slices.empty() ? PaperDepthOutcome::RESTING
                                     : PaperDepthOutcome::PARTIAL_RESTING;
    return out;
}

const char *Live::PaperTrading::outcome_name(PaperDepthOutcome outcome) {
    switch (outcome) {
        case PaperDepthOutcome::FILLED: return "filled";
        case PaperDepthOutcome::PARTIAL_CANCELED: return "partial_canceled";
        case PaperDepthOutcome::PARTIAL_RESTING: return "partial_resting";
        case PaperDepthOutcome::RESTING: return "resting";
        case PaperDepthOutcome::NO_LIQUIDITY: return "no_liquidity";
        case PaperDepthOutcome::FOK_UNFILLABLE: return "fok_unfillable";
        case PaperDepthOutcome::POST_ONLY_REJECTED: return "post_only_rejected";
        case PaperDepthOutcome::STALE_BOOK: return "stale_book";
    }
    return "unknown";
}

std::optional<Live::PaperFill> Live::PaperTrading::evaluate(
    Core::OrderSide side, double limit_price, bool is_first_check,
    const PaperQuote &quote, double slippage_bps) {
    const bool is_buy = side == Core::OrderSide::BUY;
    const auto touch = is_buy ? quote.best_ask : quote.best_bid;

    if (is_first_check) {
        std::optional<double> price = touch;
        if (!price.has_value() && quote.last.has_value()) {
            const double slip = slippage_bps / 10000.0;
            price = quote.last.value() * (is_buy ? 1.0 + slip : 1.0 - slip);
        }
        if (!price.has_value()) return std::nullopt;
        const bool crosses = is_buy ? price.value() <= limit_price
                                    : price.value() >= limit_price;
        if (!crosses) return std::nullopt;
        return PaperFill{.price = price.value(), .is_maker = false};
    }

    const auto through = [&](const std::optional<double> &p) {
        return p.has_value() &&
               (is_buy ? p.value() < limit_price : p.value() > limit_price);
    };
    if (!through(touch) && !through(quote.last) &&
        !through(is_buy ? quote.trade_low : quote.trade_high))
        return std::nullopt;
    return PaperFill{.price = limit_price, .is_maker = true};
}

bool Live::PaperTrading::rejects_post_only(Core::OrderSide side,
                                           double limit_price,
                                           const PaperQuote &quote) {
    const bool is_buy = side == Core::OrderSide::BUY;
    const auto touch = is_buy ? quote.best_ask : quote.best_bid;
    const auto price = touch.has_value() ? touch : quote.last;
    return price.has_value() &&
           (is_buy ? price.value() <= limit_price : price.value() >= limit_price);
}

double Live::PaperTrading::mark_to_market_pnl(const Core::Trade &trade,
                                              double price) {
    const auto [avg_open, avg_close] = trade.get_open_and_close_price();
    const double open = avg_open.toDouble();
    const double closed_qty = trade.closed_quantity.toDouble();
    const double remaining_qty =
        (trade.open_quantity - trade.closed_quantity).toDouble();
    const double sign = trade.type == Core::TradeType::LONG ? 1.0 : -1.0;
    return sign * (closed_qty * (avg_close.toDouble() - open) +
                   remaining_qty * (price - open));
}

Core::Trade Live::PaperTrading::close_at(const Core::Trade &trade, double price,
                                         long long now_ms) {
    Core::Trade closed = trade;
    const auto [avg_open, avg_close] = trade.get_open_and_close_price();
    const mpfr::mpreal remaining = trade.open_quantity - trade.closed_quantity;
    closed.open_price = avg_open;
    closed.close_price =
        trade.open_quantity > 0
            ? (trade.closed_quantity * avg_close + remaining * price) /
                  trade.open_quantity
            : mpfr::mpreal(price);
    closed.closed_quantity = trade.open_quantity;
    closed.close_time_ms = now_ms;
    closed.pnl = mpfr::mpreal(mark_to_market_pnl(trade, price)) - trade.fees;
    const auto open_notional = trade.open_quantity * avg_open;
    closed.pnl_percentage =
        open_notional != 0 ? closed.pnl / open_notional * 100 : mpfr::mpreal(0);
    return closed;
}

Live::PaperModeSwitch Live::PaperTrading::plan_mode_switches(
    const nlohmann::json &kept_modes,
    const std::vector<std::pair<int, bool>> &strategies,
    const std::vector<Core::Order> &orders,
    const std::vector<Core::Trade> &trades) {
    PaperModeSwitch plan;
    plan.modes = kept_modes.is_object() ? kept_modes : nlohmann::json::object();
    for (const auto &[id, is_paper] : strategies) {
        const auto key = std::to_string(id);
        const bool switched = plan.modes.contains(key) &&
                              plan.modes[key].is_boolean() &&
                              plan.modes[key].get<bool>() != is_paper;
        const bool has_state =
            std::any_of(orders.begin(), orders.end(),
                        [&](const auto &o) { return o.strategy_id == id; }) ||
            std::any_of(trades.begin(), trades.end(),
                        [&](const auto &t) { return t.strategy_id == id; });
        if (switched && has_state) {
            if (is_paper) plan.refused.push_back(id);
            else plan.paper_to_live.insert(id);
        }
        plan.modes[key] = is_paper;
    }
    return plan;
}
