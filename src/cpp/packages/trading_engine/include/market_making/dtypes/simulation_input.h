#pragma once

#include <string>
#include <vector>

#include "common_types/strategy.h"
#include "market_making/dtypes/exchange_config.h"
#include "market_making/dtypes/market_data.h"
#include "nlohmann/json.hpp"

namespace Core {

struct MarketDataReplayConfig {
    std::string load_mode;
    int reader_threads = 0;
    long long chunk_duration_ms = 300000;
    long long cache_window_ms = 0;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        MarketDataReplayConfig, load_mode, reader_threads, chunk_duration_ms,
        cache_window_ms);
};

// A discretionary_account row. Exchange, market and capital come from the
// sim exchange account with the same id.
struct DiscretionaryAccountConfig {
    long long account_id = 0;
    bool paper = true;
    bool enabled = false;
    double max_directional_exposure = 0;
    double max_order_notional = 0;
    int num_max_orders_10_sec = 0;
    int num_max_orders_1_min = 0;
    int num_max_orders_1_hour = 0;
};

struct SimulationInput {
    MarketData market_data;
    // Parsed by the runner's config reader, not by the macro below.
    std::vector<StrategyConfig> strategies;
    std::vector<DiscretionaryAccountConfig> discretionary_accounts;
    long long simulation_start_date_ms;
    long long simulation_end_date_ms;
    long long market_data_reading_offset_ms;
    long long num_required_klines_on_datasets = 6000;
    double fee_rate_coeff;
    std::vector<ExchangeConfig> exchanges;
    bool is_strategy_development;
    bool emit_trading_logs;
    long long ohlcv_delay_ms;
    double swap_leverage;
    bool aggregate_klines_from_trades;
    bool generate_predictions_from_klines;
    bool show_realtime_dashboard;
    bool skip_reading_data_with_offset_on_trades;
    bool autofill_orders;
    std::string market_data_library_root_path;
    bool read_all_market_data_at_once = false;
    bool skip_historical_order_accumulation_on_backtest = false;
    bool run_strategy_on_bar_arrival = false;
    long long latency_to_exchange = 0;
    // Ack a cancel when the simulated venue removes the order (cancel time +
    // latency) instead of on send, so a fill inside that window reaches its
    // trade as it would live. Off by default: existing runs are unchanged.
    bool ack_cancels_at_exchange_removal = false;
    bool use_trade_price_for_fills = false;
    double polymarket_queue_notional = 0.0;
    int ohlcv_sampling_rate = 1;
    int fill_bars_aggregation_ticks = 1;
    // When set, the runner writes trades.json + summary.json + equity_curve.json
    // under this directory. Empty (default) suppresses all three. Field is
    // optional in the JSON config so existing configs without it still parse.
    std::string output_dir;
    MarketDataReplayConfig market_data_replay;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        SimulationInput, market_data, simulation_start_date_ms, simulation_end_date_ms,
        market_data_reading_offset_ms, num_required_klines_on_datasets,
        fee_rate_coeff, exchanges, is_strategy_development, emit_trading_logs,
        ohlcv_delay_ms, swap_leverage, aggregate_klines_from_trades,
        generate_predictions_from_klines, show_realtime_dashboard,
        skip_reading_data_with_offset_on_trades, autofill_orders,
        market_data_library_root_path,
        read_all_market_data_at_once,
        skip_historical_order_accumulation_on_backtest,
        run_strategy_on_bar_arrival, latency_to_exchange,
        ack_cancels_at_exchange_removal,
        use_trade_price_for_fills,
        polymarket_queue_notional, ohlcv_sampling_rate,
        fill_bars_aggregation_ticks, output_dir, market_data_replay);

    nlohmann::json to_json() const;
};

}  // namespace Core
