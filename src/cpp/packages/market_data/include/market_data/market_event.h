#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "market_making/dtypes/orderbook.h"

namespace MarketData {

struct TradeEvent {
    std::string symbol;
    std::string exchange_name;
    std::string market_type;
    double price;
    double qty;
    bool is_buyer_maker;
    long long timestamp_ms;
};

// OHLCV bar event — used by the simulator's MarketDataReplayer to
// carry bar data from disk through to wire-format conversion. The
// engine itself doesn't consume MarketEvent::OHLCV directly; it
// receives the raw WS frame (e.g., a Polygon AM array) and parses it
// via `parse_polygon_ohlcv_message`. This carrier exists so the
// disk-replay pipeline has a single MarketEvent type for all
// historical playback.
struct OhlcvEvent {
    std::string symbol;
    std::string exchange_name;
    std::string market_type;
    long long kline_open_time_ms;
    long long kline_close_time_ms;
    double open_price;
    double high_price;
    double low_price;
    double close_price;
    double volume;
    double quote_asset_volume;
    long long number_of_trades;
    // Taker-buy splits — required for OFI / burst feature computation.
    // Default to 0.0 so existing code paths that don't set them continue to
    // construct cleanly; SMs that depend on these must declare data sources
    // that actually populate them.
    double taker_buy_base_volume = 0.0;
    double taker_buy_quote_volume = 0.0;
};

struct BBAEvent {
    std::string symbol;
    std::string exchange_name;
    std::string market_type;
    double bid_price;
    double bid_qty;
    double ask_price;
    double ask_qty;
    long long timestamp_ms;
};

struct L2UpdateEvent {
    enum SubType { INCREMENTAL, FULL_REFRESH, LEVELS_ONLY };
    SubType sub_type = INCREMENTAL;
    Core::OrderBookUpdate update;
    std::string exchange_name;
    std::string market_type;
    std::string symbol;
    std::optional<Core::OrderBookLevels> full_refresh_orderbook;
};

// Funding rate update for perpetual swap markets. In live mode this is
// produced by a periodic poller of the venue's funding-rate endpoint
// (e.g. binance fapi /fundingRate); in historical mode the
// historical_reader replays from raw/funding/{exchange}/{market_type}/
// parquets, with a small simulated polling-latency offset applied to
// `timestamp_ms` so SMs see funding events at the same observable
// delay as in live (preventing train-serve skew).
struct FundingEvent {
    std::string symbol;
    std::string exchange_name;
    std::string market_type;
    double funding_rate;
    long long timestamp_ms;   // observable time (apply_time + poll_latency)
};

struct PolymarketBBAEvent {
    std::string asset_id;
    double best_bid;
    double best_ask;
    long long timestamp_ms;
};

struct PolymarketTickSizeChangeEvent {
    std::string condition_id;
    double new_tick_size;
};

struct PolymarketL2SnapshotEvent {
    std::string asset_id;
    Core::OrderBookLevels orderbook;
    long long timestamp_ms;
};

struct PolymarketL2DeltaEvent {
    std::string asset_id;
    double price;
    double size;
    bool is_bid;
    double best_bid;
    double best_ask;
    long long timestamp_ms;
};

struct PolymarketLastTradeEvent {
    std::string condition_id;
    std::string asset_id;
    double price;
    double size;
    bool is_buy;
    long long timestamp_ms;
};

struct PolymarketMarketResolvedEvent {
    std::string condition_id;
    long long timestamp_ms;
};

struct MarketEvent {
    enum Type {
        TRADE,
        BBA,
        L2_UPDATE,
        OHLCV,
        FUNDING,
        POLYMARKET_BBA,
        POLYMARKET_TICK_SIZE_CHANGE,
        POLYMARKET_L2_SNAPSHOT,
        POLYMARKET_L2_DELTA,
        POLYMARKET_LAST_TRADE,
        POLYMARKET_MARKET_RESOLVED
    };
    Type type;
    std::optional<TradeEvent> trade;
    std::optional<BBAEvent> bba;
    std::optional<L2UpdateEvent> l2_update;
    std::optional<OhlcvEvent> ohlcv;
    std::optional<FundingEvent> funding;
    std::optional<PolymarketBBAEvent> polymarket_bba;
    std::optional<PolymarketTickSizeChangeEvent> polymarket_tick_size_change;
    std::optional<PolymarketL2SnapshotEvent> polymarket_l2_snapshot;
    std::optional<PolymarketL2DeltaEvent> polymarket_l2_delta;
    std::optional<PolymarketLastTradeEvent> polymarket_last_trade;
    std::optional<PolymarketMarketResolvedEvent> polymarket_market_resolved;

    std::string symbol_key() const {
        switch (type) {
            case TRADE:
                return trade->symbol + ":" + trade->exchange_name + ":" +
                       trade->market_type;
            case BBA:
                return bba->symbol + ":" + bba->exchange_name + ":" +
                       bba->market_type;
            case L2_UPDATE:
                if (!l2_update->exchange_name.empty()) {
                    return l2_update->symbol + ":" +
                           l2_update->exchange_name + ":" +
                           l2_update->market_type;
                }
                return l2_update->update.symbol_full;
            case OHLCV:
                return ohlcv->symbol + ":" + ohlcv->exchange_name + ":" +
                       ohlcv->market_type;
            case FUNDING:
                return funding->symbol + ":" + funding->exchange_name + ":" +
                       funding->market_type;
            case POLYMARKET_BBA:
                return "polymarket:" + polymarket_bba->asset_id;
            case POLYMARKET_TICK_SIZE_CHANGE:
                return "polymarket:tick_size:" +
                       polymarket_tick_size_change->condition_id;
            case POLYMARKET_L2_SNAPSHOT:
                return "polymarket:" + polymarket_l2_snapshot->asset_id;
            case POLYMARKET_L2_DELTA:
                return "polymarket:" + polymarket_l2_delta->asset_id;
            case POLYMARKET_LAST_TRADE:
                return "polymarket:trade:" +
                       polymarket_last_trade->asset_id;
            case POLYMARKET_MARKET_RESOLVED:
                return "polymarket:resolved:" +
                       polymarket_market_resolved->condition_id;
        }
        return "";
    }
};

}  // namespace MarketData
