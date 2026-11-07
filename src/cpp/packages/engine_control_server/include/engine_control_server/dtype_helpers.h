#pragma once

#include <nlohmann/json.hpp>

#include "market_data/market_event.h"
#include "market_making/dtypes/enums.h"
#include "market_making/dtypes/market_event.h"
#include "mpreal.h"

namespace EngineControl::DtypeHelpers {

// `Core::MarketEvent` has no `from_json` factory in the engine headers
// (market_event.h:25). MarketEvent's optional sub-structs (order_book,
// aggr_trade, trade, fill_bar, polymarket_trade, polymarket_event_meta,
// backtest_prediction, bar_flush_info) need their own deserializers and
// aren't all needed yet — defer those until a scenario demands them.
//
// This subset covers the variants we have in flight: PRICE_UPDATE,
// FUNDING_PAYMENT, SIZING_COEFF_UPDATE, PREDICTION_UPDATE.
//
// Inline so multiple TUs (write_handlers.cpp for /control/engine/market-event,
// strategy_handlers.cpp for /control/strategy/sm/react) can share without an
// extra .cpp. When sub-struct optionals get added, this is the single place to
// extend.
inline Core::MarketEvent parse_market_event(const nlohmann::json& j) {
    Core::MarketEvent ev;
    ev.type = static_cast<Core::MarketEventType>(j.at("type").get<int>());
    ev.event_time = j.value("event_time", 0LL);
    if (j.contains("symbol")) ev.symbol = j["symbol"].get<std::string>();
    if (j.contains("exchange_name")) {
        ev.exchange_name = j["exchange_name"].get<std::string>();
    }
    if (j.contains("market_type")) {
        ev.market_type = j["market_type"].get<std::string>();
    }
    if (j.contains("price")) ev.price = j["price"].get<double>();
    if (j.contains("strategy_id")) {
        ev.strategy_id = j["strategy_id"].get<long long>();
    }
    if (j.contains("prediction")) {
        ev.prediction = j["prediction"].get<double>();
    }
    if (j.contains("funding_rate_paid")) {
        ev.funding_rate_paid = j["funding_rate_paid"].get<double>();
    }
    if (j.contains("notional_funding_payment")) {
        ev.notional_funding_payment =
            mpfr::mpreal(j["notional_funding_payment"].get<double>());
    }
    if (j.contains("sizing_coeff")) {
        ev.sizing_coeff = mpfr::mpreal(j["sizing_coeff"].get<double>());
    }
    return ev;
}

// `MarketData::MarketEvent` is a SEPARATE type from `Core::MarketEvent` (yes,
// same name, different namespaces). Engine endpoints use Core::MarketEvent;
// `StrategyStateMachine::react` uses MarketData::MarketEvent — different
// discriminated union, different sub-structs (TradeEvent, BBAEvent,
// L2UpdateEvent, multiple polymarket sub-events).
//
// Phase C MVP supports TRADE and BBA only — the variants strategies react
// to most often. L2_UPDATE + polymarket sub-events deferred until a
// scenario demands them.
//
// Body shape:
//   TRADE: {"type": 0, "trade": {symbol, exchange_name, market_type,
//                                price, qty, is_buyer_maker, timestamp_ms}}
//   BBA:   {"type": 1, "bba":   {symbol, exchange_name, market_type,
//                                bid_price, bid_qty, ask_price, ask_qty,
//                                timestamp_ms}}
inline MarketData::MarketEvent parse_sm_market_event(const nlohmann::json& j) {
    MarketData::MarketEvent ev;
    ev.type = static_cast<MarketData::MarketEvent::Type>(
        j.at("type").get<int>());
    if (ev.type == MarketData::MarketEvent::TRADE) {
        const auto& t = j.at("trade");
        MarketData::TradeEvent te;
        te.symbol = t.at("symbol").get<std::string>();
        te.exchange_name = t.at("exchange_name").get<std::string>();
        te.market_type = t.at("market_type").get<std::string>();
        te.price = t.at("price").get<double>();
        te.qty = t.at("qty").get<double>();
        te.is_buyer_maker = t.at("is_buyer_maker").get<bool>();
        te.timestamp_ms = t.at("timestamp_ms").get<long long>();
        ev.trade = te;
    } else if (ev.type == MarketData::MarketEvent::BBA) {
        const auto& b = j.at("bba");
        MarketData::BBAEvent be;
        be.symbol = b.at("symbol").get<std::string>();
        be.exchange_name = b.at("exchange_name").get<std::string>();
        be.market_type = b.at("market_type").get<std::string>();
        be.bid_price = b.at("bid_price").get<double>();
        be.bid_qty = b.at("bid_qty").get<double>();
        be.ask_price = b.at("ask_price").get<double>();
        be.ask_qty = b.at("ask_qty").get<double>();
        be.timestamp_ms = b.at("timestamp_ms").get<long long>();
        ev.bba = be;
    }
    // Other types: ev.type set, sub-fields left empty. SM react may
    // assert / segfault on unsupported types — caller should validate.
    return ev;
}

}  // namespace EngineControl::DtypeHelpers
