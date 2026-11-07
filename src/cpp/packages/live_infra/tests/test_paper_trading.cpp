#include <catch2/catch_all.hpp>

#include "live_infra/paper_trading.h"

using Live::PaperQuote;
using Live::PaperTrading;

TEST_CASE("first check takes at the touch when it is inside the limit",
          "[paper_trading]") {
    const PaperQuote quote{.best_bid = 99.0, .best_ask = 100.0, .last = 99.5};

    const auto buy = PaperTrading::evaluate(Core::OrderSide::BUY, 101.0, true,
                                            quote, 3.0);
    REQUIRE(buy.has_value());
    REQUIRE(buy->price == Catch::Approx(100.0));
    REQUIRE_FALSE(buy->is_maker);

    const auto sell = PaperTrading::evaluate(Core::OrderSide::SELL, 98.0, true,
                                             quote, 3.0);
    REQUIRE(sell.has_value());
    REQUIRE(sell->price == Catch::Approx(99.0));
    REQUIRE_FALSE(sell->is_maker);

    REQUIRE_FALSE(PaperTrading::evaluate(Core::OrderSide::BUY, 99.5, true,
                                         quote, 3.0));
    REQUIRE_FALSE(PaperTrading::evaluate(Core::OrderSide::SELL, 99.5, true,
                                         quote, 3.0));
}

TEST_CASE("first check without a book uses last price plus slippage",
          "[paper_trading]") {
    const PaperQuote quote{.last = 100.0};

    const auto buy = PaperTrading::evaluate(Core::OrderSide::BUY, 101.0, true,
                                            quote, 10.0);
    REQUIRE(buy.has_value());
    REQUIRE(buy->price == Catch::Approx(100.1));

    const auto sell = PaperTrading::evaluate(Core::OrderSide::SELL, 99.0, true,
                                             quote, 10.0);
    REQUIRE(sell.has_value());
    REQUIRE(sell->price == Catch::Approx(99.9));

    REQUIRE_FALSE(PaperTrading::evaluate(Core::OrderSide::BUY, 100.05, true,
                                         quote, 10.0));
}

TEST_CASE("resting order fills at its limit only when traded through",
          "[paper_trading]") {
    const PaperQuote at_limit{.best_bid = 99.0, .best_ask = 100.0};
    REQUIRE_FALSE(PaperTrading::evaluate(Core::OrderSide::BUY, 100.0, false,
                                         at_limit, 3.0));
    REQUIRE_FALSE(PaperTrading::evaluate(Core::OrderSide::SELL, 99.0, false,
                                         at_limit, 3.0));

    const auto buy = PaperTrading::evaluate(Core::OrderSide::BUY, 100.5, false,
                                            at_limit, 3.0);
    REQUIRE(buy.has_value());
    REQUIRE(buy->price == Catch::Approx(100.5));
    REQUIRE(buy->is_maker);

    const auto sell = PaperTrading::evaluate(Core::OrderSide::SELL, 98.5,
                                             false, at_limit, 3.0);
    REQUIRE(sell.has_value());
    REQUIRE(sell->price == Catch::Approx(98.5));
    REQUIRE(sell->is_maker);

    const PaperQuote last_only{.last = 97.0};
    REQUIRE(PaperTrading::evaluate(Core::OrderSide::BUY, 98.0, false,
                                   last_only, 3.0));
    REQUIRE_FALSE(PaperTrading::evaluate(Core::OrderSide::BUY, 97.0, false,
                                         last_only, 3.0));
}

TEST_CASE("no price means no fill", "[paper_trading]") {
    REQUIRE_FALSE(PaperTrading::evaluate(Core::OrderSide::BUY, 1e9, true,
                                         PaperQuote{}, 3.0));
    REQUIRE_FALSE(PaperTrading::evaluate(Core::OrderSide::SELL, 0.0, false,
                                         PaperQuote{}, 3.0));
}

static Core::OrderFillInfo fill_at(unsigned long long order_id, double price,
                                   double amount) {
    Core::OrderFillInfo fill{};
    fill.order_id = order_id;
    fill.price = price;
    fill.fill_amount = amount;
    return fill;
}

TEST_CASE("mark-to-market combines the closed part and the open remainder",
          "[paper_trading]") {
    Core::Trade trade{};
    trade.type = Core::TradeType::LONG;
    trade.open_quantity = 2;
    trade.closed_quantity = 1;
    trade.open_fills = {fill_at(1, 100.0, 2)};
    trade.close_fills = {fill_at(2, 110.0, 1)};

    // 1 × (110 − 100) realized + 1 × (105 − 100) open.
    REQUIRE(PaperTrading::mark_to_market_pnl(trade, 105.0) ==
            Catch::Approx(15.0));

    trade.type = Core::TradeType::SHORT;
    REQUIRE(PaperTrading::mark_to_market_pnl(trade, 105.0) ==
            Catch::Approx(-15.0));
}

TEST_CASE("resting order fills on a trade through it since the last check, not a touch",
          "[paper_trading]") {
    const PaperQuote touched{.last = 100.0, .trade_low = 99.0, .trade_high = 101.0};
    REQUIRE_FALSE(PaperTrading::evaluate(Core::OrderSide::BUY, 99.0, false,
                                         touched, 3.0));
    REQUIRE_FALSE(PaperTrading::evaluate(Core::OrderSide::SELL, 101.0, false,
                                         touched, 3.0));

    const auto buy = PaperTrading::evaluate(Core::OrderSide::BUY, 99.5, false,
                                            touched, 3.0);
    REQUIRE(buy.has_value());
    REQUIRE(buy->price == Catch::Approx(99.5));
    REQUIRE(buy->is_maker);
    const auto sell = PaperTrading::evaluate(Core::OrderSide::SELL, 100.5, false,
                                             touched, 3.0);
    REQUIRE(sell.has_value());
    REQUIRE(sell->is_maker);

    // The book has not moved through the bid, but a print went through it.
    const PaperQuote book_and_print{.best_bid = 99.0, .best_ask = 99.1, .trade_low = 98.9};
    REQUIRE(PaperTrading::evaluate(Core::OrderSide::BUY, 99.0, false,
                                   book_and_print, 3.0));
}

TEST_CASE("post-only rejects when it would take on arrival", "[paper_trading]") {
    const PaperQuote quote{.best_bid = 99.0, .best_ask = 100.0, .last = 99.5};
    REQUIRE(PaperTrading::rejects_post_only(Core::OrderSide::BUY, 100.0, quote));
    REQUIRE_FALSE(PaperTrading::rejects_post_only(Core::OrderSide::BUY, 99.9, quote));
    REQUIRE(PaperTrading::rejects_post_only(Core::OrderSide::SELL, 99.0, quote));
    REQUIRE_FALSE(PaperTrading::rejects_post_only(Core::OrderSide::SELL, 99.1, quote));
    const PaperQuote last_only{.last = 50.0};
    REQUIRE(PaperTrading::rejects_post_only(Core::OrderSide::BUY, 50.0, last_only));
}
