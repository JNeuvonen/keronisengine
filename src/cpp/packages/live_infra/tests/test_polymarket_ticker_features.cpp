#include <catch2/catch_all.hpp>

#include "datetime_utils.h"
#include "market_making/backtest_trade_ticker.h"
#include "market_making/dtypes/order_fill_info.h"
#include "market_making/dtypes/trade.h"
#include "market_making/engine.h"
#include "market_making/ticker.h"

static Core::Engine& get_test_engine() {
    auto& engine = Core::Engine::getInstance();
    if (!engine.isInitialized()) {
        engine.initialize([](const std::vector<TradingLog>&) {},
                          [](const Core::EngineStateSnapshot&) {},
                          Core::EngineConfig{
                              .is_backtest = true,
                              .is_strategy_development = true,
                              .emit_trading_logs = false,
                          });
    }
    return engine;
}

static unsigned long long next_fill_id() {
    static unsigned long long id = 90000;
    return ++id;
}

static unsigned long long next_order_id() {
    static unsigned long long id = 70000;
    return ++id;
}

static unsigned long long next_trade_id() {
    static unsigned long long id = 50000;
    return ++id;
}

static Core::OrderFillInfo make_polymarket_fill(
    const std::string& taker_order_id = "", bool is_finalized = true,
    const mpfr::mpreal& fill_delta_applied = 0,
    const mpfr::mpreal& fill_amount = 10,
    const mpfr::mpreal& price = mpfr::mpreal("0.55"),
    const mpfr::mpreal& sizing_coeff_delta = mpfr::mpreal("0.1"),
    unsigned long long order_id = 0) {
    Core::OrderFillInfo fill;
    fill.symbol = "condition_123";
    fill.exchange_name = "polymarket";
    fill.market_type = "spot";
    fill.price = price;
    fill.fill_amount = fill_amount;
    fill.orig_order_full_size = fill_amount;
    fill.orig_order_sizing_coeff_delta = sizing_coeff_delta;
    fill.account_id = 1;
    fill.side = Core::OrderSide::BUY;
    fill.strategy_id = 1;
    fill.order_id = order_id == 0 ? next_order_id() : order_id;
    fill.order_fill_id = next_fill_id();
    fill.order_created_timestamp = 1700000000000;
    fill.order_fill_timestamp = 1700000001000;
    fill.sizing_coeff_delta = sizing_coeff_delta;
    fill.is_fee_paid_in_base_asset = false;
    fill.taker_order_id = taker_order_id;
    fill.is_polymarket_fill_confirmed = is_finalized;
    fill.fill_delta_applied = fill_delta_applied;
    return fill;
}

static Core::Trade make_polymarket_trade(
    std::vector<Core::OrderFillInfo> open_fills = {},
    std::vector<Core::OrderFillInfo> close_fills = {},
    const mpfr::mpreal& open_quantity = 0,
    const mpfr::mpreal& closed_quantity = 0) {
    Core::Trade trade;
    trade.id = next_trade_id();
    trade.id_str = std::to_string(trade.id);
    trade.strategy_id = 1;
    trade.symbol = "condition_123";
    trade.exchange_name = "polymarket";
    trade.market_type = "spot";
    trade.account_id = 1;
    trade.type = Core::TradeType::LONG;
    trade.open_price = mpfr::mpreal("0.55");
    trade.close_price = 0;
    trade.open_quantity = open_quantity;
    trade.closed_quantity = closed_quantity;
    trade.sizing_coeff_delta = 0;
    trade.open_time_ms = 1700000000000;
    trade.enter_signal_received_time_ms = 1700000000000;
    trade.close_time_ms = 0;
    trade.open_fills = std::move(open_fills);
    trade.close_fills = std::move(close_fills);

    for (const auto& f : trade.open_fills) {
        trade.sizing_coeff_delta += f.sizing_coeff_delta;
        if (std::find(trade.enter_order_ids.begin(),
                      trade.enter_order_ids.end(),
                      f.order_id) == trade.enter_order_ids.end()) {
            trade.enter_order_ids.push_back(f.order_id);
        }
    }
    return trade;
}

static long long recent_close_time_ms() {
    return DatetimeUtils::get_current_time_ms() - 60000;
}

static std::shared_ptr<Core::BacktestTradeTicker> make_polymarket_ticker() {
    auto ticker = std::make_shared<Core::BacktestTradeTicker>(
        std::vector<Core::ExchangeAndMarket>{{"polymarket", "spot", 1}},
        "condition_123", "USDC", "OUTCOME", 1);
    ticker->is_trading_on_prediction_market.store(true);
    return ticker;
}

// ─── A. OrderFillInfo serialization round-trip ───

TEST_CASE("OrderFillInfo new fields survive to_json/from_json",
          "[polymarket][serialization]") {
    SECTION("round-trip preserves new fields") {
        auto fill = make_polymarket_fill("0xTAKER123", false, 10);
        auto json = fill.to_json();
        auto restored = Core::OrderFillInfo::from_json(json);

        REQUIRE(restored.is_polymarket_fill_confirmed == false);
        REQUIRE(restored.fill_delta_applied == mpfr::mpreal(10));
        REQUIRE(restored.taker_order_id == "0xTAKER123");
    }

    SECTION("defaults when fields absent from JSON") {
        auto fill = make_polymarket_fill();
        auto json = fill.to_json();
        json.erase("is_finalized");
        json.erase("fill_delta_applied");
        json.erase("taker_order_id");

        auto restored = Core::OrderFillInfo::from_json(json);
        REQUIRE(restored.is_polymarket_fill_confirmed == true);
        REQUIRE(restored.fill_delta_applied == mpfr::mpreal(0));
        REQUIRE(restored.taker_order_id.empty());
    }
}

// ─── B. Trade::on_enter_fill stores fill_delta_applied ───

TEST_CASE("Trade::on_enter_fill stores fill_delta_applied",
          "[polymarket][trade]") {
    Core::Trade trade = make_polymarket_trade();
    auto fill = make_polymarket_fill(
        "0xTAKER_A", false, 0, 10, mpfr::mpreal("0.55"), mpfr::mpreal("0.25"));
    mpfr::mpreal fill_delta = 10;

    trade.on_enter_fill(fill, fill_delta);

    REQUIRE(trade.open_fills.size() == 1);
    REQUIRE(trade.open_fills[0].fill_delta_applied == fill_delta);
    REQUIRE(trade.open_quantity == fill_delta);
    REQUIRE(trade.sizing_coeff_delta == mpfr::mpreal("0.25"));
}

// ─── C. Trade::from_order_fill stores fill_delta_applied ───

TEST_CASE("Trade::from_order_fill stores fill_delta_applied",
          "[polymarket][trade]") {
    auto fill = make_polymarket_fill("0xTAKER_B", false, 0, 20,
                                     mpfr::mpreal("0.60"), mpfr::mpreal("0.5"));
    mpfr::mpreal fill_delta = 20;

    auto trade = Core::Trade::from_order_fill(fill, fill_delta);

    REQUIRE(trade.open_fills.size() == 1);
    REQUIRE(trade.open_fills[0].fill_delta_applied == fill_delta);
    REQUIRE(trade.open_quantity == fill_delta);
}

// ─── D. Trade::revert_enter_fill ───

TEST_CASE("Trade::revert_enter_fill", "[polymarket][trade][revert]") {
    SECTION("single fill revert clears trade") {
        auto fill =
            make_polymarket_fill("0xTAKER_C", false, 10, 10,
                                 mpfr::mpreal("0.55"), mpfr::mpreal("0.3"));
        auto trade = make_polymarket_trade({fill}, {}, 10);

        trade.revert_enter_fill(fill);

        REQUIRE(trade.open_fills.empty());
        REQUIRE(trade.open_quantity == mpfr::mpreal(0));
        REQUIRE(trade.sizing_coeff_delta == mpfr::mpreal(0));
        REQUIRE(trade.enter_order_ids.empty());
    }

    SECTION("revert one of two fills") {
        auto fill1 =
            make_polymarket_fill("0xTAKER_D1", false, 10, 10,
                                 mpfr::mpreal("0.55"), mpfr::mpreal("0.2"));
        auto fill2 =
            make_polymarket_fill("0xTAKER_D2", true, 5, 5, mpfr::mpreal("0.60"),
                                 mpfr::mpreal("0.1"));
        auto trade = make_polymarket_trade({fill1, fill2}, {}, 15);

        trade.revert_enter_fill(fill1);

        REQUIRE(trade.open_fills.size() == 1);
        REQUIRE(trade.open_fills[0].taker_order_id == "0xTAKER_D2");
        REQUIRE(trade.open_quantity.toDouble() == Catch::Approx(5.0));
        REQUIRE(trade.sizing_coeff_delta.toDouble() == Catch::Approx(0.1));
    }
}

// ─── E. Trade::revert_exit_fill ───

TEST_CASE("Trade::revert_exit_fill", "[polymarket][trade][revert]") {
    SECTION("single exit fill revert resets closed_quantity") {
        auto enter_fill = make_polymarket_fill("0xENTER", true, 10, 10);
        auto exit_fill = make_polymarket_fill("0xEXIT", false, 0, 10);
        exit_fill.order_fill_id = next_fill_id();

        auto trade = make_polymarket_trade({enter_fill}, {exit_fill}, 10, 10);

        trade.revert_exit_fill(exit_fill);

        REQUIRE(trade.close_fills.empty());
        REQUIRE(trade.closed_quantity == mpfr::mpreal(0));
    }

    SECTION("revert one of two exit fills recalculates closed_quantity") {
        auto enter_fill = make_polymarket_fill("0xENTER2", true, 20, 20);

        auto exit1 = make_polymarket_fill("0xEXIT1", true, 0, 8);
        exit1.order_fill_id = next_fill_id();
        exit1.order_id = next_order_id();

        auto exit2 = make_polymarket_fill("0xEXIT2", false, 0, 5);
        exit2.order_fill_id = next_fill_id();
        exit2.order_id = next_order_id();

        auto trade =
            make_polymarket_trade({enter_fill}, {exit1, exit2}, 20, 13);

        trade.revert_exit_fill(exit2);

        REQUIRE(trade.close_fills.size() == 1);
        REQUIRE(trade.closed_quantity == mpfr::mpreal(8));
    }
}

// ─── F. Trade::has_unfinalized_fills ───

TEST_CASE("Trade::has_unfinalized_fills", "[polymarket][trade]") {
    SECTION("all finalized returns false") {
        auto fill = make_polymarket_fill("", true, 10);
        auto trade = make_polymarket_trade({fill}, {}, 10);
        REQUIRE_FALSE(trade.has_unfinalized_fills());
    }

    SECTION("unfinalized open fill returns true") {
        auto fill = make_polymarket_fill("0xTAKER", false, 10);
        auto trade = make_polymarket_trade({fill}, {}, 10);
        REQUIRE(trade.has_unfinalized_fills());
    }

    SECTION("unfinalized close fill returns true") {
        auto enter = make_polymarket_fill("", true, 10);
        auto exit = make_polymarket_fill("0xTAKER", false, 0);
        exit.order_fill_id = next_fill_id();
        auto trade = make_polymarket_trade({enter}, {exit}, 10, 10);
        REQUIRE(trade.has_unfinalized_fills());
    }
}

// ─── G. Trade::count_open_fills ───

TEST_CASE("Trade::count_open_fills", "[polymarket][trade]") {
    auto fill1 = make_polymarket_fill();
    auto fill2 = make_polymarket_fill();
    auto trade = make_polymarket_trade({fill1, fill2}, {}, 20);

    REQUIRE(trade.count_open_fills() == 2);

    trade.revert_enter_fill(fill1);
    REQUIRE(trade.count_open_fills() == 1);
}

// ─── H. TradeTicker::move_trade_to_completed backup ───

TEST_CASE("move_trade_to_completed backs up unfinalized polymarket trades",
          "[polymarket][ticker]") {
    SECTION("polymarket + unfinalized fills: backed up") {
        auto ticker = make_polymarket_ticker();
        auto fill = make_polymarket_fill("0xTAKER_H1", false, 10, 10);
        auto trade = make_polymarket_trade({fill}, {}, 10);
        ticker->active_trades.push_back(trade);
        ticker->active_trade_count.store(1);

        ticker->move_trade_to_completed(ticker->active_trades[0],
                                        1700000002000);

        REQUIRE(ticker->recently_completed_polymarket_trades.size() == 1);
        REQUIRE(ticker->recently_completed_polymarket_trades[0].id == trade.id);
        REQUIRE(ticker->active_trades.empty());
    }

    SECTION("polymarket + all finalized: NOT backed up") {
        auto ticker = make_polymarket_ticker();
        auto fill = make_polymarket_fill("", true, 10, 10);
        auto trade = make_polymarket_trade({fill}, {}, 10);
        ticker->active_trades.push_back(trade);
        ticker->active_trade_count.store(1);

        ticker->move_trade_to_completed(ticker->active_trades[0],
                                        1700000002000);

        REQUIRE(ticker->recently_completed_polymarket_trades.empty());
    }

    SECTION("non-polymarket ticker: never backs up") {
        auto ticker = make_polymarket_ticker();
        ticker->is_trading_on_prediction_market.store(false);

        auto fill = make_polymarket_fill("0xTAKER_H3", false, 10, 10);
        auto trade = make_polymarket_trade({fill}, {}, 10);
        ticker->active_trades.push_back(trade);
        ticker->active_trade_count.store(1);

        ticker->move_trade_to_completed(ticker->active_trades[0],
                                        1700000002000);

        REQUIRE(ticker->recently_completed_polymarket_trades.empty());
    }
}

// ─── I. TradeTicker::confirm_polymarket_fills ───

TEST_CASE("confirm_polymarket_fills", "[polymarket][ticker]") {
    auto& engine = get_test_engine();

    SECTION("marks fills as finalized in active_trades") {
        auto ticker = make_polymarket_ticker();
        auto fill = make_polymarket_fill("0xTAKER_I1", false, 10, 10);
        auto trade = make_polymarket_trade({fill}, {}, 10);
        ticker->active_trades.push_back(trade);

        ticker->confirm_polymarket_fills(engine, "0xTAKER_I1");

        REQUIRE(ticker->active_trades[0]
                    .open_fills[0]
                    .is_polymarket_fill_confirmed == true);
    }

    SECTION("self-cleaning: removes from rollback when all confirmed") {
        auto ticker = make_polymarket_ticker();
        auto fill = make_polymarket_fill("0xTAKER_I2", false, 10, 10);
        auto trade = make_polymarket_trade({fill}, {}, 10);
        trade.close_time_ms = recent_close_time_ms();
        ticker->recently_completed_polymarket_trades.push_back(trade);

        ticker->confirm_polymarket_fills(engine, "0xTAKER_I2");

        REQUIRE(ticker->recently_completed_polymarket_trades.empty());
    }

    SECTION("unknown taker_order_id is a no-op") {
        auto ticker = make_polymarket_ticker();
        auto fill = make_polymarket_fill("0xTAKER_I3", false, 10, 10);
        auto trade = make_polymarket_trade({fill}, {}, 10);
        ticker->active_trades.push_back(trade);

        ticker->confirm_polymarket_fills(engine, "0xNONEXISTENT");

        REQUIRE(ticker->active_trades[0]
                    .open_fills[0]
                    .is_polymarket_fill_confirmed == false);
    }

    SECTION("partial confirm: trade stays in rollback vector") {
        auto ticker = make_polymarket_ticker();
        auto fill1 = make_polymarket_fill("0xTAKER_I4A", false, 5, 5);
        auto fill2 = make_polymarket_fill("0xTAKER_I4B", false, 5, 5);
        auto trade = make_polymarket_trade({fill1, fill2}, {}, 10);
        trade.close_time_ms = recent_close_time_ms();
        ticker->recently_completed_polymarket_trades.push_back(trade);

        ticker->confirm_polymarket_fills(engine, "0xTAKER_I4A");

        REQUIRE(ticker->recently_completed_polymarket_trades.size() == 1);
        REQUIRE(ticker->recently_completed_polymarket_trades[0]
                    .open_fills[0]
                    .is_polymarket_fill_confirmed == true);
        REQUIRE(ticker->recently_completed_polymarket_trades[0]
                    .open_fills[1]
                    .is_polymarket_fill_confirmed == false);
    }
}

// ─── J. TradeTicker::revert_polymarket_fills on active trades ───

TEST_CASE("revert_polymarket_fills on active trades",
          "[polymarket][ticker][revert]") {
    auto& engine = get_test_engine();

    SECTION("revert only enter fill removes trade from active") {
        auto ticker = make_polymarket_ticker();
        auto fill = make_polymarket_fill("0xTAKER_J1", false, 10, 10);
        auto trade = make_polymarket_trade({fill}, {}, 10);
        ticker->active_trades.push_back(trade);
        ticker->active_trade_count.store(1);

        ticker->revert_polymarket_fills(engine, "0xTAKER_J1");

        REQUIRE(ticker->active_trades.empty());
        REQUIRE(ticker->active_trade_count.load() == 0);
    }

    SECTION("revert exit fill on active trade keeps trade active") {
        auto ticker = make_polymarket_ticker();
        auto enter_fill = make_polymarket_fill("0xENTER_J2", true, 10, 10);
        auto exit_fill = make_polymarket_fill("0xEXIT_J2", false, 0, 5);
        exit_fill.order_fill_id = next_fill_id();
        exit_fill.order_id = next_order_id();

        auto trade = make_polymarket_trade({enter_fill}, {exit_fill}, 10, 5);
        ticker->active_trades.push_back(trade);
        ticker->active_trade_count.store(1);

        ticker->revert_polymarket_fills(engine, "0xEXIT_J2");

        REQUIRE(ticker->active_trades.size() == 1);
        REQUIRE(ticker->active_trades[0].close_fills.empty());
        REQUIRE(ticker->active_trades[0].closed_quantity == mpfr::mpreal(0));
        REQUIRE(ticker->active_trade_count.load() == 1);
    }
}

// ─── K. TradeTicker::revert_polymarket_fills on completed trades ───

TEST_CASE("revert_polymarket_fills on completed trades",
          "[polymarket][ticker][revert]") {
    auto& engine = get_test_engine();

    SECTION("revert exit fill moves completed trade back to active") {
        auto ticker = make_polymarket_ticker();
        auto enter_fill = make_polymarket_fill("0xENTER_K1", true, 10, 10);
        auto exit_fill = make_polymarket_fill("0xEXIT_K1", false, 0, 10);
        exit_fill.order_fill_id = next_fill_id();
        exit_fill.order_id = next_order_id();

        auto trade = make_polymarket_trade({enter_fill}, {exit_fill}, 10, 10);
        trade.close_time_ms = recent_close_time_ms();
        trade.pnl = mpfr::mpreal("5.0");
        ticker->recently_completed_polymarket_trades.push_back(trade);
        ticker->active_trade_count.store(0);

        ticker->revert_polymarket_fills(engine, "0xEXIT_K1");

        REQUIRE(ticker->recently_completed_polymarket_trades.empty());
        REQUIRE(ticker->active_trades.size() == 1);
        REQUIRE(ticker->active_trades[0].close_fills.empty());
        REQUIRE(ticker->active_trades[0].closed_quantity == mpfr::mpreal(0));
        REQUIRE(ticker->active_trade_count.load() == 1);
    }

    SECTION(
        "revert enter fill on completed trade with no remaining fills "
        "removes from rollback") {
        auto ticker = make_polymarket_ticker();
        auto fill = make_polymarket_fill("0xTAKER_K2", false, 10, 10);
        auto trade = make_polymarket_trade({fill}, {}, 10);
        trade.close_time_ms = recent_close_time_ms();
        ticker->recently_completed_polymarket_trades.push_back(trade);

        ticker->revert_polymarket_fills(engine, "0xTAKER_K2");

        REQUIRE(ticker->recently_completed_polymarket_trades.empty());
        REQUIRE(ticker->active_trades.empty());
    }
}

// ─── L. Lifecycle: MATCHED enter fill -> CONFIRMED ───

TEST_CASE("lifecycle: MATCHED enter fill then CONFIRMED",
          "[polymarket][lifecycle]") {
    auto& engine = get_test_engine();

    auto ticker = make_polymarket_ticker();
    auto fill = make_polymarket_fill("0xTAKER_L", false, 10, 10);
    auto trade = make_polymarket_trade({fill}, {}, 10);
    ticker->active_trades.push_back(trade);
    ticker->active_trade_count.store(1);

    ticker->confirm_polymarket_fills(engine, "0xTAKER_L");

    REQUIRE(ticker->active_trades.size() == 1);
    REQUIRE(
        ticker->active_trades[0].open_fills[0].is_polymarket_fill_confirmed ==
        true);
    REQUIRE(ticker->recently_completed_polymarket_trades.empty());
    REQUIRE(ticker->active_trade_count.load() == 1);
}

// ─── M. Lifecycle: MATCHED -> trade completes -> FAILED -> restore ───

TEST_CASE("lifecycle: MATCHED enter -> complete -> FAILED reverts enter",
          "[polymarket][lifecycle]") {
    auto& engine = get_test_engine();

    auto ticker = make_polymarket_ticker();
    auto enter_fill = make_polymarket_fill(
        "0xTAKER_M", false, 10, 10, mpfr::mpreal("0.55"), mpfr::mpreal("0.3"));
    auto exit_fill = make_polymarket_fill("0xEXIT_M", true, 10, 10);
    exit_fill.order_fill_id = next_fill_id();
    exit_fill.order_id = next_order_id();

    auto trade = make_polymarket_trade({enter_fill}, {exit_fill}, 10, 10);
    ticker->active_trades.push_back(trade);
    ticker->active_trade_count.store(1);

    ticker->move_trade_to_completed(ticker->active_trades[0], 1700000005000);

    REQUIRE(ticker->active_trades.empty());
    REQUIRE(ticker->recently_completed_polymarket_trades.size() == 1);

    ticker->revert_polymarket_fills(engine, "0xTAKER_M");

    REQUIRE(ticker->recently_completed_polymarket_trades.empty());
    REQUIRE(ticker->active_trades.empty());
    REQUIRE(ticker->active_trade_count.load() == 0);
}

// ─── N. Idempotency: double CONFIRMED ───

TEST_CASE("idempotent double CONFIRMED", "[polymarket][idempotency]") {
    auto& engine = get_test_engine();

    auto ticker = make_polymarket_ticker();
    auto fill = make_polymarket_fill("0xTAKER_N", false, 10, 10);
    auto trade = make_polymarket_trade({fill}, {}, 10);
    ticker->active_trades.push_back(trade);

    ticker->confirm_polymarket_fills(engine, "0xTAKER_N");
    REQUIRE(
        ticker->active_trades[0].open_fills[0].is_polymarket_fill_confirmed ==
        true);

    ticker->confirm_polymarket_fills(engine, "0xTAKER_N");
    REQUIRE(
        ticker->active_trades[0].open_fills[0].is_polymarket_fill_confirmed ==
        true);
    REQUIRE(ticker->active_trades.size() == 1);
    REQUIRE(ticker->active_trades[0].open_fills.size() == 1);
}

// ─── O. Idempotency: double FAILED ───

TEST_CASE("idempotent double FAILED", "[polymarket][idempotency]") {
    auto& engine = get_test_engine();

    auto ticker = make_polymarket_ticker();
    auto fill = make_polymarket_fill("0xTAKER_O", false, 10, 10);
    auto trade = make_polymarket_trade({fill}, {}, 10);
    ticker->active_trades.push_back(trade);
    ticker->active_trade_count.store(1);

    ticker->revert_polymarket_fills(engine, "0xTAKER_O");
    REQUIRE(ticker->active_trades.empty());
    REQUIRE(ticker->active_trade_count.load() == 0);

    ticker->revert_polymarket_fills(engine, "0xTAKER_O");
    REQUIRE(ticker->active_trades.empty());
    REQUIRE(ticker->active_trade_count.load() == 0);
}

// ─── P. Terminal ordering: CONFIRMED then FAILED ───

TEST_CASE("terminal: CONFIRMED then FAILED is blocked",
          "[polymarket][terminal]") {
    auto& engine = get_test_engine();

    auto ticker = make_polymarket_ticker();
    auto fill = make_polymarket_fill("0xTAKER_P", false, 10, 10);
    auto trade = make_polymarket_trade({fill}, {}, 10);
    ticker->active_trades.push_back(trade);
    ticker->active_trade_count.store(1);

    ticker->confirm_polymarket_fills(engine, "0xTAKER_P");
    REQUIRE(
        ticker->active_trades[0].open_fills[0].is_polymarket_fill_confirmed ==
        true);

    ticker->revert_polymarket_fills(engine, "0xTAKER_P");

    REQUIRE(ticker->active_trades.size() == 1);
    REQUIRE(ticker->active_trades[0].open_fills.size() == 1);
    REQUIRE(
        ticker->active_trades[0].open_fills[0].is_polymarket_fill_confirmed ==
        true);
    REQUIRE(ticker->active_trade_count.load() == 1);
}

// ─── Q. Terminal ordering: FAILED then CONFIRMED ───

TEST_CASE("terminal: FAILED then CONFIRMED finds nothing",
          "[polymarket][terminal]") {
    auto& engine = get_test_engine();

    auto ticker = make_polymarket_ticker();
    auto fill = make_polymarket_fill("0xTAKER_Q", false, 10, 10);
    auto trade = make_polymarket_trade({fill}, {}, 10);
    ticker->active_trades.push_back(trade);
    ticker->active_trade_count.store(1);

    ticker->revert_polymarket_fills(engine, "0xTAKER_Q");
    REQUIRE(ticker->active_trades.empty());

    ticker->confirm_polymarket_fills(engine, "0xTAKER_Q");

    REQUIRE(ticker->active_trades.empty());
    REQUIRE(ticker->recently_completed_polymarket_trades.empty());
    REQUIRE(ticker->active_trade_count.load() == 0);
}

// ─── R. Confirm close fills on active trade ───

TEST_CASE("confirm close fills on active trade", "[polymarket][ticker]") {
    auto& engine = get_test_engine();

    auto ticker = make_polymarket_ticker();
    auto enter_fill = make_polymarket_fill("0xENTER_R", true, 10, 10);
    auto exit_fill = make_polymarket_fill("0xEXIT_R", false, 0, 5);
    exit_fill.order_fill_id = next_fill_id();
    exit_fill.order_id = next_order_id();

    auto trade = make_polymarket_trade({enter_fill}, {exit_fill}, 10, 5);
    ticker->active_trades.push_back(trade);

    ticker->confirm_polymarket_fills(engine, "0xEXIT_R");

    REQUIRE(ticker->active_trades.size() == 1);
    REQUIRE(ticker->active_trades[0].close_fills.size() == 1);
    REQUIRE(
        ticker->active_trades[0].close_fills[0].is_polymarket_fill_confirmed ==
        true);
    REQUIRE(
        ticker->active_trades[0].open_fills[0].is_polymarket_fill_confirmed ==
        true);
}

// ─── S. Revert with both active AND completed trades matching ───

TEST_CASE("revert with both active and completed trades matching",
          "[polymarket][ticker][revert]") {
    auto& engine = get_test_engine();

    auto ticker = make_polymarket_ticker();

    auto shared_order_id = next_order_id();

    auto active_fill =
        make_polymarket_fill("0xSHARED_S", false, 5, 5, mpfr::mpreal("0.55"),
                             mpfr::mpreal("0.1"), shared_order_id);
    auto active_trade = make_polymarket_trade({active_fill}, {}, 5);
    ticker->active_trades.push_back(active_trade);
    ticker->active_trade_count.store(1);

    auto completed_fill = make_polymarket_fill(
        "0xSHARED_S", false, 8, 8, mpfr::mpreal("0.55"), mpfr::mpreal("0.2"));
    auto completed_trade = make_polymarket_trade({completed_fill}, {}, 8);
    completed_trade.close_time_ms = recent_close_time_ms();
    ticker->recently_completed_polymarket_trades.push_back(completed_trade);

    ticker->revert_polymarket_fills(engine, "0xSHARED_S");

    REQUIRE(ticker->active_trades.empty());
    REQUIRE(ticker->active_trade_count.load() == 0);
    REQUIRE(ticker->recently_completed_polymarket_trades.empty());
}

// ─── T. TTL eviction of stale rollback entries ───

TEST_CASE("TTL eviction of stale rollback entries",
          "[polymarket][ticker][ttl]") {
    auto& engine = get_test_engine();

    SECTION("stale entry beyond TTL is evicted") {
        auto ticker = make_polymarket_ticker();

        long long now_ms = DatetimeUtils::get_current_time_ms();
        long long stale_close_time =
            now_ms - Core::POLYMARKET_ROLLBACK_TTL_MS - 1000;

        auto fill = make_polymarket_fill("0xSTALE_T1", false, 10, 10);
        auto trade = make_polymarket_trade({fill}, {}, 10);
        trade.close_time_ms = stale_close_time;
        ticker->recently_completed_polymarket_trades.push_back(trade);

        ticker->confirm_polymarket_fills(engine, "0xUNRELATED");

        REQUIRE(ticker->recently_completed_polymarket_trades.empty());
    }

    SECTION("fresh entry within TTL is NOT evicted") {
        auto ticker = make_polymarket_ticker();

        long long now_ms = DatetimeUtils::get_current_time_ms();
        long long fresh_close_time = now_ms - 60000;

        auto fill = make_polymarket_fill("0xFRESH_T2", false, 10, 10);
        auto trade = make_polymarket_trade({fill}, {}, 10);
        trade.close_time_ms = fresh_close_time;
        ticker->recently_completed_polymarket_trades.push_back(trade);

        ticker->confirm_polymarket_fills(engine, "0xUNRELATED");

        REQUIRE(ticker->recently_completed_polymarket_trades.size() == 1);
    }

    SECTION("mixed: stale evicted, fresh kept") {
        auto ticker = make_polymarket_ticker();

        long long now_ms = DatetimeUtils::get_current_time_ms();

        auto stale_fill = make_polymarket_fill("0xSTALE_T3", false, 10, 10);
        auto stale_trade = make_polymarket_trade({stale_fill}, {}, 10);
        stale_trade.close_time_ms =
            now_ms - Core::POLYMARKET_ROLLBACK_TTL_MS - 5000;
        ticker->recently_completed_polymarket_trades.push_back(stale_trade);

        auto fresh_fill = make_polymarket_fill("0xFRESH_T3", false, 5, 5);
        auto fresh_trade = make_polymarket_trade({fresh_fill}, {}, 5);
        fresh_trade.close_time_ms = now_ms - 30000;
        ticker->recently_completed_polymarket_trades.push_back(fresh_trade);

        ticker->confirm_polymarket_fills(engine, "0xUNRELATED");

        REQUIRE(ticker->recently_completed_polymarket_trades.size() == 1);
        REQUIRE(ticker->recently_completed_polymarket_trades[0].id ==
                fresh_trade.id);
    }
}
