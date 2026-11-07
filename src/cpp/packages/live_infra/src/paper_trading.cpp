#include "live_infra/paper_trading.h"

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
