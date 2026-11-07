#include "market_making/dtypes/orderbook.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "chrono_utils.h"

// ---------------------------------------------------------------------------
// copy_from — copies ring + metadata. The ring already knows how to
// copy itself (atomics loaded explicitly in its copy operator).
// ---------------------------------------------------------------------------
void Core::OrderBook::copy_from(const OrderBook &other) {
    timestamp = other.timestamp;
    last_full_refresh_timestamp = other.last_full_refresh_timestamp;
    last_sequence_number = other.last_sequence_number;
    is_in_sync = other.is_in_sync;
    was_just_refreshed = other.was_just_refreshed;
    has_received_update = other.has_received_update;
    ring = other.ring;
    publish_bba();
}

// ---------------------------------------------------------------------------
// rebuild_ring_from_levels — clears the ring (preserving tick_size) and
// repopulates it from an OrderBookLevels transport payload. Used by
// full-refresh and levels-only-refresh writer sites where the engine gets
// a bulk snapshot of levels and needs to flush the ring to match.
//
// Iteration order matters: asks ascending, bids descending. The ring's
// apply_level uses the first insert on each side to establish
// best_ask_tick / best_bid_tick, and subsequent inserts must stay within
// WINDOW_SIZE of that first best. Ascending asks → lowest ask first →
// correct best. Descending bids → highest bid first → correct best.
// Out-of-window trailing levels are silently dropped (same semantics as
// Phase 2 mirror helpers).
// ---------------------------------------------------------------------------
void Core::OrderBook::rebuild_ring_from_levels(
    const OrderBookLevels &levels) {
    if (ring.tick_size <= 0.0) return;
    double saved_tick_size = ring.tick_size;
    ring.clear();
    ring.tick_size = saved_tick_size;

    // Copy + sort asks ascending.
    std::vector<std::pair<double, double>> sorted_asks = levels.asks;
    std::sort(sorted_asks.begin(), sorted_asks.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });
    uint32_t updated_sec =
        static_cast<uint32_t>(levels.timestamp_ms / 1000);
    for (const auto &[price, qty] : sorted_asks) {
        if (qty <= 0.0) continue;
        uint32_t tick = ring.price_to_tick(price);
        if (tick == 0) continue;
        (void)ring.apply_ask(tick, qty, updated_sec);
    }

    // Copy + sort bids descending.
    std::vector<std::pair<double, double>> sorted_bids = levels.bids;
    std::sort(sorted_bids.begin(), sorted_bids.end(),
              [](const auto &a, const auto &b) { return a.first > b.first; });
    for (const auto &[price, qty] : sorted_bids) {
        if (qty <= 0.0) continue;
        uint32_t tick = ring.price_to_tick(price);
        if (tick == 0) continue;
        (void)ring.apply_bid(tick, qty, updated_sec);
    }

    ring.is_in_sync = is_in_sync;
}

// ---------------------------------------------------------------------------
// publish_bba — reads the ring's best_bid_tick / best_ask_tick atomics,
// then reads the corresponding slot's qty + updated_sec, and publishes to
// the BBA atomic fast-path. Caller owns the outer seqlock bracket.
// ---------------------------------------------------------------------------
void Core::OrderBook::publish_bba() {
    if (is_in_sync) {
        uint32_t bb_tick =
            ring.best_bid_tick.load(std::memory_order_relaxed);
        if (bb_tick != 0) {
            size_t slot = bb_tick & ring.mask;
            // Trust the tag: we're inside the writer bracket, so the
            // slot we're reading was just written.
            const auto &lvl = ring.bids[slot];
            bba_bid_price.store(
                static_cast<double>(bb_tick) * ring.tick_size,
                std::memory_order_relaxed);
            bba_bid_qty.store(lvl.qty, std::memory_order_relaxed);
            // updated_sec → ms for the BBA atomic.
            bba_bid_updated_ms.store(
                static_cast<long long>(lvl.updated_sec) * 1000LL,
                std::memory_order_relaxed);
            bba_has_bid.store(true, std::memory_order_relaxed);
        } else {
            bba_has_bid.store(false, std::memory_order_relaxed);
        }
        uint32_t ba_tick =
            ring.best_ask_tick.load(std::memory_order_relaxed);
        if (ba_tick != 0) {
            size_t slot = ba_tick & ring.mask;
            const auto &lvl = ring.asks[slot];
            bba_ask_price.store(
                static_cast<double>(ba_tick) * ring.tick_size,
                std::memory_order_relaxed);
            bba_ask_qty.store(lvl.qty, std::memory_order_relaxed);
            bba_ask_updated_ms.store(
                static_cast<long long>(lvl.updated_sec) * 1000LL,
                std::memory_order_relaxed);
            bba_has_ask.store(true, std::memory_order_relaxed);
        } else {
            bba_has_ask.store(false, std::memory_order_relaxed);
        }
    } else {
        bba_has_bid.store(false, std::memory_order_relaxed);
        bba_has_ask.store(false, std::memory_order_relaxed);
    }
}

// ---------------------------------------------------------------------------
// BBA fast-path readers — seqlock retry on the outer `seq` counter, reading
// the BBA atomics. Unchanged from Phase 1/2: top-of-book queries stay on
// the zero-copy atomic path.
// ---------------------------------------------------------------------------

namespace {

struct BbaRead {
    bool has_bid;
    bool has_ask;
    double bid_price;
    double bid_qty;
    long long bid_updated_ms;
    double ask_price;
    double ask_qty;
    long long ask_updated_ms;
};

inline BbaRead load_bba(const Core::OrderBook &ob) {
    BbaRead r{};
    uint64_t s1, s2;
    do {
        s1 = ob.seq.load(std::memory_order_acquire);
        if (s1 & 1) continue;
        r.has_bid = ob.bba_has_bid.load(std::memory_order_relaxed);
        r.has_ask = ob.bba_has_ask.load(std::memory_order_relaxed);
        r.bid_price = ob.bba_bid_price.load(std::memory_order_relaxed);
        r.bid_qty = ob.bba_bid_qty.load(std::memory_order_relaxed);
        r.bid_updated_ms =
            ob.bba_bid_updated_ms.load(std::memory_order_relaxed);
        r.ask_price = ob.bba_ask_price.load(std::memory_order_relaxed);
        r.ask_qty = ob.bba_ask_qty.load(std::memory_order_relaxed);
        r.ask_updated_ms =
            ob.bba_ask_updated_ms.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        s2 = ob.seq.load(std::memory_order_relaxed);
    } while (s1 != s2);
    return r;
}

}  // namespace

std::optional<double> Core::OrderBook::get_best_ask() const {
    auto r = load_bba(*this);
    if (!r.has_ask) return std::nullopt;
    return r.ask_price;
}

std::optional<double> Core::OrderBook::get_best_bid() const {
    auto r = load_bba(*this);
    if (!r.has_bid) return std::nullopt;
    return r.bid_price;
}

std::optional<std::pair<double, double>>
Core::OrderBook::get_best_ask_with_qty() const {
    auto r = load_bba(*this);
    if (!r.has_ask) return std::nullopt;
    return std::pair<double, double>{r.ask_price, r.ask_qty};
}

std::optional<std::pair<double, double>>
Core::OrderBook::get_best_bid_with_qty() const {
    auto r = load_bba(*this);
    if (!r.has_bid) return std::nullopt;
    return std::pair<double, double>{r.bid_price, r.bid_qty};
}

std::optional<Core::BidAskWithQty>
Core::OrderBook::get_best_bid_ask_with_qty() const {
    auto r = load_bba(*this);
    if (!r.has_bid || !r.has_ask) return std::nullopt;
    return BidAskWithQty{.bid_price = r.bid_price,
                         .bid_qty = r.bid_qty,
                         .ask_price = r.ask_price,
                         .ask_qty = r.ask_qty,
                         .bid_updated_ms = r.bid_updated_ms,
                         .ask_updated_ms = r.ask_updated_ms};
}

std::optional<double> Core::OrderBook::get_spread() const {
    auto r = load_bba(*this);
    if (!r.has_bid || !r.has_ask) return std::nullopt;
    return r.ask_price - r.bid_price;
}

std::optional<long long> Core::OrderBook::get_best_bid_updated_ms() const {
    auto r = load_bba(*this);
    if (!r.has_bid) return std::nullopt;
    return r.bid_updated_ms;
}

std::optional<long long> Core::OrderBook::get_best_ask_updated_ms() const {
    auto r = load_bba(*this);
    if (!r.has_ask) return std::nullopt;
    return r.ask_updated_ms;
}

// ---------------------------------------------------------------------------
// Deep readers — ring-only path with seqlock retry.
//
// All of these require `ring.tick_size > 0`. Books without metadata
// (e.g. uninitialized ones) return nullopt. The Engine ensures that
// every book it stores has `ring.tick_size` set at construction time,
// either from `get_tick_size()` for exchange instruments or hardcoded
// 0.001 for Polymarket books.
// ---------------------------------------------------------------------------

std::optional<double> Core::OrderBook::get_ask_depth(double slippage) const {
    if (ring.tick_size <= 0.0) return std::nullopt;
    uint64_t s1 = seq.load(std::memory_order_acquire);
    if (s1 & 1 || !is_in_sync || !ring.is_in_sync) return std::nullopt;
    uint32_t best = ring.best_ask_tick.load(std::memory_order_relaxed);
    if (best == 0) return std::nullopt;
    double best_px = static_cast<double>(best) * ring.tick_size;
    double max_px = best_px * (1.0 + slippage);
    uint64_t max_tick_u =
        static_cast<uint64_t>(std::round(max_px / ring.tick_size));
    if (max_tick_u > static_cast<uint64_t>(UINT32_MAX)) {
        max_tick_u = UINT32_MAX;
    }
    uint32_t max_tick = static_cast<uint32_t>(max_tick_u);
    double volume = 0.0;
    ring.walk_asks_upward_from(
        best, max_tick,
        [&](const RingPriceLevel &lvl) { volume += lvl.qty; });
    std::atomic_thread_fence(std::memory_order_acquire);
    uint64_t s2 = seq.load(std::memory_order_relaxed);
    if (s1 != s2) return std::nullopt;
    return volume;
}

std::optional<double> Core::OrderBook::get_bid_depth(double slippage) const {
    if (ring.tick_size <= 0.0) return std::nullopt;
    uint64_t s1 = seq.load(std::memory_order_acquire);
    if (s1 & 1 || !is_in_sync || !ring.is_in_sync) return std::nullopt;
    uint32_t best = ring.best_bid_tick.load(std::memory_order_relaxed);
    if (best == 0) return std::nullopt;
    double best_px = static_cast<double>(best) * ring.tick_size;
    double min_px = best_px * (1.0 - slippage);
    if (min_px < 0.0) min_px = 0.0;
    int64_t min_tick_i =
        static_cast<int64_t>(std::round(min_px / ring.tick_size));
    if (min_tick_i < 1) min_tick_i = 1;
    uint32_t min_tick = static_cast<uint32_t>(min_tick_i);
    double volume = 0.0;
    ring.walk_bids_downward_from(
        best, min_tick,
        [&](const RingPriceLevel &lvl) { volume += lvl.qty; });
    std::atomic_thread_fence(std::memory_order_acquire);
    uint64_t s2 = seq.load(std::memory_order_relaxed);
    if (s1 != s2) return std::nullopt;
    return volume;
}

std::optional<double> Core::OrderBook::get_avg_ask_size(int levels) const {
    if (ring.tick_size <= 0.0 || levels <= 0) return std::nullopt;
    uint64_t s1 = seq.load(std::memory_order_acquire);
    if (s1 & 1 || !is_in_sync || !ring.is_in_sync) return std::nullopt;
    uint32_t best = ring.best_ask_tick.load(std::memory_order_relaxed);
    if (best == 0) return std::nullopt;
    double total = 0.0;
    int count = 0;
    ring.walk_asks_upward_from(
        best, UINT32_MAX, [&](const RingPriceLevel &lvl) {
            if (count >= levels) return;
            total += lvl.qty;
            ++count;
        });
    std::atomic_thread_fence(std::memory_order_acquire);
    uint64_t s2 = seq.load(std::memory_order_relaxed);
    if (s1 != s2) return std::nullopt;
    return count > 0 ? total / count : 0.0;
}

std::optional<double> Core::OrderBook::get_avg_bid_size(int levels) const {
    if (ring.tick_size <= 0.0 || levels <= 0) return std::nullopt;
    uint64_t s1 = seq.load(std::memory_order_acquire);
    if (s1 & 1 || !is_in_sync || !ring.is_in_sync) return std::nullopt;
    uint32_t best = ring.best_bid_tick.load(std::memory_order_relaxed);
    if (best == 0) return std::nullopt;
    double total = 0.0;
    int count = 0;
    ring.walk_bids_downward_from(
        best, 1, [&](const RingPriceLevel &lvl) {
            if (count >= levels) return;
            total += lvl.qty;
            ++count;
        });
    std::atomic_thread_fence(std::memory_order_acquire);
    uint64_t s2 = seq.load(std::memory_order_relaxed);
    if (s1 != s2) return std::nullopt;
    return count > 0 ? total / count : 0.0;
}

std::optional<long long> Core::OrderBook::get_ms_since_last_update() const {
    uint64_t s1 = seq.load(std::memory_order_acquire);
    if (s1 & 1 || !is_in_sync) return std::nullopt;
    auto now = ChronoUtils::make_now_utc_timepoint();
    auto duration = now - timestamp;
    long long result =
        std::chrono::duration_cast<std::chrono::milliseconds>(duration)
            .count();
    std::atomic_thread_fence(std::memory_order_acquire);
    uint64_t s2 = seq.load(std::memory_order_relaxed);
    if (s1 != s2) return std::nullopt;
    return result;
}

std::optional<long long>
Core::OrderBook::get_ms_since_last_full_refresh() const {
    uint64_t s1 = seq.load(std::memory_order_acquire);
    if (s1 & 1) return std::nullopt;
    if (last_full_refresh_timestamp.time_since_epoch().count() == 0) {
        return std::nullopt;
    }
    auto now = ChronoUtils::make_now_utc_timepoint();
    auto duration = now - last_full_refresh_timestamp;
    long long result =
        std::chrono::duration_cast<std::chrono::milliseconds>(duration)
            .count();
    std::atomic_thread_fence(std::memory_order_acquire);
    uint64_t s2 = seq.load(std::memory_order_relaxed);
    if (s1 != s2) return std::nullopt;
    return result;
}

std::optional<double>
Core::OrderBook::get_book_volume_before_bid(double price) const {
    if (ring.tick_size <= 0.0) return std::nullopt;
    uint64_t s1 = seq.load(std::memory_order_acquire);
    if (s1 & 1 || !is_in_sync || !ring.is_in_sync) return std::nullopt;
    uint32_t best = ring.best_bid_tick.load(std::memory_order_relaxed);
    if (best == 0) return std::nullopt;
    uint32_t stop_tick = ring.price_to_tick(price);
    double volume = 0.0;
    ring.walk_bids_downward_from(
        best, stop_tick + 1, [&](const RingPriceLevel &lvl) {
            double lvl_price =
                static_cast<double>(lvl.tick_tag) * ring.tick_size;
            volume += lvl_price * lvl.qty;
        });
    std::atomic_thread_fence(std::memory_order_acquire);
    uint64_t s2 = seq.load(std::memory_order_relaxed);
    if (s1 != s2) return std::nullopt;
    return volume;
}

std::optional<double>
Core::OrderBook::get_book_volume_before_ask(double price) const {
    if (ring.tick_size <= 0.0) return std::nullopt;
    uint64_t s1 = seq.load(std::memory_order_acquire);
    if (s1 & 1 || !is_in_sync || !ring.is_in_sync) return std::nullopt;
    uint32_t best = ring.best_ask_tick.load(std::memory_order_relaxed);
    if (best == 0) return std::nullopt;
    uint32_t stop_tick = ring.price_to_tick(price);
    if (stop_tick == 0) return 0.0;
    double volume = 0.0;
    ring.walk_asks_upward_from(
        best, stop_tick - 1, [&](const RingPriceLevel &lvl) {
            double lvl_price =
                static_cast<double>(lvl.tick_tag) * ring.tick_size;
            volume += lvl_price * lvl.qty;
        });
    std::atomic_thread_fence(std::memory_order_acquire);
    uint64_t s2 = seq.load(std::memory_order_relaxed);
    if (s1 != s2) return std::nullopt;
    return volume;
}

std::optional<int>
Core::OrderBook::get_number_of_levels_before_bid(double price) const {
    if (ring.tick_size <= 0.0) return std::nullopt;
    uint64_t s1 = seq.load(std::memory_order_acquire);
    if (s1 & 1 || !is_in_sync || !ring.is_in_sync) return std::nullopt;
    uint32_t best = ring.best_bid_tick.load(std::memory_order_relaxed);
    if (best == 0) return std::nullopt;
    uint32_t stop_tick = ring.price_to_tick(price);
    int count = 0;
    ring.walk_bids_downward_from(
        best, stop_tick + 1,
        [&](const RingPriceLevel &) { ++count; });
    std::atomic_thread_fence(std::memory_order_acquire);
    uint64_t s2 = seq.load(std::memory_order_relaxed);
    if (s1 != s2) return std::nullopt;
    return count;
}

std::optional<int>
Core::OrderBook::get_number_of_levels_before_ask(double price) const {
    if (ring.tick_size <= 0.0) return std::nullopt;
    uint64_t s1 = seq.load(std::memory_order_acquire);
    if (s1 & 1 || !is_in_sync || !ring.is_in_sync) return std::nullopt;
    uint32_t best = ring.best_ask_tick.load(std::memory_order_relaxed);
    if (best == 0) return std::nullopt;
    uint32_t stop_tick = ring.price_to_tick(price);
    if (stop_tick == 0) return 0;
    int count = 0;
    ring.walk_asks_upward_from(
        best, stop_tick - 1,
        [&](const RingPriceLevel &) { ++count; });
    std::atomic_thread_fence(std::memory_order_acquire);
    uint64_t s2 = seq.load(std::memory_order_relaxed);
    if (s1 != s2) return std::nullopt;
    return count;
}

std::optional<double>
Core::OrderBook::get_top_n_volume_imbalance(int n) const {
    if (n <= 0 || ring.tick_size <= 0.0) return std::nullopt;
    uint64_t s1 = seq.load(std::memory_order_acquire);
    if (s1 & 1 || !is_in_sync || !ring.is_in_sync) return std::nullopt;
    uint32_t bb = ring.best_bid_tick.load(std::memory_order_relaxed);
    uint32_t ba = ring.best_ask_tick.load(std::memory_order_relaxed);
    if (bb == 0 || ba == 0) return std::nullopt;

    double bid_sum = 0.0;
    int bid_count = 0;
    ring.walk_bids_downward_from(
        bb, 1, [&](const RingPriceLevel &lvl) {
            if (bid_count >= n) return;
            bid_sum += lvl.qty;
            ++bid_count;
        });
    double ask_sum = 0.0;
    int ask_count = 0;
    ring.walk_asks_upward_from(
        ba, UINT32_MAX, [&](const RingPriceLevel &lvl) {
            if (ask_count >= n) return;
            ask_sum += lvl.qty;
            ++ask_count;
        });

    std::atomic_thread_fence(std::memory_order_acquire);
    uint64_t s2 = seq.load(std::memory_order_relaxed);
    if (s1 != s2) return std::nullopt;
    double total = bid_sum + ask_sum;
    if (total <= 0.0) return std::nullopt;
    return (bid_sum - ask_sum) / total;
}

// ---------------------------------------------------------------------------
// JSON serialization — walks the ring populated bitmaps to emit level lists.
// Encoded format now includes `tick_size` so `from_json` can rebuild the
// ring correctly. Pre-Phase-3 state snapshots without `tick_size` will fail
// to restore — this is expected and documented.
// ---------------------------------------------------------------------------

nlohmann::json Core::OrderBook::to_json() const {
    nlohmann::json json;
    json["tick_size"] = ring.tick_size;
    json["window_bits"] = static_cast<int>(ring.window_bits);
    json["asks"] = nlohmann::json::array();
    json["bids"] = nlohmann::json::array();

    // Walk populated bitmaps.
    if (ring.tick_size > 0.0) {
        uint32_t ba = ring.best_ask_tick.load(std::memory_order_relaxed);
        if (ba != 0) {
            ring.walk_asks_upward_from(
                ba, UINT32_MAX, [&](const RingPriceLevel &lvl) {
                    double price = static_cast<double>(lvl.tick_tag) *
                                   ring.tick_size;
                    long long updated_ms =
                        static_cast<long long>(lvl.updated_sec) * 1000LL;
                    json["asks"].push_back(nlohmann::json::object(
                        {{"price", price},
                         {"quantity", lvl.qty},
                         {"updated_ms", updated_ms}}));
                });
        }
        uint32_t bb = ring.best_bid_tick.load(std::memory_order_relaxed);
        if (bb != 0) {
            ring.walk_bids_downward_from(
                bb, 1, [&](const RingPriceLevel &lvl) {
                    double price = static_cast<double>(lvl.tick_tag) *
                                   ring.tick_size;
                    long long updated_ms =
                        static_cast<long long>(lvl.updated_sec) * 1000LL;
                    json["bids"].push_back(nlohmann::json::object(
                        {{"price", price},
                         {"quantity", lvl.qty},
                         {"updated_ms", updated_ms}}));
                });
        }
    }

    json["timestamp"] = ChronoUtils::to_ms_timestamp(this->timestamp);
    json["is_in_sync"] = this->is_in_sync;
    json["last_full_refresh_timestamp"] =
        ChronoUtils::to_ms_timestamp(this->last_full_refresh_timestamp);
    return json;
}

std::unique_ptr<Core::OrderBook>
Core::OrderBook::from_json(const nlohmann::json &json) {
    // Heap-allocate: Core::OrderBook is ~16 MB (contains the ring) and
    // can't fit on the default 8 MB stack. Use the embedded window_bits
    // if present (post-Phase-4 snapshots) so polymarket books restore
    // at 10 bits, exchange books at 19.
    size_t window_bits =
        static_cast<size_t>(json.value("window_bits",
                                        static_cast<int>(
                                            Core::RingWindow::kExchange)));
    auto order_book = std::make_unique<Core::OrderBook>(window_bits);

    // Pre-Phase-3 state snapshots won't have `tick_size` embedded.
    // We fail-soft here (tick_size=0 produces an inert book that
    // returns nullopt from deep readers); the engine's existing
    // recovery path requests a fresh snapshot from exchanges.
    order_book->ring.tick_size = json.value("tick_size", 0.0);
    order_book->timestamp = ChronoUtils::make_utc_timepoint_from_epoch_ms(
        json["timestamp"].get<long long>());
    order_book->is_in_sync = json.value("is_in_sync", false);

    long long ts = json["timestamp"].get<long long>();
    uint32_t updated_sec = static_cast<uint32_t>(ts / 1000);

    if (order_book->ring.tick_size > 0.0) {
        // Collect and sort on entry so best-tracking establishes correctly.
        std::vector<std::pair<double, double>> asks;
        std::vector<std::pair<double, double>> bids;
        for (const auto &entry : json["asks"]) {
            double price = entry["price"].get<double>();
            double quantity = entry["quantity"].get<double>();
            asks.emplace_back(price, quantity);
        }
        for (const auto &entry : json["bids"]) {
            double price = entry["price"].get<double>();
            double quantity = entry["quantity"].get<double>();
            bids.emplace_back(price, quantity);
        }
        std::sort(asks.begin(), asks.end(),
                  [](const auto &a, const auto &b) {
                      return a.first < b.first;
                  });
        std::sort(bids.begin(), bids.end(),
                  [](const auto &a, const auto &b) {
                      return a.first > b.first;
                  });
        for (const auto &[price, qty] : asks) {
            if (qty <= 0.0) continue;
            uint32_t tick = order_book->ring.price_to_tick(price);
            if (tick == 0) continue;
            (void)order_book->ring.apply_ask(tick, qty, updated_sec);
        }
        for (const auto &[price, qty] : bids) {
            if (qty <= 0.0) continue;
            uint32_t tick = order_book->ring.price_to_tick(price);
            if (tick == 0) continue;
            (void)order_book->ring.apply_bid(tick, qty, updated_sec);
        }
        order_book->ring.is_in_sync = order_book->is_in_sync;
    }

    order_book->publish_bba();
    return order_book;
}

// ---------------------------------------------------------------------------
// OrderBookLevels — lightweight transport type. Simple JSON round-trip.
// ---------------------------------------------------------------------------

nlohmann::json Core::OrderBookLevels::to_json() const {
    nlohmann::json json;
    json["timestamp_ms"] = timestamp_ms;
    json["last_sequence_number"] = last_sequence_number;
    json["asks"] = nlohmann::json::array();
    json["bids"] = nlohmann::json::array();
    for (const auto &[price, qty] : asks) {
        json["asks"].push_back(
            nlohmann::json::object({{"price", price}, {"quantity", qty}}));
    }
    for (const auto &[price, qty] : bids) {
        json["bids"].push_back(
            nlohmann::json::object({{"price", price}, {"quantity", qty}}));
    }
    return json;
}

Core::OrderBookLevels
Core::OrderBookLevels::from_json(const nlohmann::json &j) {
    Core::OrderBookLevels levels;
    levels.timestamp_ms = j.value("timestamp_ms", 0LL);
    levels.last_sequence_number = j.value("last_sequence_number", 0LL);
    for (const auto &entry : j["asks"]) {
        levels.asks.emplace_back(entry["price"].get<double>(),
                                 entry["quantity"].get<double>());
    }
    for (const auto &entry : j["bids"]) {
        levels.bids.emplace_back(entry["price"].get<double>(),
                                 entry["quantity"].get<double>());
    }
    return levels;
}
