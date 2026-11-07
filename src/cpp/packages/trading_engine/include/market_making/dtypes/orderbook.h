#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "market_making/dtypes/ring_buffer_book.h"
#include "nlohmann/json.hpp"

namespace Core {

struct BidAskWithQty {
    double bid_price;
    double bid_qty;
    double ask_price;
    double ask_qty;
    long long bid_updated_ms = 0;
    long long ask_updated_ms = 0;
};

// PriceLevel is still used by the BBA helper to describe a point-in-time
// quote quantity + updated timestamp for external serialization. The live
// storage is now entirely inside `RingBufferBook` via `RingPriceLevel`.
struct PriceLevel {
    double qty;
    long long updated_ms;
};

// Lightweight transport type for bulk full-refresh / levels-only payloads
// flowing from the market-data pipeline into the engine. Not a live book —
// just a carrier for (price, qty) pairs. The engine iterates these and
// calls `ring.apply_*` on the destination live book.
struct OrderBookLevels {
    // (price, qty) pairs. Order is not significant for correctness, but
    // the engine rebuilds the ring by iterating asks ascending and bids
    // descending so best-tracking settles on the right side. Callers
    // that can sort cheaply should do so; otherwise the engine sorts.
    std::vector<std::pair<double, double>> asks;
    std::vector<std::pair<double, double>> bids;
    long long timestamp_ms = 0;
    long long last_sequence_number = 0;

    nlohmann::json to_json() const;
    static OrderBookLevels from_json(const nlohmann::json& j);
};

struct OrderBook {
    // Timestamps and metadata.
    std::chrono::system_clock::time_point timestamp;
    std::chrono::system_clock::time_point last_full_refresh_timestamp;
    long long last_sequence_number = 0;
    bool is_in_sync = false;
    bool was_just_refreshed = false;
    bool has_received_update = false;
    std::atomic<uint64_t> seq{0};

    // BBA fast-path snapshot. Written by `publish_bba()` from inside a
    // begin_write()/end_write() bracket, so these stores are ordered by
    // the outer `seq` counter — the same seqlock that protects the ring.
    // Readers of `get_best_bid_with_qty()` etc. spin on `seq` and then
    // read these atomics relaxed; no rwlock, no shared_lock, no blocking.
    //
    // INVARIANT: every mutation of the ring under a begin_write/end_write
    // bracket MUST call `publish_bba()` before `end_write()` so the
    // atomics reflect post-mutation state.
    std::atomic<double>    bba_bid_price{0.0};
    std::atomic<double>    bba_bid_qty{0.0};
    std::atomic<long long> bba_bid_updated_ms{0};
    std::atomic<double>    bba_ask_price{0.0};
    std::atomic<double>    bba_ask_qty{0.0};
    std::atomic<long long> bba_ask_updated_ms{0};
    std::atomic<bool>      bba_has_bid{false};
    std::atomic<bool>      bba_has_ask{false};

    // Authoritative storage. Runtime-sized; default window_bits=19
    // (524K slots, ~16 MB virtual — lazy-paged) for exchange books,
    // Polymarket books pass window_bits=10 (1024 slots, ~32 KB) via
    // the explicit OrderBook(window_bits) constructor.
    // Engine populates `ring.tick_size` at construction time via
    // `Core::Engine::get_tick_size` for exchange instruments, or hardcodes
    // `0.001` for Polymarket books.
    RingBufferBook ring;

    OrderBook() : ring(RingWindow::kExchange) {}
    explicit OrderBook(size_t ring_window_bits) : ring(ring_window_bits) {}
    OrderBook(const OrderBook& other)
        : timestamp(other.timestamp),
          last_full_refresh_timestamp(other.last_full_refresh_timestamp),
          last_sequence_number(other.last_sequence_number),
          is_in_sync(other.is_in_sync),
          was_just_refreshed(other.was_just_refreshed),
          has_received_update(other.has_received_update),
          seq(other.seq.load(std::memory_order_relaxed)),
          ring(other.ring) {
        publish_bba();
    }
    OrderBook& operator=(const OrderBook& other) {
        if (this != &other) {
            timestamp = other.timestamp;
            last_full_refresh_timestamp = other.last_full_refresh_timestamp;
            last_sequence_number = other.last_sequence_number;
            is_in_sync = other.is_in_sync;
            was_just_refreshed = other.was_just_refreshed;
            has_received_update = other.has_received_update;
            seq.store(other.seq.load(std::memory_order_relaxed),
                      std::memory_order_relaxed);
            ring = other.ring;
            publish_bba();
        }
        return *this;
    }
    OrderBook(OrderBook&& other) noexcept
        : timestamp(other.timestamp),
          last_full_refresh_timestamp(other.last_full_refresh_timestamp),
          last_sequence_number(other.last_sequence_number),
          is_in_sync(other.is_in_sync),
          was_just_refreshed(other.was_just_refreshed),
          has_received_update(other.has_received_update),
          seq(other.seq.load(std::memory_order_relaxed)),
          ring(std::move(other.ring)) {
        publish_bba();
    }
    OrderBook& operator=(OrderBook&& other) noexcept {
        if (this != &other) {
            timestamp = other.timestamp;
            last_full_refresh_timestamp = other.last_full_refresh_timestamp;
            last_sequence_number = other.last_sequence_number;
            is_in_sync = other.is_in_sync;
            was_just_refreshed = other.was_just_refreshed;
            has_received_update = other.has_received_update;
            seq.store(other.seq.load(std::memory_order_relaxed),
                      std::memory_order_relaxed);
            ring = std::move(other.ring);
            publish_bba();
        }
        return *this;
    }

    inline void begin_write() { seq.fetch_add(1, std::memory_order_seq_cst); }
    inline void end_write() { seq.fetch_add(1, std::memory_order_release); }

    // Publishes the current ring-tracked top-of-book to the BBA seqlock
    // atomics. PRECONDITION: caller holds the outer begin_write/end_write
    // bracket open (i.e. `seq` is odd), OR the object is not yet shared
    // with any other thread. Honors `is_in_sync`: when false, clears
    // `bba_has_bid` / `bba_has_ask` so readers return nullopt.
    void publish_bba();

    void copy_from(const OrderBook& other);

    // Clears the ring and re-populates it from an OrderBookLevels payload.
    // Used by full-refresh and levels-only-refresh writer sites.
    // Preserves `ring.tick_size`. Iterates asks ascending and bids
    // descending so the ring's best-tracking settles on the real best.
    // Requires caller to hold the outer begin_write/end_write bracket.
    void rebuild_ring_from_levels(const OrderBookLevels& levels);

    std::optional<double> get_best_ask() const;
    std::optional<double> get_best_bid() const;
    std::optional<std::pair<double, double>> get_best_ask_with_qty() const;
    std::optional<std::pair<double, double>> get_best_bid_with_qty() const;
    std::optional<BidAskWithQty> get_best_bid_ask_with_qty() const;
    std::optional<double> get_spread() const;

    std::optional<double> get_ask_depth(double slippage) const;
    std::optional<double> get_bid_depth(double slippage) const;

    std::optional<double> get_avg_ask_size(int levels) const;
    std::optional<double> get_avg_bid_size(int levels) const;

    std::optional<long long> get_ms_since_last_update() const;
    std::optional<long long> get_ms_since_last_full_refresh() const;

    std::optional<double> get_book_volume_before_bid(double price) const;
    std::optional<double> get_book_volume_before_ask(double price) const;
    std::optional<int> get_number_of_levels_before_bid(double price) const;
    std::optional<int> get_number_of_levels_before_ask(double price) const;

    // Walks top-N levels of both sides and returns
    // (sum_bid_qty - sum_ask_qty) / (sum_bid_qty + sum_ask_qty), or
    // nullopt when not in sync / either side empty.
    std::optional<double> get_top_n_volume_imbalance(int n) const;

    // Lock-free BBA staleness accessors — read the bba_*_updated_ms
    // atomics via the outer seqlock.
    std::optional<long long> get_best_bid_updated_ms() const;
    std::optional<long long> get_best_ask_updated_ms() const;

    nlohmann::json to_json() const;
    // Returns unique_ptr because OrderBook is ~16 MB (contains the ring)
    // and can't fit on the default stack. Callers should either move
    // into a heap container or dereference with care.
    static std::unique_ptr<OrderBook> from_json(const nlohmann::json& json);
};

struct OrderBookUpdate {
    std::vector<std::pair<double, double>> asks;
    std::vector<std::pair<double, double>> bids;
    std::string symbol_full;
    long long timestamp;
    long long sequence_start;
    long long sequence_end;
};

}  // namespace Core
