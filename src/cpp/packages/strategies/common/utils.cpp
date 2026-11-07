#include "strategies/common/utils.h"

#include <iostream>
#include <limits>

#include "market_data/bar_types_constant.h"
#include "market_data/utils.h"
#include "market_making/dtypes/exchange_info.h"
#include "market_making/dtypes/global_general_cache.h"
#include "market_making/dtypes/order_ladder_params.h"
#include "market_making/engine.h"
#include "market_making/strategy.h"
#include "market_making/ticker.h"
#include "market_making/utils.h"
#include "technical_analysis.h"

namespace Core {

Core::Signal from_prediction(float prediction, double price,
                             long long bar_close_time_ms, Core::Engine &engine,
                             long long strategy_id,
                             const std::function<mpfr::mpreal(float prediction)>
                                 &signal_strength_callback) {
    return {
        .signal_strength = signal_strength_callback(prediction),
        .prediction = prediction,
        .price = price,
        .event_time_ms = engine.get_time(),
        .bar_close_time_ms = bar_close_time_ms,
        .kv_cache = {},
        .signal_id = Common::generate_timestamp_uuid(),
        .strategy_id = strategy_id,
    };
}

std::unordered_set<std::string> get_binance_top_n_performing_assets(
    Core::Engine &engine, Core::Strategy &strategy,
    MarketData::Store &dataset_store, size_t n, size_t window_size) {
    std::unordered_set<std::string> top_n_performing_assets;

    std::vector<std::pair<std::string, double>> performing_assets_pairs;
    bool is_backtest = engine.is_backtest;

    for (const auto &trade_ticker : strategy.trade_tickers) {
        const auto &key = ::MarketData::get_derived_bars_base_dir(
            trade_ticker.second->symbol, "binance", "swap",
            MarketData::FIFTEEN_MIN_EXCHANGE_BAR, true);

        const auto &pointer_to_dataset =
            dataset_store.get_synced_ohlcv_dataset(key, is_backtest);

        if (pointer_to_dataset.has_value()) {
            const auto &dataset = pointer_to_dataset.value();
            const auto latest_bar_close_time_ms =
                dataset->get_latest_bar_close_time_ms();
            if (!latest_bar_close_time_ms.has_value()) {
                continue;
            }
            if (!ChronoUtils::is_two_ts_on_same_interval(
                    engine.get_time(), latest_bar_close_time_ms.value(),
                    900000)) {
                continue;
            }
            const auto close_prices = dataset->get_close_prices();

            if (close_prices.size() < window_size + 50) {
                continue;
            }

            const std::span<const double> close_prices_span =
                std::span<const double>(close_prices);

            const auto n_rocp =
                TechnicalAnalysis::rocp_ratio(close_prices_span, window_size);
            performing_assets_pairs.push_back(
                {trade_ticker.second->symbol, n_rocp});
        }
    }

    std::sort(performing_assets_pairs.begin(), performing_assets_pairs.end(),
              [](const auto &a, const auto &b) { return a.second > b.second; });

    for (size_t i = 0; i < performing_assets_pairs.size(); i++) {
        if (top_n_performing_assets.size() >= n) {
            break;
        }
        top_n_performing_assets.insert(performing_assets_pairs[i].first);
    }

    return top_n_performing_assets;
}

std::unordered_set<std::string> get_binance_top_n_quote_asset_symbols(
    Core::Engine &engine, Core::Strategy &strategy,
    MarketData::Store &dataset_store, size_t n, size_t window_size) {
    std::unordered_set<std::string> top_n_quote_asset_symbols;

    std::vector<std::pair<std::string, double>> quote_asset_volume_pairs;
    bool is_backtest = engine.is_backtest;

    for (const auto &trade_ticker : strategy.trade_tickers) {
        const auto &key = ::MarketData::get_derived_bars_base_dir(
            trade_ticker.second->symbol, "binance", "swap",
            MarketData::FIFTEEN_MIN_EXCHANGE_BAR, true);

        const auto &pointer_to_dataset =
            dataset_store.get_synced_ohlcv_dataset(key, is_backtest);

        if (pointer_to_dataset.has_value()) {
            const auto &dataset = pointer_to_dataset.value();
            const auto latest_bar_close_time_ms =
                dataset->get_latest_bar_close_time_ms();
            if (!latest_bar_close_time_ms.has_value()) {
                continue;
            }
            if (!ChronoUtils::is_two_ts_on_same_interval(
                    engine.get_time(), latest_bar_close_time_ms.value(),
                    900000)) {
                continue;
            }
            const auto quote_asset_volumes = dataset->get_quote_asset_volumes();

            const size_t volumes_size = quote_asset_volumes.size();

            if (volumes_size < window_size) {
                continue;
            }

            const size_t start_index =
                volumes_size >= window_size ? volumes_size - window_size : 0;

            double last_24_quote_asset_vol = 0.0;

            for (size_t i = start_index; i < volumes_size; ++i) {
                last_24_quote_asset_vol += quote_asset_volumes[i];
            }

            quote_asset_volume_pairs.push_back(
                {trade_ticker.second->symbol, last_24_quote_asset_vol});
        }
    }

    std::sort(quote_asset_volume_pairs.begin(), quote_asset_volume_pairs.end(),
              [](const auto &a, const auto &b) { return a.second > b.second; });

    for (size_t i = 0; i < quote_asset_volume_pairs.size(); i++) {
        if (top_n_quote_asset_symbols.size() >= n) {
            break;
        }
        top_n_quote_asset_symbols.insert(quote_asset_volume_pairs[i].first);
    }

    return top_n_quote_asset_symbols;
}

bool is_trade_hold_duration_passed_rounded_klines(
    long long open_time_ms, long long current_time_ms, long long interval_ms,
    long long required_time_diff_ms) {
    const long long floored_open_time =
        (open_time_ms / interval_ms) * interval_ms;
    const long long floored_current_time =
        (current_time_ms / interval_ms) * interval_ms;

    return (floored_current_time - floored_open_time) >= required_time_diff_ms;
}

bool is_stop_loss_triggered(double trade_open_price, double current_price,
                            double stop_loss_threshold, bool is_short_sale) {
    if (is_short_sale) {
        return current_price > trade_open_price * (1 + stop_loss_threshold);
    } else {
        return current_price < trade_open_price * (1 - stop_loss_threshold);
    }
}

bool is_take_profit_triggered(double trade_open_price, double current_price,
                              double take_profit_threshold,
                              bool is_short_sale) {
    if (is_short_sale) {
        return current_price < trade_open_price * (1 - take_profit_threshold);
    } else {
        return current_price > trade_open_price * (1 + take_profit_threshold);
    }
}

std::optional<double> compute_market_volatility_estimate(
    std::vector<std::shared_ptr<MarketData::OhlcvDataset>> &datasets,
    long long kline_timestamp_ms, long long lookback_klines) {
    std::vector<double> normalized_volatilities;

    if (!::Core::Utils::is_all_time_based_datasets_synced(datasets,
                                                          kline_timestamp_ms)) {
        return std::nullopt;
    }

    std::string cache_key =
        "market_vol_estimate:" + std::to_string(kline_timestamp_ms) + ":" +
        std::to_string(lookback_klines);

    for (const auto &dataset : datasets) {
        cache_key += dataset->identifier;
    }

    const auto &cached_value = Core::GlobalGeneralCache::get<double>(cache_key);
    if (cached_value.has_value()) {
        return cached_value.value();
    }

    for (const auto &dataset : datasets) {
        auto normalized_volatility =
            dataset->compute_feature([&](MarketData::OhlcvDataset *dataset) {
                return TechnicalAnalysis::normalized_volatility(
                    std::span<const double>(dataset->close_prices),
                    lookback_klines);
            });
        if (!normalized_volatility.has_value()) {
            return std::nullopt;
        }
        normalized_volatilities.push_back(normalized_volatility.value());
    }

    double mean_normalized_volatility = 0.0;
    for (const auto &vol : normalized_volatilities) {
        mean_normalized_volatility += vol;
    }
    mean_normalized_volatility /= normalized_volatilities.size();

    Core::GlobalGeneralCache::insert(cache_key, mean_normalized_volatility);
    return mean_normalized_volatility;
}

int get_max_ladder_orders_from_mid_distance(
    std::optional<double> price_distance_from_mid) {
    if (!price_distance_from_mid.has_value()) {
        return 1;
    }

    double price_distance_from_mid_value = price_distance_from_mid.value();

    if (price_distance_from_mid_value > 0.05) {
        return 1;
    }

    if (price_distance_from_mid_value > 0.03) {
        return 4;
    }

    if (price_distance_from_mid_value > 0.01) {
        return 8;
    }

    return 15;
}

}  // namespace Core

namespace StrategyRules {

double clamp_price_to_bba(Core::Engine &engine, const std::string &symbol,
                          const std::string &exchange_name,
                          const std::string &market_type, Core::OrderSide side,
                          double desired_price, double buffer_bp) {
    if (engine.is_backtest) return desired_price;
    const double buffer = buffer_bp / 1e4;
    if (side == Core::OrderSide::SELL) {
        const auto best_bid =
            engine.get_best_bid(symbol, exchange_name, market_type);
        if (!best_bid.has_value() || best_bid.value() <= 0) return desired_price;
        const double floor_px = best_bid.value() * (1.0 + buffer);
        return desired_price < floor_px ? floor_px : desired_price;
    }
    const auto best_ask =
        engine.get_best_ask(symbol, exchange_name, market_type);
    if (!best_ask.has_value() || best_ask.value() <= 0) return desired_price;
    const double ceil_px = best_ask.value() * (1.0 - buffer);
    return desired_price > ceil_px ? ceil_px : desired_price;
}

static void add_passive_exit_orders_gateio_15m_short_algos(
    Core::StrategyDecision &strategy_decision, long long current_time_ms,
    Core::Engine &engine, Core::Strategy &strategy,
    Core::TradeTicker &trade_ticker,
    const std::optional<Core::Signal> &prediction, const Core::Trade &trade,
    long long trade_max_time_ms,
    const std::function<double(double)> &vol_proxy_transform) {
    const auto &remaining_quantity =
        trade_ticker.get_trade_remaining_qty(trade);

    if (remaining_quantity <= 0) {
        return;
    }

    double enter_price = trade.get_average_open_price().toDouble();

    if (enter_price == 0 ||
        enter_price == std::numeric_limits<double>::infinity()) {
        return;
    }

    auto it_vol_proxy = trade.kv_cache.find(StrategyRulesKeys::VOL_PROXY);
    if (it_vol_proxy == trade.kv_cache.end()) {
        std::cout << "No VOL_PROXY found" << std::endl;
        return;
    }

    double vol_proxy = it_vol_proxy->second;

    long long trade_open_duration = trade.get_time_since_open(engine);
    if (trade_open_duration < 60000 * 2) {
        return;
    }

    double vol_proxy_transformed = vol_proxy_transform(vol_proxy);
    double price_coeff =
        Core::Utils::calc_price_coeff_from_vol_and_trade_time_remaining_v2(
            vol_proxy_transformed, trade_open_duration, trade_max_time_ms,
            true);
    double take_profit_first_price = enter_price * price_coeff;

    auto it_turnover = trade.kv_cache.find(StrategyRulesKeys::TURNOVER_24H);
    if (it_turnover == trade.kv_cache.end()) {
        return;
    }

    double daily_turnover = it_turnover->second;

    const auto price_distance_from_mid = engine.get_price_distance_from_mid(
        trade_ticker.symbol, trade.exchange_name, trade.market_type,
        enter_price);
    bool is_within_1pct_of_mid = price_distance_from_mid.has_value() &&
                                 price_distance_from_mid.value() <= 0.01;

    auto ms_since_last_ob_update = engine.get_ms_since_last_orderbook_update(
        trade_ticker.symbol, trade.exchange_name, trade.market_type);
    auto best_bid_ask = engine.get_best_bid_ask_with_qty(
        trade_ticker.symbol, trade.exchange_name, trade.market_type);

    std::unordered_map<std::string, double> debug_kv_cache = {
        {"take_profit_first_price", take_profit_first_price},
        {"enter_price", enter_price},
        {"vol_proxy", vol_proxy},
        {"vol_proxy_transformed", vol_proxy_transformed},
        {"price_coeff", price_coeff},
        {"trade_open_duration_ms", static_cast<double>(trade_open_duration)},
        {"trade_max_time_ms", static_cast<double>(trade_max_time_ms)},
        {"current_time_ms", static_cast<double>(current_time_ms)},
        {"remaining_quantity", remaining_quantity.toDouble()},
        {"daily_turnover", daily_turnover},
        {"ms_since_last_ob_update",
         ms_since_last_ob_update.has_value()
             ? static_cast<double>(ms_since_last_ob_update.value())
             : -1.0},
        {"best_bid_price",
         best_bid_ask.has_value() ? best_bid_ask->bid_price : -1.0},
        {"best_ask_price",
         best_bid_ask.has_value() ? best_bid_ask->ask_price : -1.0},
        {"price_distance_from_mid", price_distance_from_mid.has_value()
                                        ? price_distance_from_mid.value()
                                        : -1.0},
        {"max_ladder_orders",
         static_cast<double>(Core::get_max_ladder_orders_from_mid_distance(
             price_distance_from_mid))},
    };

    Core::OrderLadderParams lp{
        .price_change_strategy =
            Core::TickSizeIncrementStrategy::DEFAULT_STRATEGY,
        .size_strategy = Core::OrderSizeStrategy::FROM_DAILY_TURNOVER,
        .exchange_name = trade.exchange_name,
        .market_type = trade.market_type,
        .start_price = take_profit_first_price,
        .reference_price = enter_price,
        .side = Core::OrderSide::BUY,
        .type_internal = Core::OrderTypeInternal::PASSIVE_TAKE_PROFIT,
        .is_trade_close_order = true,
        .expires_on_delta_to_present_time_ms = 1000 * 60 * 5,
        .daily_turnover = daily_turnover,
        .prevent_overlapping_orders = true,
        .randomize_expiration_time = true,
        .randomized_expiration_time_ms_lower_bound =
            std::make_optional<long long>(1000LL * 60LL * 1LL),
        .randomized_expiration_time_ms_upper_bound =
            std::make_optional<long long>(1000LL * 60LL * 5LL),
        .prevent_price_exceeding_threshold = true,
        .safe_price_above_current_price_threshold = 0.0025,
        .automatically_add_orderbook_prices_to_params = true,
        .limit_ticker_level_exit_order_rate = !is_within_1pct_of_mid,
        .max_ticker_level_exit_orders_per_10s = 2,
        .limit_max_number_of_orders_in_ladder = true,
        .max_number_of_orders_in_ladder =
            Core::get_max_ladder_orders_from_mid_distance(
                price_distance_from_mid),
        .ensure_minimum_order_size_notional = 15,
        .max_notional_resting_on_book = 200,
        .kv_cache = debug_kv_cache,
    };

    Core::Utils::create_order_ladder(
        strategy_decision, current_time_ms, engine, strategy, trade_ticker,
        std::make_optional(trade), prediction.value_or(Core::Signal{}),
        remaining_quantity.toDouble(), lp);
}

void post_passive_exits_for_short_algos(
    Core::StrategyDecision &strategy_decision, long long current_time_ms,
    Core::Engine &engine, Core::Strategy &strategy,
    Core::TradeTicker &trade_ticker, long long trade_max_time_ms,
    const std::function<double(double)> &vol_proxy_transform) {
    const auto trades = trade_ticker.get_trades(false);
    const auto prediction = trade_ticker.get_prediction();

    for (const auto &trade : trades) {
        const auto &remaining_quantity =
            trade_ticker.get_trade_remaining_qty(trade);

        if (remaining_quantity > 0) {
            add_passive_exit_orders_gateio_15m_short_algos(
                strategy_decision, current_time_ms, engine, strategy,
                trade_ticker, prediction, trade, trade_max_time_ms,
                vol_proxy_transform);
        }
    }
}

void post_enter_orders_short_algos_backtest(
    long long current_time_ms, Core::Engine &engine, Core::Strategy &strategy,
    Core::StrategyDecision &strategy_decision, Core::TradeTicker &trade_ticker,
    const Core::Signal &prediction, const mpfr::mpreal &sizing_coeff_delta) {
    size_t num_exchanges = 1;

    const Core::ExchangeAndMarket exchange_and_market{"gateio", "spot"};
    const std::string &symbol = trade_ticker.symbol;

    if (trade_ticker.get_num_enter_orders(exchange_and_market.exchange_name,
                                          exchange_and_market.market_type) >
        0) {
        return;
    }

    auto max_quote_sizing_notional =
        strategy.get_strategy_trade_size(engine,
                                         exchange_and_market.exchange_name,
                                         exchange_and_market.market_type) *
        sizing_coeff_delta;

    if (max_quote_sizing_notional > 5) {
        const auto price = prediction.price;

        const auto order_quantity_info =
            Core::Utils::calculate_exchange_distributed_quantity_info(
                max_quote_sizing_notional, 1.0, sizing_coeff_delta,
                num_exchanges, price);

        if (order_quantity_info.quantity <= 0) {
            return;
        }

        Core::CreateOrderParams params{
            .symbol = trade_ticker.symbol,
            .type = Core::OrderType::LIMIT,
            .side = Core::OrderSide::SELL,
            .price = prediction.price,
            .prediction = prediction,
            .quantity = order_quantity_info.quantity,
            .exchange_name = exchange_and_market.exchange_name,
            .market_type = exchange_and_market.market_type,
            .strategy_id = strategy.strategy_config.id,
            .expires_on_ms = current_time_ms + 60000 * 2,
            .sizing_coeff_delta = order_quantity_info.sizing_coeff_delta,
            .trade_id = std::nullopt,
            .reduce_only = false,
            .timestamp = current_time_ms,
            .quote_asset = trade_ticker.quote_asset,
            .base_asset = trade_ticker.base_asset,
            .allowed_to_bypass_risk_engine = false,
            .must_pass_min_notional_filter = true,
            .is_trade_close_order = false,
            .is_alt_market_routed = false,
            .route_order_to_alt_symbol = symbol,
            .routed_order_base_asset = trade_ticker.base_asset,
            .routed_order_quote_asset = "USDT",
            .margin_auto_borrow = true,
            .margin_auto_repay = true,
            .kv_cache = prediction.kv_cache,
            .max_unacked_enter_orders = 1,
        };

        auto [result, order] =
            trade_ticker.construct_order(engine, strategy, params);

        if (result != Core::OrderConstructionResult::VALID ||
            !order.has_value()) {
            return;
        }

        trade_ticker.mark_order_to_be_created(engine, order.value(),
                                              strategy_decision, strategy);
    }
}

void add_enter_taker_orders_level1(
    long long current_time_ms, Core::Engine &engine, Core::Strategy &strategy,
    Core::StrategyDecision &strategy_decision, Core::TradeTicker &trade_ticker,
    const Core::Signal &prediction, const std::string &symbol,
    const Core::ExchangeAndMarket &exchange_and_market, bool is_binance_spot,
    const mpfr::mpreal &remaining_sizing_coeff_delta,
    const mpfr::mpreal &max_quote_sizing_notional) {
    if (remaining_sizing_coeff_delta <= 0) {
        return;
    }

    const auto bid_ask_with_qty = engine.get_best_bid_ask_with_qty(
        symbol, exchange_and_market.exchange_name,
        exchange_and_market.market_type, 5000);

    if (!bid_ask_with_qty.has_value()) {
        return;
    }

    double target_notional = max_quote_sizing_notional.toDouble() *
                             remaining_sizing_coeff_delta.toDouble();

    if (target_notional <= 0) {
        return;
    }

    double quantity = target_notional / bid_ask_with_qty->bid_price;
    double bid_quantity_notional =
        bid_ask_with_qty->bid_qty * bid_ask_with_qty->bid_price;

    Core::CreateOrderParams params{
        .symbol = trade_ticker.symbol,
        .type = Core::OrderType::LIMIT,
        .side = Core::OrderSide::SELL,
        .price = bid_ask_with_qty->bid_price,
        .prediction = prediction,
        .quantity = quantity,
        .exchange_name = exchange_and_market.exchange_name,
        .market_type = exchange_and_market.market_type,
        .strategy_id = strategy.strategy_config.id,
        .expires_on_ms = current_time_ms + 60000,
        .sizing_coeff_delta = remaining_sizing_coeff_delta,
        .trade_id = std::nullopt,
        .reduce_only = false,
        .timestamp = current_time_ms,
        .quote_asset = trade_ticker.quote_asset,
        .base_asset = trade_ticker.base_asset,
        .allowed_to_bypass_risk_engine = false,
        .must_pass_min_notional_filter = true,
        .is_trade_close_order = false,
        .is_alt_market_routed = is_binance_spot,
        .route_order_to_alt_symbol = symbol,
        .routed_order_base_asset = trade_ticker.base_asset,
        .routed_order_quote_asset = is_binance_spot ? "USDC" : "USDT",
        .margin_auto_borrow = true,
        .margin_auto_repay = true,
        .maximum_notional_quantity =
            !engine.is_backtest ? 500.0
                                : std::numeric_limits<double>::infinity(),
        .kv_cache = prediction.kv_cache,
        .is_randomized_size = true,
        .is_randomized_delay_between_orders = true,
        .randomized_size_lower_bound = bid_quantity_notional * 0.3,
        .randomized_size_upper_bound = bid_quantity_notional * 0.9,
        .randomized_delay_between_orders_ms_lower_bound = 5000,
        .randomized_delay_between_orders_ms_upper_bound = 12500,
        .is_maximum_slippage_enabled = true,
        .signal_start_price = prediction.price,
        .maximum_slippage_coeff = 0.02,
        .max_unacked_enter_orders = 1,
    };

    auto [result, order] =
        trade_ticker.construct_order(engine, strategy, params);
    if (result != Core::OrderConstructionResult::VALID || !order.has_value()) {
        return;
    }
    trade_ticker.mark_order_to_be_created(engine, order.value(),
                                          strategy_decision, strategy);
}

void add_enter_taker_orders_acquire_fast(
    long long current_time_ms, Core::Engine &engine, Core::Strategy &strategy,
    Core::StrategyDecision &strategy_decision, Core::TradeTicker &trade_ticker,
    const Core::Signal &prediction, const std::string &symbol,
    const Core::ExchangeAndMarket &exchange_and_market, bool is_binance_spot,
    const mpfr::mpreal &remaining_sizing_coeff_delta,
    const mpfr::mpreal &max_quote_sizing_notional) {
    if (remaining_sizing_coeff_delta <= 0) {
        return;
    }

    const auto bid_ask_with_qty = engine.get_best_bid_ask_with_qty(
        symbol, exchange_and_market.exchange_name,
        exchange_and_market.market_type, 5000);

    if (!bid_ask_with_qty.has_value()) {
        return;
    }

    double target_notional = max_quote_sizing_notional.toDouble() *
                             remaining_sizing_coeff_delta.toDouble();

    if (target_notional <= 0) {
        return;
    }

    double quantity = target_notional / bid_ask_with_qty->bid_price;
    double bid_quantity_notional =
        bid_ask_with_qty->bid_qty * bid_ask_with_qty->bid_price;

    Core::CreateOrderParams params{
        .symbol = trade_ticker.symbol,
        .type = Core::OrderType::LIMIT,
        .side = Core::OrderSide::SELL,
        .price = bid_ask_with_qty->bid_price * 0.999,
        .prediction = prediction,
        .quantity = quantity,
        .exchange_name = exchange_and_market.exchange_name,
        .market_type = exchange_and_market.market_type,
        .strategy_id = strategy.strategy_config.id,
        .expires_on_ms = current_time_ms + 60000,
        .sizing_coeff_delta = remaining_sizing_coeff_delta,
        .trade_id = std::nullopt,
        .reduce_only = false,
        .timestamp = current_time_ms,
        .quote_asset = trade_ticker.quote_asset,
        .base_asset = trade_ticker.base_asset,
        .allowed_to_bypass_risk_engine = false,
        .must_pass_min_notional_filter = true,
        .is_trade_close_order = false,
        .is_alt_market_routed = is_binance_spot,
        .route_order_to_alt_symbol = symbol,
        .routed_order_base_asset = trade_ticker.base_asset,
        .routed_order_quote_asset = is_binance_spot ? "USDC" : "USDT",
        .margin_auto_borrow = true,
        .margin_auto_repay = true,
        .maximum_notional_quantity =
            !engine.is_backtest ? 500.0
                                : std::numeric_limits<double>::infinity(),
        .kv_cache = prediction.kv_cache,
        .is_randomized_size = true,
        .is_randomized_delay_between_orders = true,
        .randomized_size_lower_bound = 100,
        .randomized_size_upper_bound = 250,
        .randomized_delay_between_orders_ms_lower_bound = 1000,
        .randomized_delay_between_orders_ms_upper_bound = 2500,
        .is_maximum_slippage_enabled = true,
        .signal_start_price = prediction.price,
        .maximum_slippage_coeff = 0.01,
        .max_unacked_enter_orders = 1,
    };

    auto [result, order] =
        trade_ticker.construct_order(engine, strategy, params);
    if (result != Core::OrderConstructionResult::VALID || !order.has_value()) {
        return;
    }
    trade_ticker.mark_order_to_be_created(engine, order.value(),
                                          strategy_decision, strategy);
}

void post_close_trade_orders_short_algos_backtest(
    Core::StrategyDecision &strategy_decision, long long current_time_ms,
    Core::Engine &engine, Core::Strategy &strategy,
    Core::TradeTicker &trade_ticker, const Core::Signal &prediction,
    long long trade_max_time_ms,
    const std::function<double(double)> &vol_proxy_transform) {
    const auto trades = trade_ticker.get_trades(false);

    for (const auto &trade : trades) {
        const auto &remaining_quantity =
            trade_ticker.get_trade_remaining_qty(trade);

        if (remaining_quantity > 0) {
            double enter_price = trade.get_average_open_price().toDouble();
            if (enter_price == 0 ||
                enter_price == std::numeric_limits<double>::infinity()) {
                continue;
            }

            auto it_vol_proxy =
                trade.kv_cache.find(StrategyRulesKeys::VOL_PROXY);
            if (it_vol_proxy == trade.kv_cache.end()) {
                continue;
            }

            double vol_proxy = it_vol_proxy->second;

            long long trade_open_duration = trade.get_time_since_open(engine);

            double take_profit_price =
                enter_price *
                Core::Utils::
                    calc_price_coeff_from_vol_and_trade_time_remaining_v2(
                        vol_proxy_transform(vol_proxy), trade_open_duration,
                        trade_max_time_ms, true);

            bool price_is_good = prediction.price <= take_profit_price;

            if (!price_is_good) {
                continue;
            }

            bool is_binance_spot_trade = trade.exchange_name == "binance";

            Core::CreateOrderParams params{
                .symbol = trade_ticker.symbol,
                .type = Core::OrderType::LIMIT,
                .side = Core::OrderSide::BUY,
                .price = prediction.price,
                .prediction = prediction,
                .quantity = remaining_quantity,
                .exchange_name = trade.exchange_name,
                .market_type = trade.market_type,
                .strategy_id = strategy.strategy_config.id,
                .expires_on_ms = current_time_ms + 60000 * 2,
                .sizing_coeff_delta = remaining_quantity / trade.open_quantity,
                .trade_id = trade.id,
                .reduce_only = false,
                .timestamp = current_time_ms,
                .quote_asset = trade_ticker.quote_asset,
                .base_asset = trade_ticker.base_asset,
                .allowed_to_bypass_risk_engine = false,
                .must_pass_min_notional_filter = true,
                .is_trade_close_order = true,
                .is_alt_market_routed = is_binance_spot_trade,
                .route_order_to_alt_symbol =
                    trade_ticker.base_asset +
                    (is_binance_spot_trade ? "USDC" : "USDT"),
                .routed_order_base_asset = trade_ticker.base_asset,
                .routed_order_quote_asset =
                    is_binance_spot_trade ? "USDC" : "USDT",
                .margin_auto_borrow = true,
                .margin_auto_repay = true,
                .maximum_notional_quantity =
                    std::numeric_limits<double>::infinity(),
                .is_randomized_size = true,
                .is_randomized_delay_between_orders = true,
                .randomized_size_lower_bound = 50,
                .randomized_size_upper_bound = 200,
                .randomized_delay_between_orders_ms_lower_bound = 1000,
                .randomized_delay_between_orders_ms_upper_bound = 2500};

            auto [result, order] =
                trade_ticker.construct_order(engine, strategy, params);
            if (result != Core::OrderConstructionResult::VALID ||
                !order.has_value()) {
                continue;
            }
            trade_ticker.mark_order_to_be_created(engine, order.value(),
                                                  strategy_decision, strategy);
        }
    }
}

void post_enter_taker_orders_remove_level1(
    long long current_time_ms, Core::Engine &engine, Core::Strategy &strategy,
    Core::StrategyDecision &strategy_decision, Core::TradeTicker &trade_ticker,
    const Core::Signal &prediction, const mpfr::mpreal &sizing_coeff_delta,
    const std::vector<Core::ExchangeAndMarket> &available_markets) {
    if (engine.is_backtest) {
        post_enter_orders_short_algos_backtest(
            current_time_ms, engine, strategy, strategy_decision, trade_ticker,
            prediction, sizing_coeff_delta);
        return;
    }

    for (const auto &exchange_and_market : available_markets) {
        bool is_binance_spot = exchange_and_market.exchange_name == "binance" &&
                               exchange_and_market.market_type == "spot";

        const std::string &symbol = !is_binance_spot
                                        ? trade_ticker.symbol
                                        : trade_ticker.base_asset + "USDC";

        if (!Core::ExchangeInfo::getInstance().is_symbol_active(
                symbol, exchange_and_market.exchange_name,
                exchange_and_market.market_type)) {
            continue;
        }

        auto max_quote_sizing_notional = strategy.get_strategy_trade_size(
            engine, exchange_and_market.exchange_name,
            exchange_and_market.market_type);

        add_enter_taker_orders_level1(
            current_time_ms, engine, strategy, strategy_decision, trade_ticker,
            prediction, symbol, exchange_and_market, is_binance_spot,
            sizing_coeff_delta, max_quote_sizing_notional);
    }
}

void post_enter_taker_orders_acquire_fast(
    long long current_time_ms, Core::Engine &engine, Core::Strategy &strategy,
    Core::StrategyDecision &strategy_decision, Core::TradeTicker &trade_ticker,
    const Core::Signal &prediction, const mpfr::mpreal &sizing_coeff_delta,
    const std::vector<Core::ExchangeAndMarket> &available_markets) {
    if (engine.is_backtest) {
        post_enter_orders_short_algos_backtest(
            current_time_ms, engine, strategy, strategy_decision, trade_ticker,
            prediction, sizing_coeff_delta);
        return;
    }

    for (const auto &exchange_and_market : available_markets) {
        bool is_binance_spot = exchange_and_market.exchange_name == "binance" &&
                               exchange_and_market.market_type == "spot";

        const std::string &symbol = !is_binance_spot
                                        ? trade_ticker.symbol
                                        : trade_ticker.base_asset + "USDC";

        if (!Core::ExchangeInfo::getInstance().is_symbol_active(
                symbol, exchange_and_market.exchange_name,
                exchange_and_market.market_type)) {
            continue;
        }

        auto max_quote_sizing_notional = strategy.get_strategy_trade_size(
            engine, exchange_and_market.exchange_name,
            exchange_and_market.market_type);

        add_enter_taker_orders_acquire_fast(
            current_time_ms, engine, strategy, strategy_decision, trade_ticker,
            prediction, symbol, exchange_and_market, is_binance_spot,
            sizing_coeff_delta, max_quote_sizing_notional);
    }
}

void cancel_expired_orders(long long current_time_ms,
                           Core::StrategyDecision &strategy_decision,
                           Core::Engine &engine,
                           Core::TradeTicker &trade_ticker) {
    for (Core::Order &order : trade_ticker.get_active_orders_mut_ref()) {
        if (order.expires_on_ms <= 0 ||
            order.expires_on_ms >= current_time_ms) {
            continue;
        }
        if (trade_ticker.is_cancel_already_sent(order)) {
            continue;
        }
        trade_ticker.mark_order_to_be_cancelled(engine, order,
                                                strategy_decision);
    }
}

bool cancel_all_orders_if_stop_making_markets(
    Core::StrategyDecision &strategy_decision, Core::Engine &engine,
    Core::Strategy &strategy, Core::TradeTicker &trade_ticker) {
    if (!strategy.stop_making_markets.load()) return false;

    auto orders = trade_ticker.get_orders(false);
    for (auto &order : orders) {
        if (trade_ticker.is_cancel_already_sent(order)) continue;
        trade_ticker.mark_order_to_be_cancelled(engine, order,
                                                strategy_decision);
    }
    return true;
}

}  // namespace StrategyRules
