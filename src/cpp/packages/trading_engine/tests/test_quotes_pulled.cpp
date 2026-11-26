// A strategy whose quotes are pulled refuses new resting entries and has the
// resting ones swept; take-profits and taker orders are left alone.

#include <catch2/catch_test_macros.hpp>

#include "market_making/engine.h"
#include "market_making/strategy.h"
#include "test_helpers.h"

namespace {

using SyncResult = Core::Engine::SyncResult;

Core::Order resting(unsigned long long id, Core::OrderType type, bool close) {
    Core::Order o;
    o.id = id;
    o.id_str = std::to_string(id);
    o.client_order_id = "c" + o.id_str;
    o.type = type;
    o.side = Core::OrderSide::BUY;
    o.symbol = "BTCUSDT";
    o.exchange_name = "binance";
    o.market_type = "swap";
    o.exchange_enum = Core::Exchange::BINANCE;
    o.account_id = 1;
    o.strategy_id = 900700;
    o.price = mpfr::mpreal("3000");
    o.quantity = mpfr::mpreal("0.01");
    o.filled_quantity = mpfr::mpreal("0");
    o.create_timestamp = 1;
    o.expires_on_ms = 0;
    o.is_trade_close_order = close;
    o.is_order_creation_acknowledged = true;
    return o;
}

Core::CreateOrderParams params(Core::OrderType type, bool close) {
    return Core::CreateOrderParams{.symbol = "BTCUSDT",
                                   .type = type,
                                   .side = Core::OrderSide::BUY,
                                   .price = mpfr::mpreal("3000"),
                                   .quantity = mpfr::mpreal("0.01"),
                                   .exchange_name = "binance",
                                   .market_type = "swap",
                                   .strategy_id = 900700,
                                   .is_trade_close_order = close};
}

}  // namespace

TEST_CASE("Pulled quotes: entries refused and swept, exits kept",
          "[engine][quotes_pulled]") {
    TestHelpers::ensure_minimal_engine();
    auto& engine = Core::Engine::getInstance();
    engine.is_backtest = true;
    constexpr int kId = 900700;

    auto strategy = TestHelpers::make_test_strategy(kId);
    strategy->strategy_config.is_disabled = false;
    strategy->set_disabled(false);
    auto owned = TestHelpers::make_test_ticker("BTCUSDT", "USDT", "BTC", kId);
    TestHelpers::MockTradeTicker* ticker = owned.get();
    strategy->add_trade_ticker("BTCUSDT", std::shared_ptr<Core::TradeTicker>(std::move(owned)));
    REQUIRE(engine.add_strategy_while_trading(strategy) == SyncResult::APPLIED);
    Core::Strategy* loaded = nullptr;
    for (auto& s : engine.get_strategies()) {
        if (s->strategy_config.id == kId) loaded = s.get();
    }
    REQUIRE(loaded != nullptr);

    ticker->orders = {resting(1, Core::OrderType::LIMIT_MAKER, false),
                      resting(2, Core::OrderType::LIMIT, true),
                      resting(3, Core::OrderType::IMMEDIATE_OR_CANCEL, false)};

    SECTION("not pulled: nothing is swept or refused") {
        REQUIRE(engine.sweep_pulled_quotes() == 0);
        CHECK(ticker->cancel_order_impl_calls.empty());
        auto p = params(Core::OrderType::LIMIT, false);
        CHECK(std::get<0>(ticker->construct_order(engine, *loaded, p)) !=
              Core::STRATEGY_QUOTES_PULLED);
    }

    SECTION("pulled") {
        loaded->set_quotes_pulled(true, Core::QuotesPullReason::PULL_SIGNAL);

        for (const auto& [type, close, refused] :
             {std::tuple{Core::OrderType::LIMIT, false, true},
              std::tuple{Core::OrderType::LIMIT_MAKER, false, true},
              std::tuple{Core::OrderType::LIMIT, true, false},
              std::tuple{Core::OrderType::IMMEDIATE_OR_CANCEL, false, false},
              std::tuple{Core::OrderType::MARKET, false, false}}) {
            auto p = params(type, close);
            const auto result = std::get<0>(ticker->construct_order(engine, *loaded, p));
            CHECK((result == Core::STRATEGY_QUOTES_PULLED) == refused);
        }

        // The entry quote is still on the books until its cancel is acked.
        CHECK(engine.sweep_pulled_quotes() == 1);
        REQUIRE(ticker->cancel_order_impl_calls.size() == 1);
        CHECK(ticker->cancel_order_impl_calls[0].id == 1ULL);

        // A cancel already in flight is not sent twice.
        engine.sweep_pulled_quotes();
        CHECK(ticker->cancel_order_impl_calls.size() == 1);

        // Once acked the order is gone and the sweep reports nothing resting.
        ticker->orders.erase(ticker->orders.begin());
        CHECK(engine.sweep_pulled_quotes() == 0);

        loaded->set_quotes_pulled(false, Core::QuotesPullReason::PULL_SIGNAL);
        auto p = params(Core::OrderType::LIMIT, false);
        CHECK(std::get<0>(ticker->construct_order(engine, *loaded, p)) !=
              Core::STRATEGY_QUOTES_PULLED);
    }

    ticker->orders.clear();
    REQUIRE(engine.remove_strategy(kId) == SyncResult::APPLIED);
}
