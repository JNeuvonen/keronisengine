#include "market_data/bar.h"

#include <algorithm>
#include <chrono>
#include <mutex>

#include "datetime_utils.h"
#include "disk_logging/log.h"
#include "market_data/bar_close_policy.h"
#include "market_data/normalized_trade.h"
#include "market_data/ohlcv_bar_meta.h"
#include "market_data/utils.h"
#include "ohlcv_dataset_tick.h"

/**
 * Bar class for aggregating market trades into OHLCV bars.
 * Supports multiple bar types:
 * - Time-based bars (e.g., 1-minute, 5-minute)
 * - Dollar volume-based bars
 * - Tick count-based bars
 *
 * Handles both live streamed and historical trade data with
 * synchronization mechanisms to ensure data consistency.
 */

void Bar::add_trade(const MarketData::NormalizedParsedTrade &trade,
                    bool is_live_streamed) {
    // Exchange bars come from klines and the live sync walk skips them, so
    // they would buffer every streamed trade without ever syncing.
    if (is_live_streamed && bar_close_policy.is_exchange_bar) return;
    std::lock_guard<std::mutex> lock(this->mutex);

    // Reject non-live trades if already synced with live
    if (is_synced_with_live && !is_live_streamed) {
        return;
    }

    if (is_live_streamed) {
        if (this->first_live_stream_trade_timestamp_ms == 0) {
            this->first_live_stream_trade_timestamp_ms = trade.timestamp;
            this->first_live_stream_trade_id = trade.trade_id;
        }
        if (!is_synced_with_live) {
            this->add_trade_to_live_stream_buffer(trade);
            return;
        }
        this->latency_sum += trade.latency_ms;
        this->latency_count++;
        if (!this->latency_buffer_initialized && this->latency_count == 50) {
            this->latency_buffer_ms = std::max(
                (this->latency_sum / this->latency_count) * 3 / 2, 50LL);
            this->latency_buffer_initialized = true;
        }
        if (this->latency_count >= 1000) {
            this->latency_buffer_ms = std::max(
                (this->latency_sum / this->latency_count) * 3 / 2, 50LL);
            this->latency_sum = 0;
            this->latency_count = 0;
        }
    }

    this->last_trade_timestamp_ms = trade.timestamp;
    this->last_trade_id = trade.trade_id;

    if (!is_synced_with_live && !is_live_streamed &&
        first_live_stream_trade_timestamp_ms > 0) {
        if (trade.timestamp >= first_live_stream_trade_timestamp_ms + 50 ||
            (!trade.trade_id.empty() && !first_live_stream_trade_id.empty() &&
             trade.trade_id == first_live_stream_trade_id)) {
            this->is_synced_with_live = true;
            this->last_sync_time = std::chrono::steady_clock::now();
        }
    }

    // Track the latest WS-applied trade_id so the REST gap-fill walker resumes
    // from the live frontier (not the position where the initial bootstrap
    // walk ended) after a WS disconnect. Without this, sync_live_streamed_bars
    // re-fetches every trade since startup on every reconnect.
    if (is_live_streamed && this->is_synced_with_live &&
        !trade.trade_id.empty()) {
        this->sync_walk_cursor_trade_id = trade.trade_id;
    }

    this->handle_trade(trade);
    if (buffered_trades_unsynced && this->is_synced_with_live) {
        this->sync_buffered_trades();
    }
}

void Bar::_add_trade_lockless(const MarketData::NormalizedParsedTrade &trade) {
    this->last_trade_timestamp_ms = trade.timestamp;
    this->handle_trade(trade);
}

long long Bar::get_last_trade_timestamp_ms() {
    std::lock_guard<std::mutex> lock(this->mutex);
    return this->last_trade_timestamp_ms;
}

std::string Bar::get_last_trade_id() {
    std::lock_guard<std::mutex> lock(this->mutex);
    return this->last_trade_id;
}

long long Bar::get_first_live_stream_trade_timestamp_ms() {
    std::lock_guard<std::mutex> lock(this->mutex);
    return this->first_live_stream_trade_timestamp_ms;
}

bool Bar::get_is_synced_with_live() {
    std::lock_guard<std::mutex> lock(this->mutex);
    return this->is_synced_with_live;
}

std::string Bar::get_sync_walk_cursor_trade_id() {
    std::lock_guard<std::mutex> lock(this->mutex);
    return this->sync_walk_cursor_trade_id;
}

void Bar::set_sync_walk_cursor_trade_id(const std::string &id) {
    std::lock_guard<std::mutex> lock(this->mutex);
    this->sync_walk_cursor_trade_id = id;
}

void Bar::add_trade_from_live_stream_buffer(
    const MarketData::NormalizedParsedTrade &trade) {
    this->handle_trade(trade);
}

void Bar::add_trade_to_live_stream_buffer(
    const MarketData::NormalizedParsedTrade &trade) {
    this->buffered_trades.push_back(trade);
    if (buffered_trades.size() > 5000000) {
        this->is_synced_with_live = false;
        this->buffered_trades_unsynced = true;
        this->first_live_stream_trade_timestamp_ms = 0;
        this->first_live_stream_trade_id = "";
        this->buffered_trades.clear();
    }
}

void Bar::handle_trade(const MarketData::NormalizedParsedTrade &trade) {
    _handle_trade_for_ticks(ticks, trade);
    while (!unhandled_trades.empty()) {
        if (!unhandled_new_ticks.empty()) {
            std::vector<MarketData::OhlcvDatasetTick> new_ticks =
                std::move(unhandled_new_ticks);
            unhandled_new_ticks.clear();
            ticks.insert(ticks.end(), new_ticks.begin(), new_ticks.end());
        }

        std::vector<MarketData::NormalizedParsedTrade> trades_copy =
            std::move(unhandled_trades);
        unhandled_trades.clear();

        for (const auto &deferred_trade : trades_copy) {
            _handle_trade_for_ticks(ticks, deferred_trade);
        }
    }
}

void Bar::_handle_time_based_bar_trade(
    MarketData::OhlcvDatasetTick &tick,
    const MarketData::NormalizedParsedTrade &trade) {
    if (tick.is_closed) {
        return;
    }

    if (trade.timestamp < tick.kline_open_time) {
        return;
    }

    if (trade.timestamp > tick.kline_close_time) {
        tick.is_closed = true;
        tick.added_trade_ids.clear();

        MarketData::OhlcvDatasetTick new_tick;

        if (trade.timestamp >
            tick.kline_close_time + bar_close_policy.time_ms + 50) {
            long long rounded_timestamp =
                DatetimeUtils::get_fully_completed_bar_close_time_from_input(
                    trade.timestamp, bar_close_policy.time_ms) -
                bar_close_policy.time_ms;
            new_tick = MarketData::create_new_ohlcv_tick(bar_close_policy,
                                                         rounded_timestamp);
        } else {
            new_tick = MarketData::create_new_ohlcv_tick(bar_close_policy,
                                                         tick.kline_close_time);
        }

        this->unhandled_new_ticks.push_back(new_tick);
        this->unhandled_trades.push_back(trade);
        return;
    }
    this->_add_new_trade_to_bar(tick, trade);
}

void Bar::_handle_dollar_vol_based_bar_trade(
    MarketData::OhlcvDatasetTick &tick,
    const MarketData::NormalizedParsedTrade &trade) {
    if (tick.is_closed) {
        return;
    }

    if (trade.timestamp < tick.kline_open_time) {
        return;
    }

    const double remaining_left =
        bar_close_policy.dollar_amount - tick.quote_asset_volume;
    if (remaining_left < trade.quantity * trade.price) {
        tick.is_closed = true;
        tick.kline_close_time = trade.timestamp;
        tick.added_trade_ids.clear();
        auto last_trade = trade;
        auto leftover_trade = trade;

        const double last_trade_qty_share =
            remaining_left / (trade.quantity * trade.price);
        const double leftover_trade_qty_share = 1 - last_trade_qty_share;

        last_trade.quantity = trade.quantity * last_trade_qty_share;
        leftover_trade.quantity = trade.quantity * leftover_trade_qty_share;

        MarketData::OhlcvDatasetTick last_trade_tick =
            MarketData::create_new_ohlcv_tick(bar_close_policy,
                                              trade.timestamp);
        this->ticks.push_back(last_trade_tick);

        this->_add_new_trade_to_bar(tick, last_trade);
        this->_add_new_trade_to_bar(ticks.back(), leftover_trade);
        return;
    }
    this->_add_new_trade_to_bar(tick, trade);
}

void Bar::_handle_tick_count_based_bar_trade(
    MarketData::OhlcvDatasetTick &tick,
    const MarketData::NormalizedParsedTrade &trade) {
    if (tick.is_closed) {
        return;
    }

    if (trade.timestamp < tick.kline_open_time) {
        return;
    }

    if (tick.number_of_trades >= bar_close_policy.tick_count - 1) {
        tick.is_closed = true;
        tick.added_trade_ids.clear();
        MarketData::OhlcvDatasetTick new_tick =
            MarketData::create_new_ohlcv_tick(bar_close_policy,
                                              trade.timestamp);
        this->ticks.push_back(new_tick);
    }
    this->_add_new_trade_to_bar(tick, trade);
}

nlohmann::json Bar::to_json() const { return {}; }

void Bar::_add_new_trade_to_bar(
    MarketData::OhlcvDatasetTick &tick,
    const MarketData::NormalizedParsedTrade &trade) {
    // Belt-and-suspenders against any sentinel/zero trade slipping past
    // the WS-boundary filter — without this, a price=0 trade pulls `low`
    // and `close` to zero on an otherwise-healthy second bar.
    if (trade.price <= 0.0 || trade.quantity <= 0.0) {
        return;
    }
    if (tick.added_trade_ids.find(trade.trade_id) !=
        tick.added_trade_ids.end()) {
        return;
    }
    tick.added_trade_ids.insert(trade.trade_id);
    if (tick.number_of_trades == 0) {
        tick.high_price = trade.price;
        tick.low_price = trade.price;
        tick.open_price = trade.price;
    }

    tick.close_price = trade.price;

    if (trade.price > tick.high_price) {
        tick.high_price = trade.price;
    }

    if (trade.price < tick.low_price) {
        tick.low_price = trade.price;
    }

    tick.volume += trade.quantity;
    tick.quote_asset_volume += trade.price * trade.quantity;
    ++tick.number_of_trades;

    // is_buyer_maker == true  → buyer rests on the book, taker is the SELLER
    // is_buyer_maker == false → seller rests on the book, taker is the BUYER
    // Only taker-buys contribute to taker_buy_* aggregates, and a single
    // taker-buy trade updates BOTH the base and quote columns.
    if (!trade.is_buyer_maker) {
        tick.taker_buy_base_volume += trade.quantity;
        tick.taker_buy_quote_volume += trade.quantity * trade.price;
    }

    if (bar_close_policy.is_dollar_bar || bar_close_policy.is_tick_count_bar) {
        tick.kline_close_time = trade.timestamp;
    }
    tick.last_trade_timestamp_ms = trade.timestamp;
}

std::vector<BarFlushInfo> Bar::emit_closed_bars() {
    std::lock_guard<std::mutex> lock(mutex);
    return emit_closed_bars_lockless();
}

std::vector<BarFlushInfo> Bar::emit_closed_bars_lockless() {
    std::vector<BarFlushInfo> ret;
    if (ticks.empty()) {
        return ret;
    }

    const auto bar_dir = MarketData::get_derived_bars_base_dir(
        bar_meta.symbol, bar_meta.exchange_name, bar_meta.market_type,
        bar_meta.bar_close_policy, true);

    auto partition_point = std::partition(
        ticks.begin(), ticks.end(),
        [this](const MarketData::OhlcvDatasetTick &tick) {
            return !tick.is_closed_tick(bar_close_policy, is_synced_with_live,
                                        latency_buffer_ms);
        });

    for (auto it = partition_point; it != ticks.end(); ++it) {
        if (it->number_of_trades > 0) {
            ret.push_back(BarFlushInfo{*it, bar_dir});
        }
    }

    ticks.erase(partition_point, ticks.end());
    return ret;
}

Bar::Bar(const MarketData::OhlcvBarMeta &meta) {
    if (meta.last_storage_tick_close_time_ms == 0) {
        this->is_dataset_init_first_time = true;
    }
    this->bar_meta = meta;
    this->bar_close_policy = meta.bar_close_policy;

    if (!this->is_dataset_init_first_time) {
        this->ticks.push_back(MarketData::create_new_ohlcv_tick(
            bar_close_policy, meta.last_storage_tick_close_time_ms));
        this->last_trade_timestamp_ms = meta.last_storage_tick_close_time_ms;
    }
}

MarketData::OhlcvDatasetTick MarketData::create_new_ohlcv_tick(
    const MarketData::BarClosePolicy &bar_close_policy,
    long long kline_open_time_ms) {
    MarketData::OhlcvDatasetTick ret;
    ret.kline_open_time = kline_open_time_ms;
    ret.number_of_trades = 0;

    if (bar_close_policy.is_time_bar) {
        ret.kline_close_time = kline_open_time_ms + bar_close_policy.time_ms;
    }

    return ret;
}

void Bar::_handle_trade_for_ticks(
    std::vector<MarketData::OhlcvDatasetTick> &ticks,
    const MarketData::NormalizedParsedTrade &trade) {
    if (ticks.empty()) {
        ticks.push_back(MarketData::create_new_ohlcv_tick(
            bar_close_policy,
            this->bar_meta.bar_close_policy.is_time_bar
                ? DatetimeUtils::get_fully_completed_bar_close_time_from_input(
                      trade.timestamp, this->bar_meta.bar_close_policy.time_ms)
                : trade.timestamp));
    }

    if (bar_close_policy.is_time_bar) {
        for (auto &tick : ticks) {
            _handle_time_based_bar_trade(tick, trade);
        }
    } else if (bar_close_policy.is_dollar_bar) {
        for (auto &tick : ticks) {
            _handle_dollar_vol_based_bar_trade(tick, trade);
        }
    } else if (bar_close_policy.is_tick_count_bar) {
        for (auto &tick : ticks) {
            _handle_tick_count_based_bar_trade(tick, trade);
        }
    }
}

void Bar::stream_disconnected_ack() {
    std::lock_guard<std::mutex> lock(this->mutex);
    this->is_synced_with_live = false;
    this->first_live_stream_trade_timestamp_ms = 0;
    this->first_live_stream_trade_id = "";
    this->buffered_trades_unsynced = true;
    this->buffered_trades.clear();
    this->last_sync_time = std::nullopt;
}

void Bar::sync_buffered_trades() {
    this->buffered_trades_unsynced = false;
    for (const auto &trade : this->buffered_trades) {
        this->add_trade_from_live_stream_buffer(trade);
    }
    this->buffered_trades.clear();
}

std::optional<long long> Bar::get_seconds_since_last_sync() {
    std::lock_guard<std::mutex> lock(this->mutex);
    if (this->last_sync_time.has_value()) {
        return std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::steady_clock::now() - *this->last_sync_time)
            .count();
    }
    return std::nullopt;
}