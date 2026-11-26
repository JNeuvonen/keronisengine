#pragma once

// ---------------------------------------------------------------------------
// RingBufferBook — fixed-size, tick-indexed order book.
//
// This is the storage backing `Core::OrderBook`: bids/asks are kept in
// heap-allocated flat arrays hashed by `tick_idx & mask`, not in a
// std::map. Goals:
//
//   1. O(1) write path: hash tick_idx → slot, update tag + qty, update the
//      populated bitmap. No tree rebalance, no allocation per update.
//
//   2. O(1) read path per tick: compute slot, check bitmap + tag, read qty.
//      Deep-read walks (e.g. get_ask_depth, top-N imbalance) walk the
//      populated bitmap — `__builtin_ctzll` skips empty words in one op.
//
//   3. Lock-free read retries under a single-writer seqlock:
//      slots are 16-byte POD (uint32 tag, uint32 seconds, double qty).
//      Readers seqlock-retry and never dereference through a tree.
//
//   4. Automatic window behavior: as the feed sends add/delete events,
//      the ring self-maintains. Ticks window_size apart alias to the same
//      slot, but tag validation rejects stale hits. Violent moves larger
//      than window_size in a single batch are handled by the engine's
//      existing full-refresh resync path.
//
//   5. **Lazy-paged heap storage.** Slot arrays live on the heap via
//      `std::make_unique_for_overwrite<RingPriceLevel[]>(N)`. For large
//      allocations, glibc's malloc uses anonymous mmap, and Linux maps
//      freshly-allocated anonymous pages to a shared zero-page via
//      copy-on-write — they cost **zero** physical memory until the
//      first write touches them. Each populated slot backs one 4 KB
//      page. The bitmap is small enough to eagerly allocate on the heap.
//
//   6. **Runtime-configurable window size.** The ctor takes `window_bits`;
//      production exchange books use 19 (524K slots, 16 MB virtual),
//      Polymarket books use 10 (1024 slots, ~32 KB).
// ---------------------------------------------------------------------------

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>

namespace Core {

// Packed 16-byte slot. `tick_tag == 0` means empty (tick 0 is never a valid
// level — price 0 is not a thing). The tag is the full integer tick index;
// `tick_tag & mask` equals the slot index, so tag mismatch means a wrapped
// stale write.
struct RingPriceLevel {
    uint32_t tick_tag;     // 0 = empty; else == the tick index this slot holds
    uint32_t updated_sec;  // seconds since epoch; coarse-grained staleness
    double   qty;          // quantity at this level
};
static_assert(sizeof(RingPriceLevel) == 16,
              "RingPriceLevel must be exactly 16 bytes for cache behavior");

class RingBufferBook {
   public:
    // Production exchange books use 19 bits → 524,288 slots → 16 MB virtual
    // per side. Polymarket books use 10 → 1024 slots → 16 KB per side.
    static constexpr size_t kDefaultWindowBits = 19;
    static constexpr size_t kMinWindowBits = 4;
    static constexpr size_t kMaxWindowBits = 30;

    // ---- Sync / metadata (public: existing call sites read these) ----
    std::atomic<uint64_t> seq{0};
    std::atomic<uint32_t> best_bid_tick{0};  // 0 = no bid
    std::atomic<uint32_t> best_ask_tick{0};  // 0 = no ask

    double tick_size = 0.0;  // price per tick (e.g. 0.01 for BTCUSDT spot)
    bool is_in_sync = false;

    // ---- Sizing constants, fixed at construction ----
    const size_t window_bits;
    const size_t window_size;    // 1 << window_bits
    const size_t mask;           // window_size - 1
    const size_t bitmap_words;   // (window_size + 63) / 64

    // ---- Slot arrays: heap-allocated, lazy-paged ----
    // Default-initialized (NOT value-initialized). The allocation is a
    // fresh anonymous mmap — pages start as the shared zero page until
    // the first write. Reads of never-written slots are gated by the
    // populated bitmap below, so we never actually observe indeterminate
    // memory through the public API.
    std::unique_ptr<RingPriceLevel[]> bids;
    std::unique_ptr<RingPriceLevel[]> asks;

    // ---- Populated-slot bitmap (one bit per slot; 1 = level present) ----
    // Also heap-allocated so Polymarket (small window) doesn't pay a
    // 128 KB bitmap tax. Mutable because find_next_populated lazy-clears
    // stale bits during const reads.
    mutable std::unique_ptr<uint64_t[]> bids_pop;
    mutable std::unique_ptr<uint64_t[]> asks_pop;

    // -----------------------------------------------------------------------
    // Constructors
    // -----------------------------------------------------------------------
    explicit RingBufferBook(size_t window_bits_arg = kDefaultWindowBits)
        : window_bits(clamp_bits(window_bits_arg)),
          window_size(1ULL << window_bits),
          mask(window_size - 1),
          bitmap_words((window_size + 63) / 64),
          bids(std::make_unique_for_overwrite<RingPriceLevel[]>(window_size)),
          asks(std::make_unique_for_overwrite<RingPriceLevel[]>(window_size)),
          bids_pop(make_zeroed_bitmap(bitmap_words)),
          asks_pop(make_zeroed_bitmap(bitmap_words)) {}

    RingBufferBook(const RingBufferBook& other) noexcept
        : seq(other.seq.load(std::memory_order_relaxed)),
          best_bid_tick(other.best_bid_tick.load(std::memory_order_relaxed)),
          best_ask_tick(other.best_ask_tick.load(std::memory_order_relaxed)),
          tick_size(other.tick_size),
          is_in_sync(other.is_in_sync),
          window_bits(other.window_bits),
          window_size(other.window_size),
          mask(other.mask),
          bitmap_words(other.bitmap_words),
          bids(std::make_unique_for_overwrite<RingPriceLevel[]>(window_size)),
          asks(std::make_unique_for_overwrite<RingPriceLevel[]>(window_size)),
          bids_pop(make_zeroed_bitmap(bitmap_words)),
          asks_pop(make_zeroed_bitmap(bitmap_words)) {
        // Copy only populated slots by walking the bitmap. Unpopulated
        // pages stay on the shared zero page — critical for memory.
        copy_populated_from(other);
    }

    RingBufferBook& operator=(const RingBufferBook& other) noexcept {
        if (this == &other) return *this;
        // Re-allocation path when window sizes differ. This is slow
        // (free + fresh mmap) but happens only on cold paths.
        if (window_bits != other.window_bits) {
            // Can't reassign the const members, so this is a fatal
            // programming error at runtime. Every reasonable callsite
            // assigns between books of identical size.
            std::abort();
        }
        seq.store(other.seq.load(std::memory_order_relaxed),
                  std::memory_order_relaxed);
        best_bid_tick.store(
            other.best_bid_tick.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        best_ask_tick.store(
            other.best_ask_tick.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        tick_size = other.tick_size;
        is_in_sync = other.is_in_sync;
        // Zero destination bitmap before copying so dest's populated-set
        // is an exact match for other's.
        std::memset(bids_pop.get(), 0, bitmap_words * sizeof(uint64_t));
        std::memset(asks_pop.get(), 0, bitmap_words * sizeof(uint64_t));
        copy_populated_from(other);
        return *this;
    }

    // Moves: transfer heap pointers. Atomics load + store.
    RingBufferBook(RingBufferBook&& other) noexcept
        : seq(other.seq.load(std::memory_order_relaxed)),
          best_bid_tick(other.best_bid_tick.load(std::memory_order_relaxed)),
          best_ask_tick(other.best_ask_tick.load(std::memory_order_relaxed)),
          tick_size(other.tick_size),
          is_in_sync(other.is_in_sync),
          window_bits(other.window_bits),
          window_size(other.window_size),
          mask(other.mask),
          bitmap_words(other.bitmap_words),
          bids(std::move(other.bids)),
          asks(std::move(other.asks)),
          bids_pop(std::move(other.bids_pop)),
          asks_pop(std::move(other.asks_pop)) {}

    RingBufferBook& operator=(RingBufferBook&& other) noexcept {
        if (this == &other) return *this;
        if (window_bits != other.window_bits) std::abort();
        seq.store(other.seq.load(std::memory_order_relaxed),
                  std::memory_order_relaxed);
        best_bid_tick.store(
            other.best_bid_tick.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        best_ask_tick.store(
            other.best_ask_tick.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        tick_size = other.tick_size;
        is_in_sync = other.is_in_sync;
        bids = std::move(other.bids);
        asks = std::move(other.asks);
        bids_pop = std::move(other.bids_pop);
        asks_pop = std::move(other.asks_pop);
        return *this;
    }

    // -----------------------------------------------------------------------
    // Seqlock brackets
    // -----------------------------------------------------------------------
    inline void begin_write() noexcept {
        seq.fetch_add(1, std::memory_order_seq_cst);
    }
    inline void end_write() noexcept {
        seq.fetch_add(1, std::memory_order_release);
    }

    // -----------------------------------------------------------------------
    // Tick <-> price conversion
    // -----------------------------------------------------------------------
    inline uint32_t price_to_tick(double price) const noexcept {
        if (tick_size <= 0.0 || price <= 0.0) return 0;
        double t = std::round(price / tick_size);
        if (t <= 0.0) return 0;
        if (t > static_cast<double>(UINT32_MAX)) return UINT32_MAX;
        return static_cast<uint32_t>(t);
    }
    inline double tick_to_price(uint32_t tick) const noexcept {
        return static_cast<double>(tick) * tick_size;
    }

    // -----------------------------------------------------------------------
    // Writer: apply one level change. qty=0 means delete. Caller owns the
    // begin_write/end_write bracket.
    // -----------------------------------------------------------------------
    bool apply_bid(uint32_t tick, double qty, uint32_t updated_sec) noexcept {
        return apply_level_impl(/*is_bid=*/true, tick, qty, updated_sec);
    }
    bool apply_ask(uint32_t tick, double qty, uint32_t updated_sec) noexcept {
        return apply_level_impl(/*is_bid=*/false, tick, qty, updated_sec);
    }

    // -----------------------------------------------------------------------
    // Convenience: clear everything. Used by full-refresh resync.
    // Zeros only the bitmap (eager) and the best trackers. Slot arrays are
    // NOT zeroed — future writes land on whatever was there, and the
    // bitmap gates all reads, so unpopulated slots remain invisible even
    // with stale bytes. This keeps `clear()` O(bitmap_size) rather than
    // O(slot_array_size) and preserves lazy-paging on the slot array.
    // -----------------------------------------------------------------------
    void clear() noexcept {
        std::memset(bids_pop.get(), 0, bitmap_words * sizeof(uint64_t));
        std::memset(asks_pop.get(), 0, bitmap_words * sizeof(uint64_t));
        best_bid_tick.store(0, std::memory_order_relaxed);
        best_ask_tick.store(0, std::memory_order_relaxed);
        is_in_sync = false;
    }

    // -----------------------------------------------------------------------
    // Readers. All const. Caller is expected to wrap in seqlock retry
    // if strict consistency is required; otherwise a single-shot read is
    // bounded by the outer begin_write/end_write semantics.
    // -----------------------------------------------------------------------

    std::optional<double> get_best_bid_price() const noexcept {
        uint32_t t = best_bid_tick.load(std::memory_order_acquire);
        if (t == 0) return std::nullopt;
        return tick_to_price(t);
    }
    std::optional<double> get_best_ask_price() const noexcept {
        uint32_t t = best_ask_tick.load(std::memory_order_acquire);
        if (t == 0) return std::nullopt;
        return tick_to_price(t);
    }
    std::optional<double> get_spread() const noexcept {
        auto b = get_best_bid_price();
        auto a = get_best_ask_price();
        if (!b || !a) return std::nullopt;
        return *a - *b;
    }

    std::optional<double> get_ask_depth(double slippage) const noexcept {
        uint32_t best = best_ask_tick.load(std::memory_order_acquire);
        if (best == 0 || !is_in_sync) return std::nullopt;
        double best_px = tick_to_price(best);
        double max_px = best_px * (1.0 + slippage);
        uint32_t max_tick = price_to_tick(max_px);
        double volume = 0.0;
        walk_asks_upward_from(best, max_tick,
                              [&](const RingPriceLevel& lv) { volume += lv.qty; });
        return volume;
    }
    std::optional<double> get_bid_depth(double slippage) const noexcept {
        uint32_t best = best_bid_tick.load(std::memory_order_acquire);
        if (best == 0 || !is_in_sync) return std::nullopt;
        double best_px = tick_to_price(best);
        double min_px = best_px * (1.0 - slippage);
        uint32_t min_tick = price_to_tick(min_px);
        double volume = 0.0;
        walk_bids_downward_from(best, min_tick,
                                [&](const RingPriceLevel& lv) { volume += lv.qty; });
        return volume;
    }

    std::optional<double> get_book_volume_before_bid(double price) const noexcept {
        uint32_t best = best_bid_tick.load(std::memory_order_acquire);
        if (best == 0 || !is_in_sync) return std::nullopt;
        uint32_t stop_tick = price_to_tick(price);
        double volume = 0.0;
        walk_bids_downward_from(best, stop_tick + 1,
                                [&](const RingPriceLevel& lv) {
                                    volume += static_cast<double>(lv.tick_tag)
                                              * tick_size * lv.qty;
                                });
        return volume;
    }
    std::optional<double> get_book_volume_before_ask(double price) const noexcept {
        uint32_t best = best_ask_tick.load(std::memory_order_acquire);
        if (best == 0 || !is_in_sync) return std::nullopt;
        uint32_t stop_tick = price_to_tick(price);
        if (stop_tick == 0) return 0.0;
        double volume = 0.0;
        walk_asks_upward_from(best, stop_tick - 1,
                              [&](const RingPriceLevel& lv) {
                                  volume += static_cast<double>(lv.tick_tag)
                                            * tick_size * lv.qty;
                              });
        return volume;
    }

    std::optional<int> get_number_of_levels_before_bid(double price) const noexcept {
        uint32_t best = best_bid_tick.load(std::memory_order_acquire);
        if (best == 0 || !is_in_sync) return std::nullopt;
        uint32_t stop_tick = price_to_tick(price);
        int count = 0;
        walk_bids_downward_from(best, stop_tick + 1,
                                [&](const RingPriceLevel&) { ++count; });
        return count;
    }
    std::optional<int> get_number_of_levels_before_ask(double price) const noexcept {
        uint32_t best = best_ask_tick.load(std::memory_order_acquire);
        if (best == 0 || !is_in_sync) return std::nullopt;
        uint32_t stop_tick = price_to_tick(price);
        if (stop_tick == 0) return 0;
        int count = 0;
        walk_asks_upward_from(best, stop_tick - 1,
                              [&](const RingPriceLevel&) { ++count; });
        return count;
    }

    std::optional<double> get_top_n_volume_imbalance(int n) const noexcept {
        if (n <= 0 || !is_in_sync) return std::nullopt;
        uint32_t bb = best_bid_tick.load(std::memory_order_acquire);
        uint32_t ba = best_ask_tick.load(std::memory_order_acquire);
        if (bb == 0 || ba == 0) return std::nullopt;

        double bid_sum = 0.0;
        int bid_count = 0;
        walk_bids_downward_from(bb, 0, [&](const RingPriceLevel& lv) {
            if (bid_count >= n) return;
            bid_sum += lv.qty;
            ++bid_count;
        });

        double ask_sum = 0.0;
        int ask_count = 0;
        walk_asks_upward_from(ba, UINT32_MAX, [&](const RingPriceLevel& lv) {
            if (ask_count >= n) return;
            ask_sum += lv.qty;
            ++ask_count;
        });

        double total = bid_sum + ask_sum;
        if (total <= 0.0) return std::nullopt;
        return (bid_sum - ask_sum) / total;
    }

    // -----------------------------------------------------------------------
    // Introspection helpers (primarily for tests).
    // -----------------------------------------------------------------------
    bool slot_is_populated_bid(uint32_t tick) const noexcept {
        if (tick == 0) return false;
        size_t slot = tick & mask;
        if ((bids_pop[slot >> 6] & (1ULL << (slot & 63))) == 0) return false;
        return bids[slot].tick_tag == tick;
    }
    bool slot_is_populated_ask(uint32_t tick) const noexcept {
        if (tick == 0) return false;
        size_t slot = tick & mask;
        if ((asks_pop[slot >> 6] & (1ULL << (slot & 63))) == 0) return false;
        return asks[slot].tick_tag == tick;
    }
    double get_qty_at_bid_tick(uint32_t tick) const noexcept {
        if (!slot_is_populated_bid(tick)) return 0.0;
        return bids[tick & mask].qty;
    }
    double get_qty_at_ask_tick(uint32_t tick) const noexcept {
        if (!slot_is_populated_ask(tick)) return 0.0;
        return asks[tick & mask].qty;
    }

    // -----------------------------------------------------------------------
    // Walk helpers — visit every valid populated level in the given
    // direction, between `start` (inclusive) and `stop` (inclusive).
    // Public so OrderBook reader methods can call them directly.
    // -----------------------------------------------------------------------
    template <typename Fn>
    void walk_bids_downward_from(uint32_t start, uint32_t stop, Fn&& fn) const {
        if (start == 0) return;
        uint32_t lower = std::max<uint32_t>(stop, 1);
        uint32_t window_lower =
            (start > window_size) ? (start - window_size + 1) : 1;
        lower = std::max(lower, window_lower);
        for (int64_t t = static_cast<int64_t>(start);
             t >= static_cast<int64_t>(lower); --t) {
            size_t slot = static_cast<size_t>(t) & mask;
            uint64_t word_idx = slot >> 6;
            uint64_t bit_mask = 1ULL << (slot & 63);
            if ((bids_pop[word_idx] & bit_mask) == 0) continue;
            if (bids[slot].tick_tag != static_cast<uint32_t>(t)) continue;
            fn(bids[slot]);
        }
    }
    template <typename Fn>
    void walk_asks_upward_from(uint32_t start, uint32_t stop, Fn&& fn) const {
        if (start == 0) return;
        uint64_t upper = std::min<uint64_t>(stop, UINT32_MAX);
        uint64_t window_upper = static_cast<uint64_t>(start) + window_size - 1;
        if (window_upper > UINT32_MAX) window_upper = UINT32_MAX;
        upper = std::min(upper, window_upper);
        for (uint64_t t = static_cast<uint64_t>(start); t <= upper; ++t) {
            size_t slot = static_cast<size_t>(t) & mask;
            uint64_t word_idx = slot >> 6;
            uint64_t bit_mask = 1ULL << (slot & 63);
            if ((asks_pop[word_idx] & bit_mask) == 0) continue;
            if (asks[slot].tick_tag != static_cast<uint32_t>(t)) continue;
            fn(asks[slot]);
        }
    }

   private:
    static constexpr size_t clamp_bits(size_t b) noexcept {
        if (b < kMinWindowBits) return kMinWindowBits;
        if (b > kMaxWindowBits) return kMaxWindowBits;
        return b;
    }

    static std::unique_ptr<uint64_t[]> make_zeroed_bitmap(size_t n_words) {
        // value-initialized (zero) — small by design so the eager zero
        // cost is trivial. At window_bits=19 → 8192 words → 64 KB per
        // side. At window_bits=10 → 16 words → 128 B per side.
        return std::unique_ptr<uint64_t[]>(new uint64_t[n_words]{});
    }

    // Copy only populated slots from `other` into *this. Caller is
    // responsible for ensuring destination bitmaps are in the correct
    // starting state (typically zero, just allocated).
    void copy_populated_from(const RingBufferBook& other) noexcept {
        for (size_t w = 0; w < bitmap_words; ++w) {
            uint64_t bw = other.bids_pop[w];
            while (bw) {
                int bit = __builtin_ctzll(bw);
                size_t slot = w * 64 + bit;
                bids[slot] = other.bids[slot];
                bw &= bw - 1;
            }
            uint64_t aw = other.asks_pop[w];
            while (aw) {
                int bit = __builtin_ctzll(aw);
                size_t slot = w * 64 + bit;
                asks[slot] = other.asks[slot];
                aw &= aw - 1;
            }
            bids_pop[w] = other.bids_pop[w];
            asks_pop[w] = other.asks_pop[w];
        }
    }

    // -----------------------------------------------------------------------
    // Core writer (runtime-dispatched on is_bid to avoid duplicating the
    // body for each side; compiler inlines in practice).
    // -----------------------------------------------------------------------
    bool apply_level_impl(bool is_bid, uint32_t tick, double qty,
                          uint32_t updated_sec) noexcept {
        if (tick == 0) return false;

        auto* arr = is_bid ? bids.get() : asks.get();
        auto* pop = is_bid ? bids_pop.get() : asks_pop.get();
        auto& best_atom = is_bid ? best_bid_tick : best_ask_tick;

        size_t slot = tick & mask;
        uint32_t cur_best = best_atom.load(std::memory_order_relaxed);

        // Reject updates outside the window centered on the current best,
        // to avoid aliasing against live far-away levels. The caller's
        // full-refresh path should catch this and resync.
        if (cur_best != 0) {
            uint32_t delta = (tick > cur_best) ? (tick - cur_best)
                                               : (cur_best - tick);
            if (delta >= window_size) return false;
        }

        if (qty > 0.0) {
            arr[slot].tick_tag = tick;
            arr[slot].qty = qty;
            arr[slot].updated_sec = updated_sec;
            pop[slot >> 6] |= (1ULL << (slot & 63));

            if (is_bid) {
                if (cur_best == 0 || tick > cur_best) {
                    best_atom.store(tick, std::memory_order_release);
                }
            } else {
                if (cur_best == 0 || tick < cur_best) {
                    best_atom.store(tick, std::memory_order_release);
                }
            }
        } else {
            if (arr[slot].tick_tag != tick) {
                return true;  // stale delete — slot holds something else
            }
            arr[slot].tick_tag = 0;
            arr[slot].qty = 0.0;
            arr[slot].updated_sec = 0;
            pop[slot >> 6] &= ~(1ULL << (slot & 63));

            if (tick == cur_best) {
                uint32_t new_best =
                    is_bid ? find_next_lower_populated_bid(tick)
                           : find_next_higher_populated_ask(tick);
                best_atom.store(new_best, std::memory_order_release);
            }
        }
        return true;
    }

    // -----------------------------------------------------------------------
    // Bitmap scans (word-at-a-time). See the detailed comment block in
    // the earlier template version for the walk-semantics rationale —
    // the logic is unchanged, only the compile-time WINDOW_SIZE/MASK
    // became runtime window_size/mask.
    // -----------------------------------------------------------------------
    uint32_t find_next_lower_populated_bid(uint32_t from_tick) const noexcept {
        if (from_tick <= 1) return 0;
        int64_t stop = (from_tick > window_size)
                           ? static_cast<int64_t>(from_tick - window_size)
                           : 0;

        int64_t t = static_cast<int64_t>(from_tick) - 1;
        while (t > stop) {
            size_t slot = static_cast<size_t>(t) & mask;
            size_t word_idx = slot >> 6;
            int bit_idx = static_cast<int>(slot & 63);

            uint64_t mask_low = (bit_idx == 63)
                                    ? ~0ULL
                                    : ((1ULL << (bit_idx + 1)) - 1);
            uint64_t word = bids_pop[word_idx] & mask_low;

            if (word == 0) {
                t -= (bit_idx + 1);
                continue;
            }

            int top_bit = 63 - __builtin_clzll(word);
            int delta = bit_idx - top_bit;
            int64_t candidate_tick = t - delta;
            if (candidate_tick <= stop) return 0;

            uint32_t expected = static_cast<uint32_t>(candidate_tick);
            size_t candidate_slot = expected & mask;
            if (bids[candidate_slot].tick_tag == expected) {
                return expected;
            }

            bids_pop[word_idx] &= ~(1ULL << top_bit);
            t = candidate_tick - 1;
        }
        return 0;
    }

    uint32_t find_next_higher_populated_ask(uint32_t from_tick) const noexcept {
        if (from_tick == UINT32_MAX) return 0;
        int64_t stop = static_cast<int64_t>(from_tick) +
                       static_cast<int64_t>(window_size);
        if (stop > static_cast<int64_t>(UINT32_MAX)) {
            stop = static_cast<int64_t>(UINT32_MAX);
        }

        int64_t t = static_cast<int64_t>(from_tick) + 1;
        while (t < stop) {
            size_t slot = static_cast<size_t>(t) & mask;
            size_t word_idx = slot >> 6;
            int bit_idx = static_cast<int>(slot & 63);

            uint64_t mask_high = ~((1ULL << bit_idx) - 1);
            uint64_t word = asks_pop[word_idx] & mask_high;

            if (word == 0) {
                t += (64 - bit_idx);
                continue;
            }

            int bot_bit = __builtin_ctzll(word);
            int delta = bot_bit - bit_idx;
            int64_t candidate_tick = t + delta;
            if (candidate_tick >= stop) return 0;

            uint32_t expected = static_cast<uint32_t>(candidate_tick);
            size_t candidate_slot = expected & mask;
            if (asks[candidate_slot].tick_tag == expected) {
                return expected;
            }

            asks_pop[word_idx] &= ~(1ULL << bot_bit);
            t = candidate_tick + 1;
        }
        return 0;
    }
};

// Window sizes used by the engine. Not aliases for RingBufferBook anymore
// (it's no longer templated) — just constants for readability at call sites.
namespace RingWindow {
constexpr size_t kExchange = 19;    // 524,288 slots, ~16 MB virtual per book
constexpr size_t kPolymarket = 10;  // 1,024 slots,  ~32 KB virtual per book
}  // namespace RingWindow

}  // namespace Core
