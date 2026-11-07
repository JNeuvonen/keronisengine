#pragma once

// Minimal shared fixtures for trading_engine unit tests.
//
// The engine is a singleton (Core::Engine::getInstance()) with
// initialize() / reset() as the lifecycle hooks. Unit tests here call
// ensure_minimal_engine() at TEST_CASE entry; subsequent calls within
// the same process are no-ops. Engine runs in backtest mode — no DB,
// no network, no strategy loading.
//
// Exchange info for BTCUSDT/ETHUSDT on binance/swap is pre-populated so
// Order::to_json() can resolve precision via SymbolPrecisionAccess.

#include <cstdlib>
#include <memory>

#include "common_types/strategy.h"
#include "market_making/dtypes/def_only_structs.h"
#include "market_making/engine.h"
#include "market_making/strategy.h"
#include "market_making/ticker.h"

namespace TestHelpers {

// A minimal concrete TradeTicker for unit tests. TradeTicker is abstract
// — the two overrides below just record that they were called. Tests
// that need engine-ack behavior must call engine.order_created_ack /
// order_canceled_ack explicitly after on_create_order / on_cancel_order.
class MockTradeTicker : public Core::TradeTicker {
   public:
    MockTradeTicker(std::vector<Core::ExchangeAndMarket> ems,
                    std::string symbol, std::string quote_asset,
                    std::string base_asset, int strategy_id)
        : Core::TradeTicker(std::move(ems), std::move(symbol),
                             std::move(quote_asset), std::move(base_asset),
                             strategy_id) {}
    void on_create_order_impl(Core::Engine& /*engine*/,
                              Core::Order& order) override {
        create_order_impl_calls.push_back(order);
    }
    void on_cancel_order_impl(Core::Engine& /*engine*/,
                              Core::Order& order) override {
        cancel_order_impl_calls.push_back(order);
    }
    std::vector<Core::Order> create_order_impl_calls;
    std::vector<Core::Order> cancel_order_impl_calls;

    // Test-only: inject a pending cancel with a specific time_point so
    // is_cancel_already_sent TTL expiration (5000ms CEX / 1000ms PM per
    // ticker.cpp:1519) can be exercised without real wall-clock delays.
    // unacked_new_cancels is protected on TradeTicker; this derived
    // method is the test surface.
    void inject_pending_cancel(
        const std::string& cancel_key, const Core::Order& order,
        std::chrono::steady_clock::time_point time_point) {
        this->unacked_new_cancels[cancel_key] = {time_point, order};
    }
};

inline ::StrategyConfig make_disabled_strategy_config(
    int id = 1, const std::string& strategy_name = "disabled-test-strategy") {
    ::StrategyConfig db;
    db.id = id;
    db.strategy_name = strategy_name;
    db.is_disabled = true;  // escape hatch: Strategy ctor accepts any name
    db.virtual_balance = 100000.0;
    db.account_mode = "shared";
    // Wire one binance/swap entry — many ticker methods key on this.
    Core::ExchangeAndMarket em;
    em.exchange_name = "binance";
    em.market_type = "swap";
    em.account_id = 1;
    db.exchanges_list = {em};
    return db;
}

inline std::unique_ptr<Core::Strategy> make_test_strategy(int id = 1) {
    auto db = make_disabled_strategy_config(id);
    return std::make_unique<Core::Strategy>(db);
}

inline std::unique_ptr<MockTradeTicker> make_test_ticker(
    const std::string& symbol = "BTCUSDT",
    const std::string& quote_asset = "USDT",
    const std::string& base_asset = "BTC", int strategy_id = 1) {
    Core::ExchangeAndMarket em;
    em.exchange_name = "binance";
    em.market_type = "swap";
    em.account_id = 1;
    return std::make_unique<MockTradeTicker>(
        std::vector<Core::ExchangeAndMarket>{em}, symbol, quote_asset,
        base_asset, strategy_id);
}

inline Core::SymbolExchangeInfo make_symbol_info(
    const std::string& symbol, const std::string& base,
    const std::string& quote, int price_precision = 2, int amount_precision = 3,
    bool is_active = true) {
    Core::SymbolExchangeInfo info;
    info.symbol = symbol;
    info.base = base;
    info.quote = quote;
    info.type = "swap";
    info.is_active = is_active;
    info.allows_margin = true;
    info.limits.amount.min = mpfr::mpreal("0.001");
    info.limits.amount.max = mpfr::mpreal("1000000");
    info.limits.price.min = mpfr::mpreal("0.01");
    info.limits.price.max = mpfr::mpreal("10000000");
    info.limits.cost.min = mpfr::mpreal("5");
    info.limits.cost.max = mpfr::mpreal("100000000");
    // Note: precision.price is used as the TICK SIZE (double, e.g. 0.01)
    // by Engine::get_tick_size — engine.cpp:1918. precision_int.price is
    // the decimal count (int, e.g. 2) used by round_price / get_price_precision.
    // Same split for amount/step. Don't conflate them in fixtures.
    auto pow10 = [](int decimals) {
        double x = 1.0;
        for (int i = 0; i < decimals; ++i) x /= 10.0;
        return x;
    };
    info.precision.price = pow10(price_precision);   // tick size
    info.precision.amount = pow10(amount_precision); // step size
    info.precision.base = pow10(amount_precision);
    info.precision.quote = pow10(price_precision);
    info.precision_int.price = price_precision;
    info.precision_int.amount = amount_precision;
    return info;
}

inline void ensure_minimal_engine() {
    static bool initialized = false;
    if (initialized) return;

    // TradeTicker ctor reads DATA_DIR_ROOT (ticker.cpp:959) to look up
    // per-ticker persistent state. Tests don't load or write real state
    // — point at /tmp so the "file exists" branch stays false.
    if (std::getenv("DATA_DIR_ROOT") == nullptr) {
        setenv("DATA_DIR_ROOT", "/tmp/trading_engine_unit_tests_data", 1);
    }

    auto& engine = Core::Engine::getInstance();
    if (engine.isInitialized()) {
        // Another fixture already owns the Engine. We can't layer our
        // config + symbol set on top without risking precision lookups
        // that return the other fixture's values. Fail loud so whoever
        // adds a second fixture consolidates into this one.
        throw std::runtime_error(
            "TestHelpers::ensure_minimal_engine: Engine already "
            "initialized. Extend this fixture rather than creating a "
            "second one.");
    }

    mpfr::mpreal::set_default_prec(512);

    Core::EngineConfig config;
    config.is_backtest = true;
    config.is_strategy_development = true;
    config.emit_trading_logs = false;
    config.emit_logs_in_real_time = false;
    config.emit_realtime_trading_state = false;
    config.enable_engine_signals = false;
    config.enable_realtime_subscriptions = false;
    config.is_order_creation_disabled = false;
    config.enable_strategy_telemetry = false;
    config.engine_instance = "test:0";

    auto log_cb = [](const std::vector<TradingLog>&) {};
    auto snapshot_cb = [](const Core::EngineStateSnapshot&) {};

    engine.initialize(log_cb, snapshot_cb, config);
    engine.initialize_symbol_precision_access();

    std::vector<Core::SymbolExchangeInfo> symbols = {
        make_symbol_info("BTCUSDT", "BTC", "USDT", 2, 3),
        make_symbol_info("ETHUSDT", "ETH", "USDT", 2, 3),
    };
    engine.add_symbol_exchange_info("binance", "swap", symbols);

    // Flip only after init completes — if anything above throws, the
    // next call re-enters and surfaces the failure instead of silently
    // skipping into a partially-initialized engine.
    initialized = true;
}

}  // namespace TestHelpers
