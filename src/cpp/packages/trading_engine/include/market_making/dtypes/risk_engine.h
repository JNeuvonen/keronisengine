#pragma once

#include <mutex>
#include <tuple>
#include <vector>

#include "market_making/dtypes/def_only_structs.h"
#include "market_making/dtypes/order.h"

namespace Core {

class Trade;
class Engine;
class Strategy;

struct ExposureState {
    mpfr::mpreal price;
    mpfr::mpreal quantity;
};

class RiskEngine {
   private:
    std::vector<Order> orders;
    std::unordered_map<long long, std::vector<Order>> orders_by_account_id;
    std::vector<Trade> trades;
    std::unordered_map<long long, std::vector<Trade>> trades_by_account_id;
    std::unordered_map<long long, mpfr::mpreal> navs_by_account_id;

    std::unordered_map<unsigned long long, ExposureState> order_states;
    std::unordered_map<unsigned long long, ExposureState> trade_states;
    std::unordered_map<long long,
                       std::unordered_map<unsigned long long, ExposureState>>
        order_states_by_account_id;
    std::unordered_map<long long,
                       std::unordered_map<unsigned long long, ExposureState>>
        trade_states_by_account_id;

    mpfr::mpreal book_buy_exposure;
    mpfr::mpreal book_sell_exposure;
    mpfr::mpreal trade_long_exposure;
    mpfr::mpreal trade_short_exposure;

    std::unordered_map<long long, mpfr::mpreal> book_buy_exposure_by_account_id;
    std::unordered_map<long long, mpfr::mpreal>
        book_sell_exposure_by_account_id;
    std::unordered_map<long long, mpfr::mpreal>
        trade_long_exposure_by_account_id;
    std::unordered_map<long long, mpfr::mpreal>
        trade_short_exposure_by_account_id;

    std::unordered_map<std::string, mpfr::mpreal> book_buy_exposure_per_symbol;
    std::unordered_map<std::string, mpfr::mpreal> book_sell_exposure_per_symbol;
    std::unordered_map<std::string, mpfr::mpreal>
        trade_long_exposure_per_symbol;
    std::unordered_map<std::string, mpfr::mpreal>
        trade_short_exposure_per_symbol;

    std::unordered_map<long long, std::unordered_map<std::string, mpfr::mpreal>>
        book_buy_exposure_per_symbol_by_account_id;
    std::unordered_map<long long, std::unordered_map<std::string, mpfr::mpreal>>
        book_sell_exposure_per_symbol_by_account_id;
    std::unordered_map<long long, std::unordered_map<std::string, mpfr::mpreal>>
        trade_long_exposure_per_symbol_by_account_id;
    std::unordered_map<long long, std::unordered_map<std::string, mpfr::mpreal>>
        trade_short_exposure_per_symbol_by_account_id;

    mutable std::mutex mutex;
    std::chrono::time_point<std::chrono::system_clock>
        last_updated_open_trades_tp =
            std::chrono::system_clock::now() - std::chrono::seconds(30);

    std::optional<Core::Order> get_order_from_account_state(
        long long account_id, unsigned long long order_id);
    std::optional<Core::Trade> get_trade_from_account_state(
        long long account_id, unsigned long long trade_id);

    std::optional<ExposureState> get_order_state_from_account_state(
        long long account_id, unsigned long long order_id);
    std::optional<ExposureState> get_trade_state_from_account_state(
        long long account_id, unsigned long long trade_id);

   public:
    RiskEngine() = default;
    ~RiskEngine() = default;

    void add_trade(const Trade& trade);
    void add_trades(const std::vector<Trade>& trades);
    void add_orders(const std::vector<Order>& orders);
    void add_or_rm_exposures_from_order(const Order& order, bool is_reduce);
    void update_exposure_on_order(const Order& order);
    void update_exposure_on_trade(const Trade& trade, double price);
    void remove_order(unsigned long long order_id);
    void remove_trade(unsigned long long trade_id, bool acquire_lock = true);
    void update_order(const Order& order);
    void update_exposures_on_open_trades();
    mpfr::mpreal get_new_order_exposure_ratio(Engine& engine,
                                              const Strategy& strategy,
                                              const Order& order);

    mpfr::mpreal get_new_order_exposure_ratio_by_account_id(
        Engine& engine, const Strategy& strategy, const Order& order);

    void resize_to_fit_max_order_notional_quantity(double max_notional_quantity,
                                                   Order& order);
    void update_trade(const Trade& trade);
    void apply_inventory_sizing_coeff(Engine& engine, const Strategy& strategy,
                                      Order& order);
    mpfr::mpreal get_long_exposure(bool require_lock) const;
    mpfr::mpreal get_short_exposure(bool require_lock) const;
    mpfr::mpreal get_long_exposure_per_symbol(const std::string& symbol,
                                              bool require_lock) const;
    mpfr::mpreal get_short_exposure_per_symbol(const std::string& symbol,
                                               bool require_lock) const;

    bool is_order_allowed_to_pass(Engine& engine, const Order& order);
    std::tuple<OrderConstructionResult, std::optional<Order>> construct_order(Engine& engine, Strategy& strategy,
                                         const CreateOrderParams& params);
    // Commit a constructed order into risk-engine state. Called from
    // ticker.cpp::mark_order_to_be_created so registration happens at the
    // actual dispatch point, not inside construct_order. Pre-commit
    // registration was the source of the ghost-order leak: orders the caller
    // dropped (qty resized to ~0 by apply_risk_bounds_resizing, or rejected
    // by a downstream strategy-side check) used to remain in this->orders
    // forever, inflating book_*_exposure.
    void register_order(const Order& order);
    // Replace the entire risk-engine state with the projection of the
    // provided orders/trades vectors. Called by reconciliation (see
    // Engine::reconcile_risk_engine_with_tickers) to bound aggregate drift.
    // All vectors, maps, and exposure counters are rebuilt from scratch
    // under one lock acquisition.
    void rebuild_from_state(const std::vector<Order>& orders,
                            const std::vector<Trade>& trades);
    void apply_risk_bounds_resizing(Engine& engine, Strategy& strategy,
                                    Order& order);
    void remove_orders_and_trades_by_symbol(const std::string& symbol);
    void clear();
    void update_navs_by_account_id(
        const std::unordered_map<long long, mpfr::mpreal>& navs);

    const std::vector<Order> get_orders() const;
    const std::vector<Trade> get_trades() const;

    // Diag accessors — lock-free, only safe to call from sim-event handlers.
    // Used by backtest_trade_ticker.cpp to detect exposure leaks.
    mpfr::mpreal peek_trade_short_exposure() const {
        return trade_short_exposure;
    }
    mpfr::mpreal peek_trade_long_exposure() const {
        return trade_long_exposure;
    }
    mpfr::mpreal peek_book_sell_exposure() const {
        return book_sell_exposure;
    }
    mpfr::mpreal peek_book_buy_exposure() const { return book_buy_exposure; }
    // size() readers defined in risk_engine.cpp (Trade is forward-declared
    // here, so vector::size needs the complete type).
    size_t peek_trades_size() const;
    size_t peek_orders_size() const;
    size_t peek_trade_states_size() const;
    size_t peek_order_states_size() const;
};
}  // namespace Core