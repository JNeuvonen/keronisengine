#pragma once

#include <nlohmann/json.hpp>

#include "common_types/trade_ticker.h"
#include "market_making/dtypes/def_only_structs.h"

struct ModelMeta {
    std::string name;
    std::string path_to_model;
};

// One strategy as the engine runs it. Live builds it from the strategy,
// trade_ticker and strategy_model rows; a backtest from its config.json. Both
// go through from_json, whose keys are the strategy table's column names.
struct StrategyConfig {
    bool is_disabled = false;
    bool is_using_all_symbols = false;
    bool is_using_percentage_max_position = false;
    bool is_using_nominal_max_position = false;
    bool is_in_reduce_only = false;
    bool uses_hedges = false;
    bool allow_co_strategy_compounding = false;
    bool needs_to_run_realtime = false;
    bool is_trading_on_prediction_market = false;
    bool is_market_making_strategy = false;
    bool is_paper_trading = false;
    bool discretionary_multi_account = false;

    std::string account_mode = "shared";

    // A fraction; config and DB column hold it in percent.
    double percentage_max_position = 0;
    double nominal_max_position = 0;
    double max_directional_exposure = 0;
    double max_sizing_per_symbol = 0;
    double virtual_balance = 0;

    int id = 0;
    int candle_interval_ms = 0;

    int num_max_orders_10_sec = 0;
    int num_max_orders_1_min = 0;
    int num_max_orders_1_hour = 0;

    std::string strategy_name;
    std::string strategy_type;

    std::string series_slug;
    // Multi-series strategies: when non-empty, events matching ANY slug in
    // this list are adopted and `series_slug` is ignored.
    std::vector<std::string> series_slugs;

    [[nodiscard]] bool accepts_series_slug(const std::string& slug) const {
        if (!series_slugs.empty()) {
            for (const auto& s : series_slugs) {
                if (s == slug) return true;
            }
            return false;
        }
        return series_slug.empty() || series_slug == slug;
    }

    std::vector<TradeTicker> trade_tickers;
    std::vector<Core::ExchangeAndMarket> exchanges_list;
    std::vector<ModelMeta> strategy_models;
    std::string main_exchange;

    // Optional path to a fold_schedule.json. When set, this strategy receives
    // MODEL_UPDATE events whose schedule_path matches.
    std::string model_schedule_path;

    // Strict: an unknown key, a wrong type or a missing id / strategy_name
    // throws. Tickers sit under "trade_tickers", models under "models".
    static StrategyConfig from_json(const nlohmann::json& config);
};
