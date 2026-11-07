#pragma once

#include <optional>
#include <string>
#include <unordered_map>

#include "market_making/dtypes/enums.h"

namespace Core {

enum TickSizeIncrementStrategy {
    DEFAULT_STRATEGY = 0,
};

enum OrderSizeStrategy {
    FROM_DAILY_TURNOVER = 0,
};

struct OrderLadderParams {
    TickSizeIncrementStrategy price_change_strategy;
    OrderSizeStrategy size_strategy;
    std::string exchange_name;
    std::string market_type;
    double start_price;
    double reference_price;
    double order_max_notional_quantity = 500;
    int max_iterations = 500;
    Core::OrderSide side;
    Core::OrderTypeInternal type_internal;
    bool reduce_only = false;
    bool is_trade_close_order;
    bool margin_auto_borrow = true;
    bool margin_auto_repay = true;
    long long expires_on_delta_to_present_time_ms;
    std::optional<double> daily_turnover = std::nullopt;

    bool is_randomized_size = false;
    bool is_randomized_delay_between_orders = false;
    std::optional<double> randomized_size_lower_bound = std::nullopt;
    std::optional<double> randomized_size_upper_bound = std::nullopt;
    std::optional<long long> randomized_delay_between_orders_ms_lower_bound =
        std::nullopt;
    std::optional<long long> randomized_delay_between_orders_ms_upper_bound =
        std::nullopt;
    bool is_maximum_slippage_enabled = false;
    std::optional<double> signal_start_price = std::nullopt;
    std::optional<double> maximum_slippage_coeff = std::nullopt;
    bool prevent_overlapping_orders = false;
    bool limit_number_of_open_orders = false;
    std::optional<int> max_number_of_open_orders = std::nullopt;
    bool ensure_post_only = false;
    std::optional<double> best_bid_price = std::nullopt;
    std::optional<double> best_ask_price = std::nullopt;
    bool randomize_expiration_time = false;
    std::optional<long long> randomized_expiration_time_ms_lower_bound =
        std::nullopt;
    std::optional<long long> randomized_expiration_time_ms_upper_bound =
        std::nullopt;
    bool prevent_price_exceeding_threshold = false;
    std::optional<double> safe_price_above_current_price_threshold =
        std::nullopt;
    bool automatically_add_orderbook_prices_to_params = false;
    bool limit_strategy_level_enter_order_rate = false;
    bool limit_ticker_level_enter_order_rate = false;
    bool limit_strategy_level_exit_order_rate = false;
    bool limit_ticker_level_exit_order_rate = false;
    std::optional<int> max_strategy_level_enter_orders_per_10s = std::nullopt;
    std::optional<int> max_ticker_level_enter_orders_per_10s = std::nullopt;
    std::optional<int> max_strategy_level_exit_orders_per_10s = std::nullopt;
    std::optional<int> max_ticker_level_exit_orders_per_10s = std::nullopt;
    bool only_post_if_price_within_threshold = false;
    std::optional<double> price_within_threshold_percent = std::nullopt;
    bool limit_max_number_of_orders_in_ladder = false;
    std::optional<int> max_number_of_orders_in_ladder = std::nullopt;
    double ensure_minimum_order_size_notional = 0;
    std::optional<double> max_notional_resting_on_book = std::nullopt;
    std::optional<int> max_unacked_enter_orders = std::nullopt;
    std::optional<int> max_unacked_exit_orders = std::nullopt;
    std::unordered_map<std::string, double> kv_cache = {};
};

}  // namespace Core
