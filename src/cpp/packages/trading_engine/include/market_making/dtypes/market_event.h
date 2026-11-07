#pragma once

#include <optional>

#include "aggr_trade_dataset_tick.h"
#include "market_data/bar.h"
#include "market_making/dtypes/def_only_structs.h"
#include "market_making/dtypes/enums.h"
#include "market_making/dtypes/orderbook.h"
#include "mpreal.h"
#include "nlohmann/json.hpp"
#include "ohlcv_dataset_tick.h"
#include "polymarket_trade_tick.h"
#include "sqlite_datasets.h"
#include "trade_tick.h"

namespace Core {

struct BacktestPredictionEvent {
    std::string model_name;
    float prediction;
    long long timestamp_ms;
};

// Carried by MarketEvent(MODEL_UPDATE) — schedules a model bundle to become
// active at `event_time`. The event is keyed by `schedule_path` (the bundle's
// identity); engine dispatch atomic-stores a ModelBundle into `active_bundle`
// of EVERY strategy whose `strategy_config.model_schedule_path` matches —
// N-to-1 strategy:bundle is supported. metadata is opaque to the engine;
// strategy reads its own keys (e.g. bear/bull thresholds).
struct ModelUpdateEvent {
    std::string schedule_path;
    std::vector<ModelMemberSpec> members;
    nlohmann::json metadata;
};

struct MarketEvent {
    MarketEventType type;
    std::optional<mpfr::mpreal> sizing_coeff;
    std::optional<std::string> symbol;
    std::optional<std::string> exchange_name;
    std::optional<std::string> market_type;
    std::optional<double> price;
    std::optional<long long> strategy_id;
    std::optional<OrderBook> order_book;
    std::optional<double> prediction;
    std::optional<AggrTradeDatasetTick> aggr_trade;
    std::optional<BarFlushInfo> bar_flush_info;
    std::optional<TradeTick> trade;
    std::optional<double> funding_rate_paid;
    std::optional<mpfr::mpreal> notional_funding_payment;
    std::optional<PolymarketTradeTick> polymarket_trade;
    std::optional<PolymarketEventMeta> polymarket_event_meta;
    std::optional<BacktestPredictionEvent> backtest_prediction;
    std::optional<ModelUpdateEvent> model_update;
    long long event_time;
    nlohmann::json to_json() const;
};

}  // namespace Core