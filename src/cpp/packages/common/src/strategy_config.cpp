#include "common_types/strategy.h"

#include "strict_json.h"

namespace {

Core::ExchangeAndMarket exchange_from_json(const nlohmann::json& config,
                                           const std::string& context) {
    StrictJson::check_keys(config, {"exchange_name", "market_type", "account_id"},
                           context);
    Core::ExchangeAndMarket exchange;
    exchange.exchange_name =
        StrictJson::require<std::string>(config, "exchange_name", context);
    exchange.market_type =
        StrictJson::require<std::string>(config, "market_type", context);
    if (config.contains("account_id")) {
        exchange.account_id =
            StrictJson::require<long long>(config, "account_id", context);
    }
    return exchange;
}

TradeTicker ticker_from_json(const nlohmann::json& config, int strategy_id,
                             const std::string& context) {
    StrictJson::check_keys(
        config,
        {"id", "symbol", "base_asset", "quote_asset", "slug", "condition_id",
         "is_active", "is_closed", "is_accepting_orders",
         "is_order_book_enabled", "order_price_min_tick_size", "order_min_size",
         "event_start_time_ms", "event_end_time_ms",
         "accepting_orders_timestamp_ms", "is_neg_risk", "num_outcomes",
         "outcome_names", "clob_token_ids"},
        context);
    TradeTicker ticker;
    ticker.id = 0;
    ticker.strategy_id = strategy_id;
    StrictJson::read(config, "id", ticker.id, context);
    ticker.symbol = StrictJson::require<std::string>(config, "symbol", context);
    ticker.base_asset =
        StrictJson::require<std::string>(config, "base_asset", context);
    ticker.quote_asset =
        StrictJson::require<std::string>(config, "quote_asset", context);
    StrictJson::read(config, "slug", ticker.slug, context);
    StrictJson::read(config, "condition_id", ticker.condition_id, context);
    StrictJson::read(config, "is_active", ticker.is_active, context);
    StrictJson::read(config, "is_closed", ticker.is_closed, context);
    StrictJson::read(config, "is_accepting_orders", ticker.is_accepting_orders,
                     context);
    StrictJson::read(config, "is_order_book_enabled",
                     ticker.is_order_book_enabled, context);
    StrictJson::read(config, "order_price_min_tick_size",
                     ticker.order_price_min_tick_size, context);
    StrictJson::read(config, "order_min_size", ticker.order_min_size, context);
    StrictJson::read(config, "event_start_time_ms", ticker.event_start_time_ms,
                     context);
    StrictJson::read(config, "event_end_time_ms", ticker.event_end_time_ms,
                     context);
    StrictJson::read(config, "accepting_orders_timestamp_ms",
                     ticker.accepting_orders_timestamp_ms, context);
    StrictJson::read(config, "is_neg_risk", ticker.is_neg_risk, context);
    StrictJson::read(config, "num_outcomes", ticker.num_outcomes, context);
    StrictJson::read(config, "outcome_names", ticker.outcome_names, context);
    StrictJson::read(config, "clob_token_ids", ticker.clob_token_ids, context);
    return ticker;
}

ModelMeta model_from_json(const nlohmann::json& config,
                          const std::string& context) {
    StrictJson::check_keys(config, {"name", "path_to_model"}, context);
    return ModelMeta{
        .name = StrictJson::require<std::string>(config, "name", context),
        .path_to_model =
            StrictJson::require<std::string>(config, "path_to_model", context),
    };
}

}  // namespace

StrategyConfig StrategyConfig::from_json(const nlohmann::json& config) {
    StrictJson::check_keys(
        config,
        {"id",
         "strategy_name",
         "strategy_type",
         "is_disabled",
         "is_using_all_available_symbols",
         "is_using_percentage_max_position",
         "is_using_nominal_max_position",
         "is_in_reduce_only",
         "uses_hedges",
         "allow_co_strategy_compounding",
         "needs_to_run_realtime",
         "is_trading_on_prediction_market",
         "is_market_making_strategy",
         "is_paper_trading",
         "discretionary_multi_account",
         "account_mode",
         "percentage_max_position",
         "nominal_max_position",
         "max_directional_exposure",
         "max_sizing_per_symbol",
         "virtual_balance",
         "candle_interval_ms",
         "num_max_orders_10_sec",
         "num_max_orders_1_min",
         "num_max_orders_1_hour",
         "series_slug",
         "series_slugs",
         "main_exchange",
         "model_schedule_path",
         "exchanges_list",
         "trade_tickers",
         "models"},
        "strategy");

    StrategyConfig strategy;
    strategy.id = StrictJson::require<int>(config, "id", "strategy");
    const std::string context = "strategy " + std::to_string(strategy.id);
    strategy.strategy_name =
        StrictJson::require<std::string>(config, "strategy_name", context);
    strategy.is_using_all_symbols = true;
    strategy.main_exchange = "binance";

    StrictJson::read(config, "strategy_type", strategy.strategy_type, context);
    StrictJson::read(config, "is_disabled", strategy.is_disabled, context);
    StrictJson::read(config, "is_using_all_available_symbols",
                     strategy.is_using_all_symbols, context);
    StrictJson::read(config, "is_using_percentage_max_position",
                     strategy.is_using_percentage_max_position, context);
    StrictJson::read(config, "is_using_nominal_max_position",
                     strategy.is_using_nominal_max_position, context);
    StrictJson::read(config, "is_in_reduce_only", strategy.is_in_reduce_only,
                     context);
    StrictJson::read(config, "uses_hedges", strategy.uses_hedges, context);
    StrictJson::read(config, "allow_co_strategy_compounding",
                     strategy.allow_co_strategy_compounding, context);
    StrictJson::read(config, "needs_to_run_realtime",
                     strategy.needs_to_run_realtime, context);
    StrictJson::read(config, "is_trading_on_prediction_market",
                     strategy.is_trading_on_prediction_market, context);
    StrictJson::read(config, "is_market_making_strategy",
                     strategy.is_market_making_strategy, context);
    StrictJson::read(config, "is_paper_trading", strategy.is_paper_trading,
                     context);
    StrictJson::read(config, "discretionary_multi_account",
                     strategy.discretionary_multi_account, context);
    StrictJson::read(config, "account_mode", strategy.account_mode, context);

    double percentage_max_position = 0;
    StrictJson::read(config, "percentage_max_position", percentage_max_position,
                     context);
    strategy.percentage_max_position = percentage_max_position / 100;
    StrictJson::read(config, "nominal_max_position",
                     strategy.nominal_max_position, context);
    StrictJson::read(config, "max_directional_exposure",
                     strategy.max_directional_exposure, context);
    StrictJson::read(config, "max_sizing_per_symbol",
                     strategy.max_sizing_per_symbol, context);
    StrictJson::read(config, "virtual_balance", strategy.virtual_balance,
                     context);

    StrictJson::read(config, "candle_interval_ms", strategy.candle_interval_ms,
                     context);
    StrictJson::read(config, "num_max_orders_10_sec",
                     strategy.num_max_orders_10_sec, context);
    StrictJson::read(config, "num_max_orders_1_min",
                     strategy.num_max_orders_1_min, context);
    StrictJson::read(config, "num_max_orders_1_hour",
                     strategy.num_max_orders_1_hour, context);

    StrictJson::read(config, "series_slug", strategy.series_slug, context);
    StrictJson::read(config, "series_slugs", strategy.series_slugs, context);
    StrictJson::read(config, "main_exchange", strategy.main_exchange, context);
    StrictJson::read(config, "model_schedule_path",
                     strategy.model_schedule_path, context);

    for (const auto& exchange :
         StrictJson::array_at(config, "exchanges_list", context)) {
        strategy.exchanges_list.push_back(
            exchange_from_json(exchange, context + ".exchanges_list"));
    }
    for (const auto& ticker :
         StrictJson::array_at(config, "trade_tickers", context)) {
        strategy.trade_tickers.push_back(
            ticker_from_json(ticker, strategy.id, context + ".trade_tickers"));
    }
    for (const auto& model : StrictJson::array_at(config, "models", context)) {
        strategy.strategy_models.push_back(
            model_from_json(model, context + ".models"));
    }
    return strategy;
}
