#pragma once
#include <mutex>

#include "market_data/bar_close_policy.h"
#include "market_data/normalized_trade.h"
#include "market_data/ohlcv_bar_meta.h"
#include "nlohmann/json.hpp"
#include "ohlcv_dataset_tick.h"

namespace MarketData {

static MarketData::OhlcvDatasetTick create_new_ohlcv_tick(
    const MarketData::BarClosePolicy &bar_close_policy,
    long long kline_open_time_ms);
}  // namespace MarketData

struct BarFlushInfo {
    MarketData::OhlcvDatasetTick tick;
    std::string bar_identifier;

    [[nodiscard]] nlohmann::json to_json() const {
        nlohmann::json json;
        json["tick"] = tick.to_json();
        json["bar_identifier"] = bar_identifier;
        return json;
    }
};

struct Bar {
    Bar(const MarketData::OhlcvBarMeta &meta);

    bool is_dataset_init_first_time = false;
    long long first_live_stream_trade_timestamp_ms = 0;
    std::string first_live_stream_trade_id = "";
    long long last_trade_timestamp_ms = 0;
    std::string last_trade_id = "";

    // REST gap-fill cursor — independent from last_trade_id (which gets
    // clobbered by WS arrivals before the gap-fill walk even starts). Tracks
    // where the by-id walk in `sync_live_streamed_bars` has progressed to.
    // Initialized empty; bootstrap step seeds it from a from-timestamp fetch
    // starting at bar_meta.last_storage_tick_close_time_ms (disk-end of bars).
    std::string sync_walk_cursor_trade_id = "";

    std::mutex mutex;
    std::vector<MarketData::OhlcvDatasetTick> ticks;
    std::vector<MarketData::NormalizedParsedTrade> buffered_trades;
    bool is_synced_with_live = false;
    std::optional<std::chrono::steady_clock::time_point> last_sync_time =
        std::nullopt;
    bool buffered_trades_unsynced = true;

    MarketData::OhlcvBarMeta bar_meta;
    MarketData::BarClosePolicy bar_close_policy;

    std::vector<MarketData::NormalizedParsedTrade> unhandled_trades;
    std::vector<MarketData::OhlcvDatasetTick> unhandled_new_ticks;

    long long latency_sum{0};
    long long latency_count{0};
    long long latency_buffer_ms{300};
    bool latency_buffer_initialized{false};

    std::vector<BarFlushInfo> emit_closed_bars();
    std::vector<BarFlushInfo> emit_closed_bars_lockless();
    [[nodiscard]] nlohmann::json to_json() const;

    void create_missing_klines();
    std::optional<long long> get_seconds_since_last_sync();

    [[nodiscard]] bool get_is_synced_with_live();
    [[nodiscard]] long long get_first_live_stream_trade_timestamp_ms();
    [[nodiscard]] long long get_last_trade_timestamp_ms();
    [[nodiscard]] std::string get_last_trade_id();
    // Thread-safe access to sync_walk_cursor_trade_id. The cursor is written
    // both from the WS path (Bar::add_trade) and the REST walker
    // (BarAggregator::sync_live_streamed_bars); std::string assignment is not
    // atomic, so concurrent direct field access risks torn reads. Both
    // accessors hold Bar::mutex.
    [[nodiscard]] std::string get_sync_walk_cursor_trade_id();
    void set_sync_walk_cursor_trade_id(const std::string &id);
    void stream_disconnected_ack();
    void add_trade(const MarketData::NormalizedParsedTrade &trade,
                   bool is_live_streamed);
    void add_trade_from_live_stream_buffer(
        const MarketData::NormalizedParsedTrade &trade);
    void add_trade_to_live_stream_buffer(
        const MarketData::NormalizedParsedTrade &trade);
    void handle_trade(const MarketData::NormalizedParsedTrade &trade);
    void sync_buffered_trades();
    void _add_trade_lockless(const MarketData::NormalizedParsedTrade &trade);
    void _add_new_trade_to_bar(MarketData::OhlcvDatasetTick &tick,
                               const MarketData::NormalizedParsedTrade &trade);
    void _handle_time_based_bar_trade(
        MarketData::OhlcvDatasetTick &tick,
        const MarketData::NormalizedParsedTrade &trade);
    void _handle_dollar_vol_based_bar_trade(
        MarketData::OhlcvDatasetTick &tick,
        const MarketData::NormalizedParsedTrade &trade);
    void _handle_tick_count_based_bar_trade(
        MarketData::OhlcvDatasetTick &tick,
        const MarketData::NormalizedParsedTrade &trade);
    void _handle_trade_for_ticks(
        std::vector<MarketData::OhlcvDatasetTick> &ticks,
        const MarketData::NormalizedParsedTrade &trade);
};