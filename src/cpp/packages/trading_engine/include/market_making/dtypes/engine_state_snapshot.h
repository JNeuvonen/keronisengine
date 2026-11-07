#pragma once

#include "market_making/dtypes/inventory.h"
#include "market_making/dtypes/order.h"
#include "market_making/dtypes/orderbook.h"
#include "market_making/dtypes/prediction.h"
#include "market_making/dtypes/quote_asset.h"
#include "market_making/dtypes/trade.h"
#include "mpreal.h"
#include "nlohmann/json.hpp"

namespace Core {

struct TopOfBook {
    double best_bid = 0;
    double best_ask = 0;
    double best_bid_qty = 0;
    double best_ask_qty = 0;

    nlohmann::json to_json() const {
        return {{"best_bid", best_bid},
                {"best_ask", best_ask},
                {"best_bid_qty", best_bid_qty},
                {"best_ask_qty", best_ask_qty}};
    }

    static TopOfBook from_json(const nlohmann::json &json) {
        TopOfBook tob;
        tob.best_bid = json.value("best_bid", 0.0);
        tob.best_ask = json.value("best_ask", 0.0);
        tob.best_bid_qty = json.value("best_bid_qty", 0.0);
        tob.best_ask_qty = json.value("best_ask_qty", 0.0);
        return tob;
    }
};

struct EngineStateSnapshot {
    long long timestamp_ms;
    long long program_start_time_ms = 0;
    mpfr::mpreal nav;
    mpfr::mpreal margin_notional;
    mpfr::mpreal long_exposure;
    mpfr::mpreal short_exposure;
    mpfr::mpreal funding_collected;
    std::unordered_map<std::string, QuoteAsset> quote_assets;
    std::unordered_map<std::string, Inventory> inventories;
    std::unordered_map<std::string, OrderBook> order_books;
    std::unordered_map<std::string, double> prices;
    std::unordered_map<std::string, TopOfBook> top_of_book;
    std::vector<Trade> active_trades;
    std::vector<Order> active_orders;
    long long num_active_trades;
    long long num_active_orders;
    std::unordered_map<std::string, std::vector<Trade>> new_trades;
    std::unordered_map<std::string, std::vector<Order>> new_orders;
    std::unordered_map<std::string, std::vector<Order>> new_orders_cancels;
    std::unordered_map<std::string, std::vector<OrderFillInfo>> new_order_fills;
    std::vector<Signal> actionable_signals;
    std::vector<Signal> all_signals;
    std::vector<CompletedMarkout> completed_markouts;

    // Engine instance identity + event_link_id watermark range captured
    // since the previous snapshot. Populated by emit_engine_state_snapshot
    // from the Engine's observability watermarks. Used by the snapshot
    // archiver to index the gzip'd file in engine_snapshot_index.
    std::string engine_instance;
    long long event_link_id_lo = -1;  // -1 = unset (empty interval)
    long long event_link_id_hi = -1;

    nlohmann::json to_json() const;
    static EngineStateSnapshot from_json(const nlohmann::json &json);
};

struct EngineStateSnapshotLight {
    long long timestamp_ms;
    std::vector<Trade> trades;
    std::vector<Order> orders;

    nlohmann::json to_json() const;
};

}  // namespace Core