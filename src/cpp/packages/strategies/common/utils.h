#pragma once

#include <functional>
#include <span>
#include <string>
#include <unordered_set>

#include "market_making/dtypes/prediction.h"
#include "market_making/dtypes/strategy_decision.h"
#include "market_making/engine.h"
#include "mpreal.h"
#include "onnx_model.h"

// =============================================================================
// Shared KV Cache Keys
// =============================================================================

namespace StrategyRulesKeys {

inline const std::string VOL_PROXY = "VOL_PROXY";
inline const std::string TURNOVER_24H = "TURNOVER_24H";

}  // namespace StrategyRulesKeys

namespace MarketData {
class OhlcvDataset;
class Store;
}  // namespace MarketData

namespace Core {

class Engine;
class Strategy;

Core::Signal from_prediction(float prediction, double price,
                             long long bar_close_time_ms, Core::Engine &engine,
                             long long strategy_id,
                             const std::function<mpfr::mpreal(float prediction)>
                                 &signal_strength_callback);








std::unordered_set<std::string> get_binance_top_n_quote_asset_symbols(
    Core::Engine &engine, Core::Strategy &strategy,
    MarketData::Store &dataset_store, size_t n, size_t window_size);

std::unordered_set<std::string> get_binance_top_n_performing_assets(
    Core::Engine &engine, Core::Strategy &strategy,
    MarketData::Store &dataset_store, size_t n, size_t window_size);

bool is_trade_hold_duration_passed_rounded_klines(
    long long open_time_ms, long long current_time_ms, long long interval_ms,
    long long required_time_diff_ms);

bool is_stop_loss_triggered(double trade_open_price, double current_price,
                            double stop_loss_threshold, bool is_short_sale);

bool is_take_profit_triggered(double trade_open_price, double current_price,
                              double take_profit_threshold, bool is_short_sale);

std::optional<double> compute_market_volatility_estimate(
    std::vector<std::shared_ptr<MarketData::OhlcvDataset>> &datasets,
    long long kline_timestamp_ms, long long lookback_klines);

int get_max_ladder_orders_from_mid_distance(
    std::optional<double> price_distance_from_mid);

};  // namespace Core

namespace StrategyRules {

// Clamp a passive limit price to never cross the live BBA.
//   side == SELL: returned price >= best_bid * (1 + buffer_bp/1e4)
//   side == BUY : returned price <= best_ask * (1 - buffer_bp/1e4)
// `buffer_bp` is the minimum distance the order must keep from the opposite
// side of the book at submission time — e.g. 1.0 ⇒ at least 1 bp away.
// In backtest (`engine.is_backtest`) BBA is not modelled; returns
// `desired_price` unchanged so backtest fills against the matched bar.
// In live, if BBA is unavailable or non-positive, returns `desired_price`
// unchanged (caller is responsible for handling the missing-book case).
double clamp_price_to_bba(Core::Engine &engine, const std::string &symbol,
                         const std::string &exchange_name,
                         const std::string &market_type, Core::OrderSide side,
                         double desired_price, double buffer_bp);

void post_passive_exits_for_short_algos(
    Core::StrategyDecision &strategy_decision, long long current_time_ms,
    Core::Engine &engine, Core::Strategy &strategy,
    Core::TradeTicker &trade_ticker, long long trade_max_time_ms,
    const std::function<double(double)> &vol_proxy_transform);

void post_enter_orders_short_algos_backtest(
    long long current_time_ms, Core::Engine &engine, Core::Strategy &strategy,
    Core::StrategyDecision &strategy_decision, Core::TradeTicker &trade_ticker,
    const Core::Signal &prediction, const mpfr::mpreal &sizing_coeff_delta);

void add_enter_taker_orders_level1(
    long long current_time_ms, Core::Engine &engine, Core::Strategy &strategy,
    Core::StrategyDecision &strategy_decision, Core::TradeTicker &trade_ticker,
    const Core::Signal &prediction, const std::string &symbol,
    const Core::ExchangeAndMarket &exchange_and_market, bool is_binance_spot,
    const mpfr::mpreal &remaining_sizing_coeff_delta,
    const mpfr::mpreal &max_quote_sizing_notional);

void add_enter_taker_orders_acquire_fast(
    long long current_time_ms, Core::Engine &engine, Core::Strategy &strategy,
    Core::StrategyDecision &strategy_decision, Core::TradeTicker &trade_ticker,
    const Core::Signal &prediction, const std::string &symbol,
    const Core::ExchangeAndMarket &exchange_and_market, bool is_binance_spot,
    const mpfr::mpreal &remaining_sizing_coeff_delta,
    const mpfr::mpreal &max_quote_sizing_notional);

void post_close_trade_orders_short_algos_backtest(
    Core::StrategyDecision &strategy_decision, long long current_time_ms,
    Core::Engine &engine, Core::Strategy &strategy,
    Core::TradeTicker &trade_ticker, const Core::Signal &prediction,
    long long trade_max_time_ms,
    const std::function<double(double)> &vol_proxy_transform);

void post_enter_taker_orders_remove_level1(
    long long current_time_ms, Core::Engine &engine, Core::Strategy &strategy,
    Core::StrategyDecision &strategy_decision, Core::TradeTicker &trade_ticker,
    const Core::Signal &prediction, const mpfr::mpreal &sizing_coeff_delta,
    const std::vector<Core::ExchangeAndMarket> &available_markets);

void post_enter_taker_orders_acquire_fast(
    long long current_time_ms, Core::Engine &engine, Core::Strategy &strategy,
    Core::StrategyDecision &strategy_decision, Core::TradeTicker &trade_ticker,
    const Core::Signal &prediction, const mpfr::mpreal &sizing_coeff_delta,
    const std::vector<Core::ExchangeAndMarket> &available_markets);

void cancel_expired_orders(long long current_time_ms,
                           Core::StrategyDecision &strategy_decision,
                           Core::Engine &engine,
                           Core::TradeTicker &trade_ticker);

bool cancel_all_orders_if_stop_making_markets(
    Core::StrategyDecision &strategy_decision, Core::Engine &engine,
    Core::Strategy &strategy, Core::TradeTicker &trade_ticker);

}  // namespace StrategyRules
