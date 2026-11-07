
#pragma once

#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <vector>

#include "common_types/active_trade.h"
#include "mpreal.h"
#include "ohlcv_dataset_tick.h"

struct TradeTicker {
    std::string symbol;
    std::string base_asset;
    std::string quote_asset;
    double prediction;
    mpfr::mpreal new_sizing_coeff;
    mpfr::mpreal current_sizing_coeff;
    std::span<const MarketData::OhlcvDatasetTick> klines;
    std::span<const double> close_prices;
    std::span<const long long> kline_close_times_ms;
    int id;
    int strategy_id;
    std::vector<ActiveTrade> active_trades;
    long long last_processed_kline_open_time_ms;

    // Polymarket fields
    std::string slug;
    std::string condition_id;
    bool is_active{false};
    bool is_closed{false};
    bool is_accepting_orders{false};
    bool is_order_book_enabled{false};
    double order_price_min_tick_size{0.0};
    double order_min_size{0.0};
    long long event_start_time_ms{0};
    long long event_end_time_ms{0};
    long long accepting_orders_timestamp_ms{0};
    bool is_neg_risk{false};
    int num_outcomes{0};
    std::vector<std::string> outcome_names;
    std::vector<std::string> clob_token_ids;
};