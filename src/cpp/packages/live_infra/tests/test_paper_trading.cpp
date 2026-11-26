#include <catch2/catch_all.hpp>

#include "live_infra/paper_trading.h"
#include "market_making/dtypes/def_only_structs.h"

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

TEST_CASE("closing a paper trade at a price closes its remainder there",
          "[paper_trading]") {
    Core::Trade trade{};
    trade.type = Core::TradeType::SHORT;
    trade.open_quantity = 2;
    trade.closed_quantity = 1;
    trade.fees = 0.5;
    trade.open_fills = {fill_at(1, 100.0, 2)};
    trade.close_fills = {fill_at(2, 90.0, 1)};

    const auto closed = PaperTrading::close_at(trade, 96.0, 1'000);
    REQUIRE(closed.closed_quantity == trade.open_quantity);
    REQUIRE(closed.open_price.toDouble() == Catch::Approx(100.0));
    REQUIRE(closed.close_price.toDouble() == Catch::Approx(93.0));
    // Short: 1 × (100 − 90) + 1 × (100 − 96) − 0.5 fees.
    REQUIRE(closed.pnl.toDouble() == Catch::Approx(13.5));
    REQUIRE(closed.pnl_percentage.toDouble() == Catch::Approx(6.75));
    REQUIRE(closed.close_time_ms == 1'000);
    REQUIRE(closed.id == trade.id);
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

namespace {

MarketData::DepthSnapshot ladder() {
    MarketData::DepthSnapshot book;
    book.received_ms = 1000;
    book.bids = {{99.0, 1.0}, {98.0, 2.0}, {97.0, 5.0}};
    book.asks = {{100.0, 1.0}, {101.0, 2.0}, {102.0, 5.0}};
    return book;
}

}  // namespace

TEST_CASE("IOC walks depth up to its limit and cancels the remainder",
          "[paper_trading][paper_depth]") {
    const auto fill = PaperTrading::evaluate_against_depth(
        Core::OrderSide::BUY, Core::OrderType::IMMEDIATE_OR_CANCEL, false,
        101.0, 4.0, ladder());
    REQUIRE(fill.outcome == Live::PaperDepthOutcome::PARTIAL_CANCELED);
    REQUIRE(fill.cancels_remainder);
    REQUIRE(fill.slices.size() == 2);
    REQUIRE(fill.slices[0].price == Catch::Approx(100.0));
    REQUIRE(fill.slices[0].quantity == Catch::Approx(1.0));
    REQUIRE(fill.slices[1].price == Catch::Approx(101.0));
    REQUIRE(fill.slices[1].quantity == Catch::Approx(2.0));
    REQUIRE(fill.filled_quantity == Catch::Approx(3.0));
    REQUIRE(fill.vwap == Catch::Approx(302.0 / 3.0));
    REQUIRE(fill.levels_consumed == 2);
    REQUIRE_FALSE(fill.depth_exhausted);

    const auto sell = PaperTrading::evaluate_against_depth(
        Core::OrderSide::SELL, Core::OrderType::IMMEDIATE_OR_CANCEL, false,
        98.0, 2.0, ladder());
    REQUIRE(sell.outcome == Live::PaperDepthOutcome::FILLED);
    REQUIRE(sell.filled_quantity == Catch::Approx(2.0));
    REQUIRE(sell.vwap == Catch::Approx(98.5));
}

TEST_CASE("IOC with nothing inside its limit is canceled unfilled",
          "[paper_trading][paper_depth]") {
    const auto fill = PaperTrading::evaluate_against_depth(
        Core::OrderSide::BUY, Core::OrderType::IMMEDIATE_OR_CANCEL, false,
        99.5, 1.0, ladder());
    REQUIRE(fill.outcome == Live::PaperDepthOutcome::NO_LIQUIDITY);
    REQUIRE(fill.cancels_remainder);
    REQUIRE(fill.slices.empty());
    REQUIRE(fill.filled_quantity == 0);
}

TEST_CASE("walking past every published level flags exhausted depth",
          "[paper_trading][paper_depth]") {
    const auto fill = PaperTrading::evaluate_against_depth(
        Core::OrderSide::BUY, Core::OrderType::MARKET, false, 0.0, 10.0,
        ladder());
    REQUIRE(fill.outcome == Live::PaperDepthOutcome::PARTIAL_CANCELED);
    REQUIRE(fill.filled_quantity == Catch::Approx(8.0));
    REQUIRE(fill.levels_consumed == 3);
    REQUIRE(fill.depth_exhausted);
}

TEST_CASE("FOK fills fully or not at all", "[paper_trading][paper_depth]") {
    const auto partial = PaperTrading::evaluate_against_depth(
        Core::OrderSide::BUY, Core::OrderType::FILL_OR_KILL, false, 101.0, 4.0,
        ladder());
    REQUIRE(partial.outcome == Live::PaperDepthOutcome::FOK_UNFILLABLE);
    REQUIRE(partial.slices.empty());
    REQUIRE(partial.filled_quantity == 0);
    REQUIRE(partial.cancels_remainder);

    const auto full = PaperTrading::evaluate_against_depth(
        Core::OrderSide::BUY, Core::OrderType::FILL_OR_KILL, false, 101.0, 3.0,
        ladder());
    REQUIRE(full.outcome == Live::PaperDepthOutcome::FILLED);
    REQUIRE(full.filled_quantity == Catch::Approx(3.0));
}

TEST_CASE("post-only rests unless it would cross",
          "[paper_trading][paper_depth]") {
    const auto crossing = PaperTrading::evaluate_against_depth(
        Core::OrderSide::BUY, Core::OrderType::LIMIT, true, 100.0, 1.0,
        ladder());
    REQUIRE(crossing.outcome == Live::PaperDepthOutcome::POST_ONLY_REJECTED);
    REQUIRE(crossing.cancels_remainder);
    REQUIRE(crossing.slices.empty());

    const auto maker = PaperTrading::evaluate_against_depth(
        Core::OrderSide::SELL, Core::OrderType::LIMIT_MAKER, false, 100.0, 1.0,
        ladder());
    REQUIRE(maker.outcome == Live::PaperDepthOutcome::RESTING);
    REQUIRE_FALSE(maker.cancels_remainder);
}

TEST_CASE("GTC takes what crosses and rests the remainder",
          "[paper_trading][paper_depth]") {
    const auto fill = PaperTrading::evaluate_against_depth(
        Core::OrderSide::BUY, Core::OrderType::LIMIT, false, 100.0, 3.0,
        ladder());
    REQUIRE(fill.outcome == Live::PaperDepthOutcome::PARTIAL_RESTING);
    REQUIRE_FALSE(fill.cancels_remainder);
    REQUIRE(fill.filled_quantity == Catch::Approx(1.0));

    const auto resting = PaperTrading::evaluate_against_depth(
        Core::OrderSide::BUY, Core::OrderType::LIMIT, false, 99.5, 3.0,
        ladder());
    REQUIRE(resting.outcome == Live::PaperDepthOutcome::RESTING);
}

TEST_CASE("without a usable book only resting orders survive",
          "[paper_trading][paper_depth]") {
    REQUIRE(PaperTrading::stale_book(Core::OrderType::IMMEDIATE_OR_CANCEL, false)
                .cancels_remainder);
    REQUIRE(PaperTrading::stale_book(Core::OrderType::MARKET, false)
                .cancels_remainder);
    REQUIRE(PaperTrading::stale_book(Core::OrderType::FILL_OR_KILL, false)
                .cancels_remainder);
    REQUIRE_FALSE(
        PaperTrading::stale_book(Core::OrderType::LIMIT, false).cancels_remainder);
    REQUIRE_FALSE(PaperTrading::stale_book(Core::OrderType::LIMIT_MAKER, false)
                      .cancels_remainder);
    REQUIRE(PaperTrading::stale_book(Core::OrderType::LIMIT, false).outcome ==
            Live::PaperDepthOutcome::STALE_BOOK);
}

TEST_CASE("the fill is judged on the book at ack plus latency, not at decision",
          "[paper_trading][paper_depth]") {
    MarketData::DepthHistory history;
    const std::string key = "BTCUSDC:lighter:swap";
    auto at_decision = ladder();
    at_decision.received_ms = 1000;
    history.publish(key, at_decision);
    auto moved = ladder();
    moved.received_ms = 1300;
    moved.asks = {{100.5, 0.5}, {101.5, 5.0}};
    history.publish(key, moved);

    const long long ack_ms = 1000;
    const long long latency_ms = 450;
    const auto book = history.at(key, ack_ms + latency_ms);
    REQUIRE(book.has_value());
    REQUIRE(book->received_ms == 1300);
    const auto fill = PaperTrading::evaluate_against_depth(
        Core::OrderSide::BUY, Core::OrderType::IMMEDIATE_OR_CANCEL, false,
        101.0, 1.0, *book);
    REQUIRE(fill.outcome == Live::PaperDepthOutcome::PARTIAL_CANCELED);
    REQUIRE(fill.filled_quantity == Catch::Approx(0.5));
    REQUIRE(fill.vwap == Catch::Approx(100.5));

    // The same order judged at decision time would have filled in full at
    // 100; the latency is what costs it.
    const auto naive = PaperTrading::evaluate_against_depth(
        Core::OrderSide::BUY, Core::OrderType::IMMEDIATE_OR_CANCEL, false,
        101.0, 1.0, *history.at(key, ack_ms));
    REQUIRE(naive.outcome == Live::PaperDepthOutcome::FILLED);
}

TEST_CASE("paper latency stays out of a venue's identity and hash",
          "[paper_trading][paper_depth]") {
    Core::ExchangeAndMarket plain{.exchange_name = "binance",
                                  .market_type = "swap",
                                  .account_id = 90003};
    REQUIRE_FALSE(plain.to_json().contains("paper_fill_latency_ms"));

    auto with_latency = plain;
    with_latency.paper_fill_latency_ms = 600;
    REQUIRE(with_latency == plain);
    REQUIRE(with_latency.to_json()["paper_fill_latency_ms"] == 600);
}

TEST_CASE("a switch to live closes the paper book; a switch to paper with state is refused",
          "[paper_trading]") {
    const auto kept = nlohmann::json{{"1", true}, {"2", false}, {"3", true}, {"5", true}};
    Core::Trade paper_trade{};
    paper_trade.strategy_id = 1;
    Core::Order live_order{};
    live_order.strategy_id = 2;
    Core::Trade other{};
    other.strategy_id = 5;
    // 1: paper -> live holding a trade. 2: live -> paper holding an order.
    // 3: paper -> live, flat. 4: new. 5: unchanged, holding a trade.
    const auto plan = PaperTrading::plan_mode_switches(
        kept, {{1, false}, {2, true}, {3, false}, {4, true}, {5, true}},
        {live_order}, {paper_trade, other});
    REQUIRE(plan.paper_to_live == std::set<int>{1});
    REQUIRE(plan.refused == std::vector<int>{2});
    REQUIRE(plan.modes == nlohmann::json{{"1", false}, {"2", true}, {"3", false}, {"4", true}, {"5", true}});

    // An unreadable modes file is a first start: nothing switched.
    const auto fresh = PaperTrading::plan_mode_switches(nlohmann::json::array(), {{1, false}}, {}, {paper_trade});
    REQUIRE(fresh.paper_to_live.empty());
    REQUIRE(fresh.refused.empty());
    REQUIRE(fresh.modes == nlohmann::json{{"1", false}});
}
