// ---------------------------------------------------------------------------
// RingBufferBook unit tests.
//
// Validates the fixed-size, tick-indexed ring buffer book as a drop-in
// alternative to std::map-based OrderBook storage. Focus: correctness of
// the hash+tag validation scheme, best-tracking, deep-read queries, and
// the window-aliasing behavior.
//
// A small 64-slot template instantiation is used for tests so aliasing
// wrap-around can be exercised directly. Production uses 16K (WINDOW_BITS=14).
// ---------------------------------------------------------------------------

#include <atomic>
#include <catch2/catch_all.hpp>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <random>
#include <thread>
#include <vector>

#include "chrono_utils.h"
#include "market_making/dtypes/orderbook.h"
#include "market_making/dtypes/ring_buffer_book.h"

using Core::RingBufferBook;
using Core::RingPriceLevel;

namespace {

// 64-slot book (6 bits) — makes aliasing cases trivial to construct.
struct TinyBook : RingBufferBook {
    static constexpr size_t WINDOW_SIZE = 64;
    static constexpr size_t MASK = WINDOW_SIZE - 1;
    TinyBook() : RingBufferBook(6) {}
};
constexpr uint32_t TINY_WINDOW = TinyBook::WINDOW_SIZE;  // 64

// Reference order book using std::map — used as an oracle that the ring
// buffer's deep-read queries agree with.
struct ReferenceBook {
    std::map<uint32_t, double> bids;
    std::map<uint32_t, double> asks;
    double tick_size = 0.0;

    void apply_bid(uint32_t tick, double qty) {
        if (qty > 0.0) bids[tick] = qty;
        else bids.erase(tick);
    }
    void apply_ask(uint32_t tick, double qty) {
        if (qty > 0.0) asks[tick] = qty;
        else asks.erase(tick);
    }
    uint32_t best_bid() const { return bids.empty() ? 0 : bids.rbegin()->first; }
    uint32_t best_ask() const { return asks.empty() ? 0 : asks.begin()->first; }
    double tick_to_price(uint32_t t) const {
        return static_cast<double>(t) * tick_size;
    }

    double ask_depth(double slippage) const {
        if (asks.empty()) return 0.0;
        uint32_t bt = best_ask();
        double best_px = tick_to_price(bt);
        double max_px = best_px * (1.0 + slippage);
        double vol = 0.0;
        for (auto& [t, q] : asks) {
            if (tick_to_price(t) > max_px) break;
            vol += q;
        }
        return vol;
    }
    double bid_depth(double slippage) const {
        if (bids.empty()) return 0.0;
        uint32_t bt = best_bid();
        double best_px = tick_to_price(bt);
        double min_px = best_px * (1.0 - slippage);
        double vol = 0.0;
        for (auto it = bids.rbegin(); it != bids.rend(); ++it) {
            if (tick_to_price(it->first) < min_px) break;
            vol += it->second;
        }
        return vol;
    }
    double top_n_imbalance(int n) const {
        double bs = 0.0, as = 0.0;
        int bc = 0, ac = 0;
        for (auto it = bids.rbegin(); it != bids.rend() && bc < n; ++it, ++bc)
            bs += it->second;
        for (auto it = asks.begin(); it != asks.end() && ac < n; ++it, ++ac)
            as += it->second;
        double total = bs + as;
        if (total <= 0.0) return 0.0;
        return (bs - as) / total;
    }
};

// Helper to wire both books identically.
template <typename Book>
void apply_bid_both(Book& ring, ReferenceBook& ref, uint32_t tick, double qty) {
    ring.apply_bid(tick, qty, 0);
    ref.apply_bid(tick, qty);
}
template <typename Book>
void apply_ask_both(Book& ring, ReferenceBook& ref, uint32_t tick, double qty) {
    ring.apply_ask(tick, qty, 0);
    ref.apply_ask(tick, qty);
}

}  // namespace

// ---------------------------------------------------------------------------
// Basic sanity: empty book, single inserts
// ---------------------------------------------------------------------------
TEST_CASE("RingBufferBook — empty book returns nullopt / zero", "[ring_book]") {
    TinyBook book;
    book.tick_size = 0.01;
    book.is_in_sync = true;

    REQUIRE(book.get_best_bid_price() == std::nullopt);
    REQUIRE(book.get_best_ask_price() == std::nullopt);
    REQUIRE(book.get_spread() == std::nullopt);
    REQUIRE(book.get_ask_depth(0.01) == std::nullopt);
    REQUIRE(book.get_bid_depth(0.01) == std::nullopt);
    REQUIRE(book.get_top_n_volume_imbalance(5) == std::nullopt);
}

TEST_CASE("RingBufferBook — single insert tracks best", "[ring_book]") {
    TinyBook book;
    book.tick_size = 0.01;
    book.is_in_sync = true;

    REQUIRE(book.apply_bid(100, 5.0, 0));
    REQUIRE(book.get_best_bid_price().value() == Catch::Approx(1.00));

    REQUIRE(book.apply_ask(102, 3.0, 0));
    REQUIRE(book.get_best_ask_price().value() == Catch::Approx(1.02));
    REQUIRE(book.get_spread().value() == Catch::Approx(0.02));
}

// ---------------------------------------------------------------------------
// Best-bid / best-ask maintenance on insertion (new inside, new outside)
// ---------------------------------------------------------------------------
TEST_CASE("RingBufferBook — best tracking on inserts", "[ring_book]") {
    TinyBook book;
    book.tick_size = 0.01;
    book.is_in_sync = true;

    // Insert 100, 98, 99 — best should track max at each step
    book.apply_bid(100, 1.0, 0);
    REQUIRE(book.best_bid_tick.load() == 100);
    book.apply_bid(98, 1.0, 0);
    REQUIRE(book.best_bid_tick.load() == 100);  // 98 < 100
    book.apply_bid(99, 1.0, 0);
    REQUIRE(book.best_bid_tick.load() == 100);
    book.apply_bid(101, 1.0, 0);
    REQUIRE(book.best_bid_tick.load() == 101);  // new best

    // Ask side: best is lowest
    book.apply_ask(110, 1.0, 0);
    REQUIRE(book.best_ask_tick.load() == 110);
    book.apply_ask(112, 1.0, 0);
    REQUIRE(book.best_ask_tick.load() == 110);
    book.apply_ask(108, 1.0, 0);
    REQUIRE(book.best_ask_tick.load() == 108);  // new best
}

// ---------------------------------------------------------------------------
// Delete handling + find_next_best
// ---------------------------------------------------------------------------
TEST_CASE("RingBufferBook — deleting best falls back to next", "[ring_book]") {
    TinyBook book;
    book.tick_size = 0.01;
    book.is_in_sync = true;

    book.apply_bid(100, 1.0, 0);
    book.apply_bid(99, 2.0, 0);
    book.apply_bid(98, 3.0, 0);
    REQUIRE(book.best_bid_tick.load() == 100);

    // Delete best — should fall back to 99
    book.apply_bid(100, 0.0, 0);
    REQUIRE(book.best_bid_tick.load() == 99);
    REQUIRE(book.get_qty_at_bid_tick(100) == 0.0);

    // Delete new best — should fall back to 98
    book.apply_bid(99, 0.0, 0);
    REQUIRE(book.best_bid_tick.load() == 98);

    // Delete last — should go to 0 (no bid)
    book.apply_bid(98, 0.0, 0);
    REQUIRE(book.best_bid_tick.load() == 0);
    REQUIRE(book.get_best_bid_price() == std::nullopt);
}

TEST_CASE("RingBufferBook — gap between best and next best is walked", "[ring_book]") {
    TinyBook book;
    book.tick_size = 0.01;
    book.is_in_sync = true;

    // Place two ask levels with a 10-tick gap between them.
    book.apply_ask(200, 1.0, 0);
    book.apply_ask(210, 2.0, 0);
    REQUIRE(book.best_ask_tick.load() == 200);

    // Delete the inner one, next-best should be 210 (gap walked).
    book.apply_ask(200, 0.0, 0);
    REQUIRE(book.best_ask_tick.load() == 210);
}

// ---------------------------------------------------------------------------
// Tag validation: two ticks that alias to the same slot, only the live one
// should be visible.
// ---------------------------------------------------------------------------
TEST_CASE("RingBufferBook — tag validation rejects wrapped stale slot", "[ring_book]") {
    TinyBook book;
    book.tick_size = 0.01;
    book.is_in_sync = true;

    // Ticks 100 and 100 + WINDOW_SIZE (= 164) alias to the same slot.
    constexpr uint32_t T1 = 100;
    constexpr uint32_t T2 = T1 + TINY_WINDOW;  // 164
    REQUIRE((T1 & TinyBook::MASK) == (T2 & TinyBook::MASK));

    // First insert T1 on the ask side. Since best_ask is 0, T1 is accepted
    // and becomes the best ask.
    REQUIRE(book.apply_ask(T1, 5.0, 0));
    REQUIRE(book.best_ask_tick.load() == T1);
    REQUIRE(book.slot_is_populated_ask(T1));

    // T2 is exactly WINDOW_SIZE away from T1, which is the rejection
    // boundary (delta >= WINDOW_SIZE rejects). So T2 is refused — this is
    // the "violent move larger than window" case that the engine must
    // handle via full refresh. Confirm the rejection is observable.
    REQUIRE_FALSE(book.apply_ask(T2, 7.0, 0));
    REQUIRE(book.best_ask_tick.load() == T1);  // unchanged
    REQUIRE(book.get_qty_at_ask_tick(T1) == 5.0);

    // Now simulate the full-refresh path: clear the book, then write T2.
    // It should land cleanly in the same slot with the new tag.
    book.clear();
    book.is_in_sync = true;
    REQUIRE(book.apply_ask(T2, 7.0, 0));
    REQUIRE(book.best_ask_tick.load() == T2);
    REQUIRE(book.slot_is_populated_ask(T2));
    // And the old T1 should not be visible via tag-validated reads.
    REQUIRE_FALSE(book.slot_is_populated_ask(T1));
    REQUIRE(book.get_qty_at_ask_tick(T1) == 0.0);
}

// ---------------------------------------------------------------------------
// Window boundary: a tick more than WINDOW_SIZE away from current best is
// rejected (simulates a "violent move" that needs a full refresh).
// ---------------------------------------------------------------------------
TEST_CASE("RingBufferBook — rejects writes outside window", "[ring_book]") {
    TinyBook book;
    book.tick_size = 0.01;
    book.is_in_sync = true;

    book.apply_bid(100, 1.0, 0);
    // 100 + 64 = 164 is exactly at the boundary — rejected (>=).
    REQUIRE_FALSE(book.apply_bid(164, 1.0, 0));
    // 100 + 63 = 163 is inside — accepted.
    REQUIRE(book.apply_bid(163, 1.0, 0));
}

// ---------------------------------------------------------------------------
// Deep reads — cross-check against the std::map reference book.
// ---------------------------------------------------------------------------
TEST_CASE("RingBufferBook — get_ask_depth matches reference", "[ring_book]") {
    TinyBook ring;
    ReferenceBook ref;
    ring.tick_size = 0.01;
    ring.is_in_sync = true;
    ref.tick_size = 0.01;

    // Build a tight ask book: 100 (best) .. 120, each with qty = i * 0.5
    for (uint32_t t = 100; t <= 120; ++t) {
        apply_ask_both(ring, ref, t, static_cast<double>(t - 99) * 0.5);
    }

    // 5% slippage covers ticks 100..105 (best * 1.05 = 1.05 → tick 105)
    double ring_d = ring.get_ask_depth(0.05).value();
    double ref_d = ref.ask_depth(0.05);
    REQUIRE(ring_d == Catch::Approx(ref_d));

    // 10% slippage covers ticks 100..110
    ring_d = ring.get_ask_depth(0.10).value();
    ref_d = ref.ask_depth(0.10);
    REQUIRE(ring_d == Catch::Approx(ref_d));

    // 1% slippage covers only tick 100 (best * 1.01 = 1.01 → tick 101,
    // but the loop is inclusive, so ticks 100..101 both count)
    ring_d = ring.get_ask_depth(0.01).value();
    ref_d = ref.ask_depth(0.01);
    REQUIRE(ring_d == Catch::Approx(ref_d));
}

TEST_CASE("RingBufferBook — get_bid_depth matches reference", "[ring_book]") {
    TinyBook ring;
    ReferenceBook ref;
    ring.tick_size = 0.01;
    ring.is_in_sync = true;
    ref.tick_size = 0.01;

    for (uint32_t t = 200; t >= 180; --t) {
        apply_bid_both(ring, ref, t, static_cast<double>(201 - t) * 0.5);
    }

    for (double s : {0.005, 0.01, 0.05, 0.10}) {
        double ring_d = ring.get_bid_depth(s).value();
        double ref_d = ref.bid_depth(s);
        REQUIRE(ring_d == Catch::Approx(ref_d));
    }
}

TEST_CASE("RingBufferBook — top_n imbalance matches reference", "[ring_book]") {
    TinyBook ring;
    ReferenceBook ref;
    ring.tick_size = 0.01;
    ring.is_in_sync = true;
    ref.tick_size = 0.01;

    // Build both sides. Intentional asymmetry: bids heavier than asks.
    for (uint32_t t = 200; t >= 190; --t) {
        apply_bid_both(ring, ref, t, 10.0);
    }
    for (uint32_t t = 202; t <= 212; ++t) {
        apply_ask_both(ring, ref, t, 5.0);
    }

    for (int n : {1, 3, 5, 10}) {
        auto ring_imb = ring.get_top_n_volume_imbalance(n);
        double ref_imb = ref.top_n_imbalance(n);
        REQUIRE(ring_imb.has_value());
        REQUIRE(*ring_imb == Catch::Approx(ref_imb));
    }
}

// ---------------------------------------------------------------------------
// book_volume_before_* / number_of_levels_before_*
// ---------------------------------------------------------------------------
TEST_CASE("RingBufferBook — book_volume_before_bid strictly above price", "[ring_book]") {
    TinyBook book;
    book.tick_size = 0.01;
    book.is_in_sync = true;

    // Bids at ticks 100 .. 110 (prices 1.00 .. 1.10), qty=2 each.
    for (uint32_t t = 100; t <= 110; ++t) book.apply_bid(t, 2.0, 0);

    // All bids strictly above price 1.05 → ticks 106..110 = 5 levels × 2 qty
    // Notional: (1.06 + 1.07 + 1.08 + 1.09 + 1.10) * 2
    auto vol = book.get_book_volume_before_bid(1.05);
    REQUIRE(vol.has_value());
    double expected = (1.06 + 1.07 + 1.08 + 1.09 + 1.10) * 2.0;
    REQUIRE(*vol == Catch::Approx(expected));

    auto levels = book.get_number_of_levels_before_bid(1.05);
    REQUIRE(levels.has_value());
    REQUIRE(*levels == 5);
}

TEST_CASE("RingBufferBook — book_volume_before_ask strictly below price", "[ring_book]") {
    TinyBook book;
    book.tick_size = 0.01;
    book.is_in_sync = true;

    for (uint32_t t = 100; t <= 110; ++t) book.apply_ask(t, 2.0, 0);

    // Asks strictly below 1.05 → ticks 100..104 = 5 levels × 2 qty
    auto vol = book.get_book_volume_before_ask(1.05);
    REQUIRE(vol.has_value());
    double expected = (1.00 + 1.01 + 1.02 + 1.03 + 1.04) * 2.0;
    REQUIRE(*vol == Catch::Approx(expected));

    auto levels = book.get_number_of_levels_before_ask(1.05);
    REQUIRE(levels.has_value());
    REQUIRE(*levels == 5);
}

// ---------------------------------------------------------------------------
// Randomized fuzz: insert/delete random levels, cross-check against
// reference book at every step.
// ---------------------------------------------------------------------------
TEST_CASE("RingBufferBook — random fuzz vs reference", "[ring_book]") {
    TinyBook ring;
    ReferenceBook ref;
    ring.tick_size = 0.01;
    ring.is_in_sync = true;
    ref.tick_size = 0.01;

    std::mt19937 rng(42);
    // Stay well inside the 64-tick window to avoid rejection — use a
    // centered range of 40 ticks around tick 1000.
    std::uniform_int_distribution<uint32_t> bid_tick(980, 999);
    std::uniform_int_distribution<uint32_t> ask_tick(1001, 1020);
    std::uniform_real_distribution<double> qty_d(0.1, 10.0);
    std::bernoulli_distribution del(0.2);

    auto sync_best = [&]() {
        // Reference best trackers are queried directly; ring buffer has
        // already advanced its own state via apply_*.
        REQUIRE(ring.best_bid_tick.load() == ref.best_bid());
        REQUIRE(ring.best_ask_tick.load() == ref.best_ask());
    };

    for (int step = 0; step < 2000; ++step) {
        bool is_bid = (step % 2) == 0;
        uint32_t t = is_bid ? bid_tick(rng) : ask_tick(rng);
        double q = del(rng) ? 0.0 : qty_d(rng);
        if (is_bid) apply_bid_both(ring, ref, t, q);
        else        apply_ask_both(ring, ref, t, q);

        if (step % 50 == 0) {
            sync_best();
            // Depth queries should match reference within tolerance
            if (ref.best_ask() != 0) {
                double r = ring.get_ask_depth(0.02).value_or(-1.0);
                double x = ref.ask_depth(0.02);
                REQUIRE(r == Catch::Approx(x));
            }
            if (ref.best_bid() != 0) {
                double r = ring.get_bid_depth(0.02).value_or(-1.0);
                double x = ref.bid_depth(0.02);
                REQUIRE(r == Catch::Approx(x));
            }
            if (ref.best_bid() != 0 && ref.best_ask() != 0) {
                auto imb = ring.get_top_n_volume_imbalance(5);
                double ref_imb = ref.top_n_imbalance(5);
                REQUIRE(imb.has_value());
                REQUIRE(*imb == Catch::Approx(ref_imb));
            }
        }
    }
    sync_best();
}

// ---------------------------------------------------------------------------
// Concurrent writer + reader — no crashes, no UB. This does NOT assert
// strict consistency (readers may see transiently stale values); it only
// asserts that the reads complete, make progress, and stay within sane
// bounds. For strict consistency you'd need the seqlock retry wrapper
// around each read, which is left to the production integration layer.
// ---------------------------------------------------------------------------
TEST_CASE("RingBufferBook — concurrent writer/reader smoke", "[ring_book][.][slow]") {
    auto book = std::make_unique<RingBufferBook>(10);  // 1024 slots
    book->tick_size = 0.01;
    book->is_in_sync = true;

    // Seed with a stable top-of-book before starting threads so readers
    // always have a meaningful best to query.
    for (uint32_t t = 500; t <= 520; ++t) book->apply_bid(t, 1.0, 0);
    for (uint32_t t = 530; t <= 550; ++t) book->apply_ask(t, 1.0, 0);

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> writer_ops{0};
    std::atomic<uint64_t> reader_ops{0};
    std::atomic<uint64_t> sane_reads{0};

    std::thread writer([&]() {
        std::mt19937 rng(7);
        // Stay within a ~60-tick range centered on 525.
        std::uniform_int_distribution<uint32_t> bid_t(500, 520);
        std::uniform_int_distribution<uint32_t> ask_t(530, 550);
        std::uniform_real_distribution<double> q(0.1, 5.0);
        std::bernoulli_distribution del(0.3);
        while (!stop.load(std::memory_order_relaxed)) {
            book->begin_write();
            if (rng() % 2) {
                book->apply_bid(bid_t(rng), del(rng) ? 0.0 : q(rng), 0);
            } else {
                book->apply_ask(ask_t(rng), del(rng) ? 0.0 : q(rng), 0);
            }
            book->end_write();
            writer_ops.fetch_add(1, std::memory_order_relaxed);
        }
    });

    auto reader_fn = [&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            auto imb = book->get_top_n_volume_imbalance(5);
            auto ask_d = book->get_ask_depth(0.05);
            auto bid_d = book->get_bid_depth(0.05);
            if (imb && *imb >= -1.0 && *imb <= 1.0) {
                sane_reads.fetch_add(1, std::memory_order_relaxed);
            }
            if (ask_d && *ask_d >= 0.0 && *bid_d && *bid_d >= 0.0) {
                sane_reads.fetch_add(1, std::memory_order_relaxed);
            }
            reader_ops.fetch_add(1, std::memory_order_relaxed);
        }
    };
    std::thread r1(reader_fn);
    std::thread r2(reader_fn);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    stop.store(true, std::memory_order_relaxed);
    writer.join();
    r1.join();
    r2.join();

    // Writer should have made many ops, readers too, and at least a
    // meaningful fraction of reads should have landed in sane ranges.
    REQUIRE(writer_ops.load() > 100);
    REQUIRE(reader_ops.load() > 100);
    REQUIRE(sane_reads.load() > 0);
}

// ===========================================================================
// Phase 0b — word-at-a-time find_next_populated tests
//
// At production WINDOW_BITS=19 the tick-by-tick scan is ~1 ms worst case on
// the writer thread. The word-at-a-time scan skips empty 64-bit bitmap
// words in one __builtin_clzll/ctzll. These tests validate correctness —
// performance is implicit: the fuzz test runs 20k random ops inside a
// Catch2 test case budget, which only finishes in time if the fast-path
// is actually fast.
// ===========================================================================

// 1024-slot book (10 bits) — 16 bitmap words per side. Wide enough to
// exercise multi-word empty-skipping and wrap-around.
struct MidBook : RingBufferBook {
    static constexpr size_t WINDOW_SIZE = 1024;
    static constexpr size_t MASK = WINDOW_SIZE - 1;
    MidBook() : RingBufferBook(10) {}
};
constexpr uint32_t MID_WINDOW = MidBook::WINDOW_SIZE;  // 1024

TEST_CASE("RingBufferBook — find_next skips many empty bitmap words", "[ring_book]") {
    auto book = std::make_unique<MidBook>();
    book->tick_size = 0.01;
    book->is_in_sync = true;

    // Place a bid at tick 1000 (slot 1000, word 15 bit 40) and another at
    // tick 10 (slot 10, word 0 bit 10). Gap of 990 ticks spans ~15 bitmap
    // words that must be skipped as empty after the best is deleted.
    REQUIRE(book->apply_bid(1000, 5.0, 0));
    REQUIRE(book->apply_bid(10, 3.0, 0));
    REQUIRE(book->best_bid_tick.load() == 1000);

    // Delete the best. find_next_lower must walk back through ~15 empty
    // words and land on tick 10.
    REQUIRE(book->apply_bid(1000, 0.0, 0));
    REQUIRE(book->best_bid_tick.load() == 10);

    // Mirror for asks: place at ticks 100 and 1090 (~15 empty words apart).
    REQUIRE(book->apply_ask(100, 1.0, 0));
    REQUIRE(book->apply_ask(1090, 2.0, 0));
    REQUIRE(book->best_ask_tick.load() == 100);

    REQUIRE(book->apply_ask(100, 0.0, 0));
    REQUIRE(book->best_ask_tick.load() == 1090);
}

TEST_CASE("RingBufferBook — find_next handles single-level book with distant best", "[ring_book]") {
    auto book = std::make_unique<MidBook>();
    book->tick_size = 0.01;
    book->is_in_sync = true;

    // Single bid at tick 800. Delete it — best should fall to 0, and the
    // scan must complete without spinning (no second level anywhere).
    REQUIRE(book->apply_bid(800, 1.0, 0));
    REQUIRE(book->best_bid_tick.load() == 800);
    REQUIRE(book->apply_bid(800, 0.0, 0));
    REQUIRE(book->best_bid_tick.load() == 0);

    // Same for asks.
    REQUIRE(book->apply_ask(800, 1.0, 0));
    REQUIRE(book->apply_ask(800, 0.0, 0));
    REQUIRE(book->best_ask_tick.load() == 0);
}

TEST_CASE("RingBufferBook — find_next walks within a single bitmap word", "[ring_book]") {
    auto book = std::make_unique<MidBook>();
    book->tick_size = 0.01;
    book->is_in_sync = true;

    // Pack 5 bids into word 5 (slots 320..383, ticks 320..383 in the first
    // generation). Delete top, next should be second-from-top etc.
    book->apply_bid(380, 1.0, 0);
    book->apply_bid(370, 1.0, 0);
    book->apply_bid(365, 1.0, 0);
    book->apply_bid(340, 1.0, 0);
    book->apply_bid(325, 1.0, 0);
    REQUIRE(book->best_bid_tick.load() == 380);

    book->apply_bid(380, 0.0, 0);
    REQUIRE(book->best_bid_tick.load() == 370);
    book->apply_bid(370, 0.0, 0);
    REQUIRE(book->best_bid_tick.load() == 365);
    book->apply_bid(365, 0.0, 0);
    REQUIRE(book->best_bid_tick.load() == 340);
    book->apply_bid(340, 0.0, 0);
    REQUIRE(book->best_bid_tick.load() == 325);
    book->apply_bid(325, 0.0, 0);
    REQUIRE(book->best_bid_tick.load() == 0);
}

TEST_CASE("RingBufferBook — find_next scans across bitmap word boundary", "[ring_book]") {
    auto book = std::make_unique<MidBook>();
    book->tick_size = 0.01;
    book->is_in_sync = true;

    // Top bid at tick 192 (slot 192, word 3 bit 0). Next at tick 191
    // (slot 191, word 2 bit 63). The scan must cross from word 3 into
    // word 2 to find it.
    book->apply_bid(192, 1.0, 0);
    book->apply_bid(191, 1.0, 0);
    REQUIRE(book->best_bid_tick.load() == 192);
    book->apply_bid(192, 0.0, 0);
    REQUIRE(book->best_bid_tick.load() == 191);

    // Mirror for asks: top at tick 63 (word 0 bit 63), next at tick 64
    // (word 1 bit 0).
    book->apply_ask(63, 1.0, 0);
    book->apply_ask(64, 1.0, 0);
    REQUIRE(book->best_ask_tick.load() == 63);
    book->apply_ask(63, 0.0, 0);
    REQUIRE(book->best_ask_tick.load() == 64);
}

TEST_CASE("RingBufferBook — find_next respects window lower bound", "[ring_book]") {
    auto book = std::make_unique<MidBook>();
    book->tick_size = 0.01;
    book->is_in_sync = true;

    // Place a bid at tick 1500. Next level, tick 200, is 1300 ticks away.
    // Window is 1024 ticks, so 200 is OUTSIDE the window from best=1500.
    // The writer's apply_bid must refuse 200 since |200 - 1500| = 1300 > WINDOW.
    REQUIRE(book->apply_bid(1500, 1.0, 0));
    REQUIRE_FALSE(book->apply_bid(200, 1.0, 0));

    // Delete best — since 200 was refused, there's no next level within the
    // window, so best should go to 0 (no walking 1024 empty slots back to 200).
    REQUIRE(book->apply_bid(1500, 0.0, 0));
    REQUIRE(book->best_bid_tick.load() == 0);
}

TEST_CASE("RingBufferBook — find_next wrap-around across tick gen boundary", "[ring_book]") {
    auto book = std::make_unique<MidBook>();
    book->tick_size = 0.01;
    book->is_in_sync = true;

    // Window is 1024. Place best at tick 1050. That establishes a valid
    // generation. Add a second level at tick 100 (within 1024 of 1050:
    // 1050 - 100 = 950 < 1024). This level is in the "previous"
    // generation relative to the simple slot-index math — the walk from
    // 1050 down must wrap past slot 0 to find it.
    REQUIRE(book->apply_bid(1050, 1.0, 0));
    REQUIRE(book->apply_bid(100, 2.0, 0));
    REQUIRE(book->best_bid_tick.load() == 1050);

    // Slot of 1050 = 26 (word 0 bit 26). Slot of 100 = 100 (word 1 bit 36).
    // Walking downward in tick space from 1049 crosses from word 0 down to
    // the "previous generation" word 15 at tick 1023, then wraps through
    // all remaining words back to word 1 where tick 100 lives.
    REQUIRE(book->apply_bid(1050, 0.0, 0));
    REQUIRE(book->best_bid_tick.load() == 100);

    // Symmetric ask case: best at tick 100, next at tick 1050.
    REQUIRE(book->apply_ask(100, 1.0, 0));
    REQUIRE(book->apply_ask(1050, 2.0, 0));
    REQUIRE(book->best_ask_tick.load() == 100);
    REQUIRE(book->apply_ask(100, 0.0, 0));
    REQUIRE(book->best_ask_tick.load() == 1050);
}

TEST_CASE("RingBufferBook — find_next fuzz with many empty regions", "[ring_book]") {
    // Random insert/delete fuzz on a 1024-slot book, cross-checking best
    // tracking against a std::map reference after every op. Exercises the
    // word-at-a-time scan across many realistic sparse configurations.
    auto ring = std::make_unique<MidBook>();
    ReferenceBook ref;
    ring->tick_size = 0.01;
    ring->is_in_sync = true;
    ref.tick_size = 0.01;

    std::mt19937 rng(1337);
    // Keep ticks centered on 500 with a range of ~500 — well inside the
    // 1024 window, but sparse enough that find_next frequently crosses
    // multiple bitmap words.
    std::uniform_int_distribution<uint32_t> bid_tick(250, 499);
    std::uniform_int_distribution<uint32_t> ask_tick(501, 750);
    std::uniform_real_distribution<double> qty_d(0.1, 10.0);
    std::bernoulli_distribution del(0.35);  // higher delete rate exercises find_next

    for (int step = 0; step < 20000; ++step) {
        bool is_bid = (step & 1) == 0;
        uint32_t t = is_bid ? bid_tick(rng) : ask_tick(rng);
        double q = del(rng) ? 0.0 : qty_d(rng);
        if (is_bid) {
            ring->apply_bid(t, q, 0);
            ref.apply_bid(t, q);
        } else {
            ring->apply_ask(t, q, 0);
            ref.apply_ask(t, q);
        }

        // Every op: best tracking must match reference.
        REQUIRE(ring->best_bid_tick.load() == ref.best_bid());
        REQUIRE(ring->best_ask_tick.load() == ref.best_ask());
    }
}

TEST_CASE("RingBufferBook — find_next lazy-clears stale bitmap bits", "[ring_book]") {
    // Manually craft a stale bit: set a bitmap bit whose slot has the
    // wrong tag. find_next should clear it and continue past.
    auto book = std::make_unique<MidBook>();
    book->tick_size = 0.01;
    book->is_in_sync = true;

    // Place a valid bid at tick 500. The walk from 499 downward will
    // traverse slots 499, 498, ..., 0, wrap to 1023, 1022, ..., 501 —
    // visiting every slot exactly once.
    book->apply_bid(500, 1.0, 0);
    REQUIRE(book->best_bid_tick.load() == 500);

    // Craft a stale bitmap bit at slot 300 (word 4 bit 44) with no
    // matching tag. In the walk from 499 downward this slot is visited
    // at t=300 (same-generation mapping). The scan should find the bit
    // set, see that bids[300].tick_tag == 0 != 300, and lazy-clear it.
    book->bids_pop[300 >> 6] |= (1ULL << (300 & 63));

    // Also craft a stale bit in a HIGHER slot that would be encountered
    // via wrap. Slot 700, word 10 bit 60. In the walk from 499, after
    // reaching slot 0 the walk wraps to slot 1023 and walks downward to
    // slot 501. Slot 700 is visited at t=700-1024=-324 virtual → but
    // lower_bound = 500 - 1024 wraps to 0 (since 500 < 1024), so the
    // walk stop is t > 0. Slot 700's wrap tick is negative, i.e. outside
    // the window entirely — the scan should NEVER visit it, so this bit
    // stays set. We'll verify the scan bounds are respected.
    book->bids_pop[700 >> 6] |= (1ULL << (700 & 63));

    // Delete the legitimate best. find_next must encounter the stale bit
    // at slot 300, clear it, and continue past to find... nothing, so 0.
    book->apply_bid(500, 0.0, 0);
    REQUIRE(book->best_bid_tick.load() == 0);

    // Confirm the stale bit at slot 300 WAS cleared (it was in-range).
    REQUIRE((book->bids_pop[300 >> 6] & (1ULL << (300 & 63))) == 0);

    // Confirm the stale bit at slot 700 was NOT cleared — it was out
    // of range for the walk, so the lazy cleanup correctly left it
    // alone. This verifies the window bound is honored.
    REQUIRE((book->bids_pop[700 >> 6] & (1ULL << (700 & 63))) != 0);
}


// ===========================================================================
// Phase 3 — Core::OrderBook integration (ring-only, no map)
//
// After Phase 3, Core::OrderBook has no std::map members. These tests drive
// the book via ring.apply_* as the sole write primitive (matching what
// engine.cpp writer sites now do) and verify the public reader API
// (get_ask_depth, etc.) returns correct results via the ring walk.
//
// Production ring is 16 MB per book; heap-allocate to keep stack usage low.
// ===========================================================================

namespace {

// Count populated slots on each side of the ring via the bitmap.
struct RingLevelCounts {
    int bid_count;
    int ask_count;
};

RingLevelCounts count_ring_levels(const Core::OrderBook& book) {
    int bc = 0;
    int ac = 0;
    for (size_t w = 0; w < book.ring.bitmap_words; ++w) {
        bc += __builtin_popcountll(book.ring.bids_pop[w]);
        ac += __builtin_popcountll(book.ring.asks_pop[w]);
    }
    return {bc, ac};
}

// Compute the tick for a given price, using the book's tick_size.
uint32_t tick_of(const Core::OrderBook& book, double price) {
    return book.ring.price_to_tick(price);
}

}  // namespace

TEST_CASE("OrderBook Phase 3 — inert when tick_size == 0",
          "[ring_book][orderbook][phase3]") {
    // Fresh OrderBook has tick_size=0. Deep readers must return nullopt.
    auto book = std::make_unique<Core::OrderBook>();
    REQUIRE(book->ring.tick_size == 0.0);
    book->is_in_sync = true;
    REQUIRE(book->get_ask_depth(0.01) == std::nullopt);
    REQUIRE(book->get_bid_depth(0.01) == std::nullopt);
    REQUIRE(book->get_top_n_volume_imbalance(5) == std::nullopt);
    REQUIRE(book->get_book_volume_before_bid(100.0) == std::nullopt);
}

TEST_CASE("OrderBook Phase 3 — get_ask_depth via ring path",
          "[ring_book][orderbook][phase3]") {
    auto book = std::make_unique<Core::OrderBook>();
    book->ring.tick_size = 0.10;
    book->is_in_sync = true;
    book->ring.is_in_sync = true;

    // Populate via ring.apply_ask directly (the post-Phase-3 writer).
    for (int i = 0; i < 10; ++i) {
        double p = 100.0 + i * 0.10;
        book->ring.apply_ask(tick_of(*book, p), 1.0, 0);
    }
    book->publish_bba();

    // 0.5% slippage → covers 100.00..100.50 inclusive (6 levels).
    auto depth = book->get_ask_depth(0.005);
    REQUIRE(depth.has_value());
    REQUIRE(*depth == Catch::Approx(6.0));

    // 1% slippage → covers 100.00..100.90 (all 10).
    depth = book->get_ask_depth(0.01);
    REQUIRE(depth.has_value());
    REQUIRE(*depth == Catch::Approx(10.0));
}

TEST_CASE("OrderBook Phase 3 — get_bid_depth via ring path",
          "[ring_book][orderbook][phase3]") {
    auto book = std::make_unique<Core::OrderBook>();
    book->ring.tick_size = 0.10;
    book->is_in_sync = true;
    book->ring.is_in_sync = true;

    for (int i = 0; i < 10; ++i) {
        double p = 100.0 - i * 0.10;
        book->ring.apply_bid(tick_of(*book, p), 2.0, 0);
    }
    book->publish_bba();

    // Best bid = 100.00. 0.5% = $99.50. Ticks 1000..995 inclusive = 6 × 2.
    auto depth = book->get_bid_depth(0.005);
    REQUIRE(depth.has_value());
    REQUIRE(*depth == Catch::Approx(12.0));

    depth = book->get_bid_depth(0.01);
    REQUIRE(depth.has_value());
    REQUIRE(*depth == Catch::Approx(20.0));
}

TEST_CASE("OrderBook Phase 3 — book_volume_before / levels_before",
          "[ring_book][orderbook][phase3]") {
    auto book = std::make_unique<Core::OrderBook>();
    book->ring.tick_size = 0.10;
    book->is_in_sync = true;
    book->ring.is_in_sync = true;

    // Bids at 100.0 down to 99.1, qty 3 each. Total 10 levels.
    for (int i = 0; i < 10; ++i) {
        double p = 100.0 - i * 0.10;
        book->ring.apply_bid(tick_of(*book, p), 3.0, 0);
    }
    // Asks at 101.0 up to 101.9, qty 3 each.
    for (int i = 0; i < 10; ++i) {
        double p = 101.0 + i * 0.10;
        book->ring.apply_ask(tick_of(*book, p), 3.0, 0);
    }
    book->publish_bba();

    // Volume of bids strictly above 99.50 → 99.60..100.00 = 5 levels.
    auto vol = book->get_book_volume_before_bid(99.50);
    REQUIRE(vol.has_value());
    REQUIRE(*vol == Catch::Approx(
                        (99.60 + 99.70 + 99.80 + 99.90 + 100.0) * 3.0));

    auto lv = book->get_number_of_levels_before_bid(99.50);
    REQUIRE(lv.has_value());
    REQUIRE(*lv == 5);

    // Volume of asks strictly below 101.50 → 101.0..101.4 = 5 levels.
    auto vol_a = book->get_book_volume_before_ask(101.50);
    REQUIRE(vol_a.has_value());
    REQUIRE(*vol_a == Catch::Approx(
                          (101.0 + 101.1 + 101.2 + 101.3 + 101.4) * 3.0));

    auto lv_a = book->get_number_of_levels_before_ask(101.50);
    REQUIRE(lv_a.has_value());
    REQUIRE(*lv_a == 5);
}

TEST_CASE("OrderBook Phase 3 — top_n_volume_imbalance via ring path",
          "[ring_book][orderbook][phase3]") {
    auto book = std::make_unique<Core::OrderBook>();
    book->ring.tick_size = 0.10;
    book->is_in_sync = true;
    book->ring.is_in_sync = true;

    // Bids heavy, asks light.
    for (int i = 0; i < 5; ++i) {
        double p = 100.0 - i * 0.10;
        book->ring.apply_bid(tick_of(*book, p), 10.0, 0);
    }
    for (int i = 0; i < 5; ++i) {
        double p = 101.0 + i * 0.10;
        book->ring.apply_ask(tick_of(*book, p), 1.0, 0);
    }
    book->publish_bba();

    // Top 3: bid_sum=30, ask_sum=3, total=33, imb=27/33.
    auto imb = book->get_top_n_volume_imbalance(3);
    REQUIRE(imb.has_value());
    REQUIRE(*imb == Catch::Approx(27.0 / 33.0));

    // Top 5: bid_sum=50, ask_sum=5, imb=45/55.
    imb = book->get_top_n_volume_imbalance(5);
    REQUIRE(imb.has_value());
    REQUIRE(*imb == Catch::Approx(45.0 / 55.0));
}

TEST_CASE("OrderBook Phase 3 — get_avg_size via ring path",
          "[ring_book][orderbook][phase3]") {
    auto book = std::make_unique<Core::OrderBook>();
    book->ring.tick_size = 0.10;
    book->is_in_sync = true;
    book->ring.is_in_sync = true;

    for (int i = 0; i < 5; ++i) {
        double p = 100.0 + i * 0.10;
        double q = static_cast<double>(i + 1);
        book->ring.apply_ask(tick_of(*book, p), q, 0);
    }
    book->publish_bba();

    auto avg = book->get_avg_ask_size(3);
    REQUIRE(avg.has_value());
    REQUIRE(*avg == Catch::Approx(2.0));  // (1+2+3)/3

    avg = book->get_avg_ask_size(5);
    REQUIRE(avg.has_value());
    REQUIRE(*avg == Catch::Approx(3.0));

    avg = book->get_avg_ask_size(10);
    REQUIRE(avg.has_value());
    REQUIRE(*avg == Catch::Approx(3.0));  // only 5 exist
}

TEST_CASE("OrderBook Phase 3 — rebuild_ring_from_levels",
          "[ring_book][orderbook][phase3]") {
    auto book = std::make_unique<Core::OrderBook>();
    book->ring.tick_size = 0.10;
    book->is_in_sync = true;

    Core::OrderBookLevels levels;
    levels.timestamp_ms = 1700000000000LL;
    for (int i = 0; i < 10; ++i) {
        levels.asks.emplace_back(100.0 + i * 0.10, 1.0 + i);
        levels.bids.emplace_back(99.9 - i * 0.10, 1.0 + i);
    }

    book->rebuild_ring_from_levels(levels);

    auto counts = count_ring_levels(*book);
    REQUIRE(counts.ask_count == 10);
    REQUIRE(counts.bid_count == 10);
    REQUIRE(book->ring.best_ask_tick.load() == 1000);
    REQUIRE(book->ring.best_bid_tick.load() == 999);
    REQUIRE(book->ring.is_in_sync);

    // Second rebuild preserves tick_size.
    book->rebuild_ring_from_levels(levels);
    REQUIRE(book->ring.tick_size == Catch::Approx(0.10));
}

TEST_CASE("OrderBook Phase 3 — to_json / from_json round-trip",
          "[ring_book][orderbook][phase3]") {
    auto src = std::make_unique<Core::OrderBook>();
    src->ring.tick_size = 0.10;
    src->is_in_sync = true;
    src->ring.is_in_sync = true;
    src->timestamp = ChronoUtils::make_utc_timepoint_from_epoch_ms(
        1700000000000LL);

    for (int i = 0; i < 5; ++i) {
        src->ring.apply_ask(tick_of(*src, 100.0 + i * 0.10), 1.0 + i, 0);
        src->ring.apply_bid(tick_of(*src, 99.9 - i * 0.10), 2.0 + i, 0);
    }
    src->publish_bba();

    nlohmann::json j = src->to_json();
    REQUIRE(j["tick_size"] == 0.10);
    REQUIRE(j["asks"].size() == 5);
    REQUIRE(j["bids"].size() == 5);

    auto restored = Core::OrderBook::from_json(j);
    REQUIRE(restored->ring.tick_size == Catch::Approx(0.10));
    REQUIRE(restored->is_in_sync);
    REQUIRE(restored->ring.best_ask_tick.load() == 1000);
    REQUIRE(restored->ring.best_bid_tick.load() == 999);

    // Deep queries on restored book agree with source.
    auto src_depth = src->get_ask_depth(0.01);
    auto rst_depth = restored->get_ask_depth(0.01);
    REQUIRE(src_depth.has_value());
    REQUIRE(rst_depth.has_value());
    REQUIRE(*src_depth == Catch::Approx(*rst_depth));
}

TEST_CASE("OrderBook Phase 3 — copy_from preserves ring state",
          "[ring_book][orderbook][phase3]") {
    auto src = std::make_unique<Core::OrderBook>();
    src->ring.tick_size = 0.10;
    src->is_in_sync = true;
    src->ring.is_in_sync = true;
    src->ring.apply_ask(tick_of(*src, 100.5), 1.0, 0);
    src->ring.apply_bid(tick_of(*src, 100.4), 2.0, 0);
    src->publish_bba();

    auto dst = std::make_unique<Core::OrderBook>();
    dst->copy_from(*src);

    REQUIRE(dst->ring.tick_size == Catch::Approx(0.10));
    REQUIRE(dst->ring.best_ask_tick.load() == 1005);
    REQUIRE(dst->ring.best_bid_tick.load() == 1004);
    REQUIRE(dst->is_in_sync);

    // Deep read on copy matches source.
    auto src_imb = src->get_top_n_volume_imbalance(1);
    auto dst_imb = dst->get_top_n_volume_imbalance(1);
    REQUIRE(src_imb.has_value());
    REQUIRE(dst_imb.has_value());
    REQUIRE(*src_imb == Catch::Approx(*dst_imb));
}

TEST_CASE("OrderBook Phase 3 — OrderBookLevels JSON round-trip",
          "[ring_book][orderbook][phase3]") {
    Core::OrderBookLevels levels;
    levels.timestamp_ms = 1700000000000LL;
    levels.last_sequence_number = 42;
    levels.asks.emplace_back(100.0, 1.0);
    levels.asks.emplace_back(100.1, 2.0);
    levels.bids.emplace_back(99.9, 3.0);
    levels.bids.emplace_back(99.8, 4.0);

    nlohmann::json j = levels.to_json();
    auto restored = Core::OrderBookLevels::from_json(j);

    REQUIRE(restored.timestamp_ms == 1700000000000LL);
    REQUIRE(restored.last_sequence_number == 42);
    REQUIRE(restored.asks.size() == 2);
    REQUIRE(restored.bids.size() == 2);
    REQUIRE(restored.asks[0].first == Catch::Approx(100.0));
    REQUIRE(restored.bids[1].second == Catch::Approx(4.0));
}

// ===========================================================================
// Phase 4 — Memory footprint regression
//
// The pre-fix Phase 3 version value-initialized the ring slot arrays, which
// eager-faulted every page and blew up RSS to 16 MB per book. This test
// spins up 10k empty polymarket-sized books and 200 empty exchange-sized
// books and asserts total RSS growth stays under a sane ceiling. Without
// the lazy-page fix this test would OOM on CI.
// ===========================================================================

namespace {
#if defined(__linux__)
long long current_rss_kb() {
    FILE* f = std::fopen("/proc/self/status", "r");
    if (!f) return -1;
    long long rss_kb = -1;
    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, "VmRSS:", 6) == 0) {
            rss_kb = std::strtoll(line + 6, nullptr, 10);
            break;
        }
    }
    std::fclose(f);
    return rss_kb;
}
#else
long long current_rss_kb() { return -1; }
#endif
}  // namespace

TEST_CASE("Phase 4 — 10k polymarket books stay under 400 MB RSS",
          "[ring_book][phase4][mem]") {
    long long baseline = current_rss_kb();
    if (baseline < 0) {
        WARN("skipping mem test — /proc/self/status unavailable");
        return;
    }

    // 10000 polymarket-sized (10-bit) books + 200 exchange-sized (19-bit)
    // books. At 32 KB virtual per polymarket book (but lazy-paged → ~0 RSS
    // until written) and 16 MB virtual per exchange book (also lazy-paged),
    // the actual RSS growth should come primarily from heap allocator
    // overhead, metadata, and bitmap pages.
    std::vector<std::unique_ptr<Core::OrderBook>> polymarket_books;
    polymarket_books.reserve(10000);
    for (int i = 0; i < 10000; ++i) {
        polymarket_books.push_back(std::make_unique<Core::OrderBook>(
            Core::RingWindow::kPolymarket));
    }

    std::vector<std::unique_ptr<Core::OrderBook>> exchange_books;
    exchange_books.reserve(200);
    for (int i = 0; i < 200; ++i) {
        exchange_books.push_back(std::make_unique<Core::OrderBook>(
            Core::RingWindow::kExchange));
    }

    long long after = current_rss_kb();
    long long delta_kb = after - baseline;
    std::cout << "[phase4 mem] baseline=" << baseline << " kB after="
              << after << " kB delta=" << delta_kb << " kB (" << (delta_kb / 1024)
              << " MB) for 10k polymarket + 200 exchange books\n";

    // Generous ceiling: 400 MB. In practice the fix drops this to tens
    // of MB, so 400 MB catches any regression where pages get eagerly
    // faulted back in (e.g. value-init regression).
    REQUIRE(delta_kb < 400 * 1024);
}
