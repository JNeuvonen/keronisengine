#pragma once

#include <span>

#include "common_types/backtest_processor_types.h"
#include "common_types/constants.h"
#include "ohlcv_dataset_tick.h"

struct AggrTradeDatasetTick;

enum class PolymarketProductType {
    UNKNOWN = 0,
    DIGITAL_CLOSE_ABOVE = 1,
    DIGITAL_CLOSE_BELOW = 2,
    ONE_TOUCH_UP = 3,
    ONE_TOUCH_DOWN = 4,
};

struct PolymarketEventMeta {
    std::string series_slug;
    std::string slug;
    std::string condition_id;
    long long event_start_time_ms;
    long long event_end_time_ms;
    long long accepting_orders_timestamp_ms;
    double order_price_min_tick_size;
    double order_min_size;
    bool is_neg_risk;
    bool active = true;
    bool closed = false;
    bool accepting_orders = true;
    int num_outcomes;
    std::vector<std::string> outcome_names;
    std::vector<std::string> clob_token_ids;
    std::vector<double> outcome_prices;
    std::string question;
    std::string group_item_title;
    std::string underlying_symbol;
    double strike_price = 0.0;
    PolymarketProductType product_type = PolymarketProductType::UNKNOWN;
};

namespace SqliteDatasets {

struct OhlcvDatasetsResult {
    std::vector<long long> kline_open_times;
    std::vector<long long> kline_close_times;
    std::vector<double> close_prices;
    std::vector<double> open_prices;
    std::vector<double> high_prices;
    std::vector<double> low_prices;
    std::vector<double> volumes;
    std::vector<double> quote_asset_volumes;
    std::vector<long> number_of_trades;
    std::vector<double> taker_buy_base_volumes;
    std::vector<double> taker_buy_quote_volumes;
};

const DbColumnsVector read_table_into_vector(
    const std::string &db_path, const std::string &table_name,
    const std::string &key_column, std::optional<long long> start_ts_ms,
    std::optional<long long> end_ts_ms,
    std::optional<std::vector<std::string>> used_columns);

std::array<StrategyDecisionTick, BACKTEST_CHUNK_SIZE> load_dataset_ticks(
    const std::string &path, long long start_date_ms, long long timestep);

std::array<MarketData::OhlcvDatasetTick, BACKTEST_CHUNK_SIZE>
load_price_dataset_ticks(const std::string &path, long long start_date_ms,
                         long long timestep);

OhlcvDatasetsResult load_ohlcv_dataset(const std::string &path,
                                       const std::string &table_name,
                                       std::optional<long long> start_ts_ms,
                                       std::optional<long long> end_ts_ms);

void write_sizing_coefficients(const std::string &db_path,
                               const std::string &table_name,
                               std::span<const long long> timestamps,
                               std::span<const char *const> coefficients);
void write_predictions(const std::string &db_path,
                       const std::string &table_name,
                       std::span<const long long> timestamps,
                       std::span<const double> predictions,
                       std::span<const double> close_prices);
std::vector<MarketData::OhlcvDatasetTick> load_ohlcv_dataset_ticks(
    const std::string &path, const std::string &table_name,
    std::optional<long long> start_ts_ms, std::optional<long long> end_ts_ms);

void write_ohlcv_dataset(
    const std::string &db_path, const std::string &table_name,
    const std::vector<MarketData::OhlcvDatasetTick> &ticks);
std::vector<StrategyDecisionTick> load_dataset_ticks_vec(
    const std::string &path, long long start_date_ms);
std::vector<PredictionTick> load_predictions_ticks_vec(const std::string &path,
                                                       long long start_date_ms);

std::vector<AggrTradeDatasetTick> load_trades_dataset_dir(
    const std::string &path, std::optional<long long> start_ts_ms,
    std::optional<long long> end_ts_ms);
void write_ohlcv_dataset_unique(
    const std::string &db_path, const std::string &table_name,
    std::vector<MarketData::OhlcvDatasetTick> &ticks);

void write_ohlcv_dataset_unique_minimal(
    const std::string &db_path, const std::string &table_name,
    std::vector<MarketData::OhlcvDatasetTick> &ticks);

// Loads markets for one series from its per-series store DB
// (<series_base_dir>/<series_slug>/store.db — tables events, markets,
// resolved_markets). series_base_dir is the shared
// ".../prediction_markets/polymarket/series/" directory; the slug subdir is
// appended internally so every reader (live, recorder, backtest) resolves the
// same path. The writer is the Python polymarket sync.
std::vector<PolymarketEventMeta> load_polymarket_markets(
    const std::string &series_base_dir, const std::string &series_slug,
    long long start_ts_ms, long long end_ts_ms);

}  // namespace SqliteDatasets
