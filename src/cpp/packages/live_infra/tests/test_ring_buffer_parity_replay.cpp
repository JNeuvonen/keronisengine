// ---------------------------------------------------------------------------
// Phase 3 parity replay test.
//
// Replays real Tardis incremental L2 events through a Core::OrderBook
// (which is now ring-only after Phase 3) and cross-checks against a LOCAL
// std::map<double, double> reference oracle held inside the test. For every
// row we mirror the update into both structures, and at each checkpoint we
// verify that ring queries (best_bid/best_ask, populated counts within the
// window) agree with the reference map for in-window prices. Out-of-window
// map levels are expected to be absent from the ring — we track them as
// an informational metric, not a divergence.
//
// This replaces the pre-Phase-3 "map-on-OrderBook vs ring-on-OrderBook"
// parity test. After Phase 3 the OrderBook has no map at all.
//
// Tagged `[.][slow]` so it's hidden by default. Run explicitly:
//
//   ./src/cpp/build-debug/build/live_infra_tests "[parity_replay]"
//
// If the parquet file is missing (e.g., on a machine without the
// market_data_library), the test WARN-skips rather than failing.
// ---------------------------------------------------------------------------

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <catch2/catch_all.hpp>
#include <parquet/arrow/reader.h>
#include <parquet/file_reader.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <string>

#include "market_making/dtypes/orderbook.h"

namespace {

const std::string kTardisBtcL2Path = [] {
    const char* root = std::getenv("DATA_DIR_ROOT");
    return std::string(root ? root : "") +
           "/flat_files/tardis/incremental_book_L2/binance/swap/BTCUSDT/"
           "year=2026/month=03/day=04/data.parquet";
}();

// Binance USDT-margined futures BTCUSDT uses $0.10 tick size.
constexpr double kBtcSwapTickSize = 0.10;

// Reference oracle: a local std::map built alongside the real ring-backed
// OrderBook. Entirely outside the OrderBook's data structures — this is
// just a plain ground-truth for the test.
struct ReferenceBook {
    std::map<double, double> asks;  // price → qty
    std::map<double, double> bids;
};

// Result of comparing ring-backed `book` against the reference oracle.
struct ParitySummary {
    bool best_bid_ok = true;
    bool best_ask_ok = true;
    bool bid_count_ok = true;
    bool ask_count_ok = true;

    // Ring state.
    uint32_t ring_best_bid_tick = 0;
    uint32_t ring_best_ask_tick = 0;
    int ring_bid_count = 0;
    int ring_ask_count = 0;

    // Reference state.
    double ref_best_bid = 0.0;
    double ref_best_ask = 0.0;
    int ref_bid_total = 0;
    int ref_ask_total = 0;
    int ref_bid_in_window = 0;   // in-window map bids (should match ring_bid_count)
    int ref_ask_in_window = 0;
    int ref_bid_out_of_window = 0;
    int ref_ask_out_of_window = 0;
};

ParitySummary check_parity(const Core::OrderBook& book,
                           const ReferenceBook& ref) {
    ParitySummary s;
    const double tol = 0.5 * book.ring.tick_size;
    const uint32_t kWindow = static_cast<uint32_t>(book.ring.window_size);

    for (size_t w = 0; w < book.ring.bitmap_words; ++w) {
        s.ring_bid_count += __builtin_popcountll(book.ring.bids_pop[w]);
        s.ring_ask_count += __builtin_popcountll(book.ring.asks_pop[w]);
    }
    s.ref_bid_total = static_cast<int>(ref.bids.size());
    s.ref_ask_total = static_cast<int>(ref.asks.size());

    s.ring_best_bid_tick =
        book.ring.best_bid_tick.load(std::memory_order_relaxed);
    s.ring_best_ask_tick =
        book.ring.best_ask_tick.load(std::memory_order_relaxed);

    // Best-price parity.
    if (ref.bids.empty()) {
        s.best_bid_ok = (s.ring_best_bid_tick == 0);
    } else {
        s.ref_best_bid = ref.bids.rbegin()->first;
        if (s.ring_best_bid_tick == 0) {
            s.best_bid_ok = false;
        } else {
            double ring_bid = static_cast<double>(s.ring_best_bid_tick) *
                              book.ring.tick_size;
            s.best_bid_ok = std::abs(ring_bid - s.ref_best_bid) < tol;
        }
    }
    if (ref.asks.empty()) {
        s.best_ask_ok = (s.ring_best_ask_tick == 0);
    } else {
        s.ref_best_ask = ref.asks.begin()->first;
        if (s.ring_best_ask_tick == 0) {
            s.best_ask_ok = false;
        } else {
            double ring_ask = static_cast<double>(s.ring_best_ask_tick) *
                              book.ring.tick_size;
            s.best_ask_ok = std::abs(ring_ask - s.ref_best_ask) < tol;
        }
    }

    // Window-aware count parity.
    uint32_t bid_best = s.ring_best_bid_tick;
    uint32_t ask_best = s.ring_best_ask_tick;
    for (const auto& [price, qty] : ref.bids) {
        uint32_t t = book.ring.price_to_tick(price);
        if (t == 0) {
            ++s.ref_bid_out_of_window;
            continue;
        }
        uint32_t delta = (bid_best == 0)
                             ? 0
                             : (t > bid_best ? t - bid_best : bid_best - t);
        if (bid_best != 0 && delta >= kWindow) {
            ++s.ref_bid_out_of_window;
        } else {
            ++s.ref_bid_in_window;
        }
    }
    for (const auto& [price, qty] : ref.asks) {
        uint32_t t = book.ring.price_to_tick(price);
        if (t == 0) {
            ++s.ref_ask_out_of_window;
            continue;
        }
        uint32_t delta = (ask_best == 0)
                             ? 0
                             : (t > ask_best ? t - ask_best : ask_best - t);
        if (ask_best != 0 && delta >= kWindow) {
            ++s.ref_ask_out_of_window;
        } else {
            ++s.ref_ask_in_window;
        }
    }

    s.bid_count_ok = (s.ring_bid_count == s.ref_bid_in_window);
    s.ask_count_ok = (s.ring_ask_count == s.ref_ask_in_window);
    return s;
}

}  // namespace

TEST_CASE("Phase 3 — parity replay against real Tardis L2 BTCUSDT perp",
          "[ring_book][parity_replay][.][slow]") {
    if (!std::filesystem::exists(kTardisBtcL2Path)) {
        WARN("Skipping parity replay test — Tardis L2 parquet not available at "
             << kTardisBtcL2Path);
        return;
    }

    auto infile_res = arrow::io::ReadableFile::Open(kTardisBtcL2Path);
    REQUIRE(infile_res.ok());
    auto infile = infile_res.ValueOrDie();

    auto reader_res =
        parquet::arrow::OpenFile(infile, arrow::default_memory_pool());
    REQUIRE(reader_res.ok());
    std::unique_ptr<parquet::arrow::FileReader> reader =
        std::move(reader_res.ValueOrDie());

    std::shared_ptr<arrow::Table> table;
    auto status = reader->ReadRowGroup(0, &table);
    REQUIRE(status.ok());
    REQUIRE(table != nullptr);
    REQUIRE(table->num_rows() > 1000);

    std::cout << "[parity_replay] Loaded " << table->num_rows()
              << " rows from first row group of " << kTardisBtcL2Path
              << "\n";

    auto ts_col = table->GetColumnByName("timestamp");
    auto snap_col = table->GetColumnByName("is_snapshot");
    auto side_col = table->GetColumnByName("side");
    auto price_col = table->GetColumnByName("price");
    auto amount_col = table->GetColumnByName("amount");
    REQUIRE(ts_col != nullptr);
    REQUIRE(snap_col != nullptr);
    REQUIRE(side_col != nullptr);
    REQUIRE(price_col != nullptr);
    REQUIRE(amount_col != nullptr);

    REQUIRE(side_col->type()->id() == arrow::Type::LARGE_STRING);

    const int64_t num_rows = table->num_rows();

    // Book + reference. The ring-backed OrderBook is the system under test;
    // the local std::map ReferenceBook is the oracle.
    auto book = std::make_unique<Core::OrderBook>();
    book->ring.tick_size = kBtcSwapTickSize;
    book->is_in_sync = true;
    book->ring.is_in_sync = true;
    ReferenceBook ref;

    int64_t rows_processed = 0;
    int64_t snapshot_rows = 0;
    int64_t incremental_rows = 0;
    int64_t checkpoints = 0;
    int64_t checkpoint_failures = 0;
    constexpr int64_t kCheckInterval = 4096;

    const int num_chunks = ts_col->num_chunks();
    REQUIRE(num_chunks == snap_col->num_chunks());
    REQUIRE(num_chunks == side_col->num_chunks());
    REQUIRE(num_chunks == price_col->num_chunks());
    REQUIRE(num_chunks == amount_col->num_chunks());

    for (int ci = 0; ci < num_chunks; ++ci) {
        auto ts_arr = std::static_pointer_cast<arrow::Int64Array>(
            ts_col->chunk(ci));
        auto snap_arr = std::static_pointer_cast<arrow::BooleanArray>(
            snap_col->chunk(ci));
        auto side_arr = std::static_pointer_cast<arrow::LargeStringArray>(
            side_col->chunk(ci));
        auto price_arr = std::static_pointer_cast<arrow::DoubleArray>(
            price_col->chunk(ci));
        auto amount_arr = std::static_pointer_cast<arrow::DoubleArray>(
            amount_col->chunk(ci));

        const int64_t chunk_len = ts_arr->length();

        for (int64_t i = 0; i < chunk_len; ++i) {
            bool is_snapshot = snap_arr->Value(i);
            double price = price_arr->Value(i);
            double amount = amount_arr->Value(i);
            auto side = side_arr->GetView(i);
            int64_t ts_us = ts_arr->Value(i);
            uint32_t updated_sec = static_cast<uint32_t>(ts_us / 1000 / 1000);

            const bool is_bid = (side == "bid");
            const bool is_ask = (side == "ask");
            if (!is_bid && !is_ask) continue;

            // Apply to reference map.
            auto& ref_side = is_bid ? ref.bids : ref.asks;
            if (amount == 0.0) {
                ref_side.erase(price);
            } else {
                ref_side[price] = amount;
            }

            // Apply to ring (the unit under test).
            uint32_t tick = book->ring.price_to_tick(price);
            if (tick != 0) {
                if (is_bid) {
                    (void)book->ring.apply_bid(tick, amount, updated_sec);
                } else {
                    (void)book->ring.apply_ask(tick, amount, updated_sec);
                }
            }

            if (is_snapshot) ++snapshot_rows;
            else ++incremental_rows;
            ++rows_processed;

            if (rows_processed % kCheckInterval == 0) {
                ++checkpoints;
                auto s = check_parity(*book, ref);
                if (!s.best_bid_ok || !s.best_ask_ok ||
                    !s.bid_count_ok || !s.ask_count_ok) {
                    ++checkpoint_failures;
                    std::cerr << "[parity_replay] divergence at row "
                              << rows_processed << ":\n"
                              << "  best_bid_ok=" << s.best_bid_ok
                              << "  ring_bid_tick=" << s.ring_best_bid_tick
                              << "  ref_best_bid=" << s.ref_best_bid << "\n"
                              << "  best_ask_ok=" << s.best_ask_ok
                              << "  ring_ask_tick=" << s.ring_best_ask_tick
                              << "  ref_best_ask=" << s.ref_best_ask << "\n"
                              << "  ring_bid_count=" << s.ring_bid_count
                              << "  ref_bid_in_window="
                              << s.ref_bid_in_window
                              << "  ref_bid_oow="
                              << s.ref_bid_out_of_window
                              << "  ref_bid_total=" << s.ref_bid_total
                              << "\n"
                              << "  ring_ask_count=" << s.ring_ask_count
                              << "  ref_ask_in_window="
                              << s.ref_ask_in_window
                              << "  ref_ask_oow="
                              << s.ref_ask_out_of_window
                              << "  ref_ask_total=" << s.ref_ask_total
                              << "\n";
                    REQUIRE(s.best_bid_ok);
                    REQUIRE(s.best_ask_ok);
                    REQUIRE(s.bid_count_ok);
                    REQUIRE(s.ask_count_ok);
                }
            }
        }
    }

    auto final_s = check_parity(*book, ref);
    REQUIRE(final_s.best_bid_ok);
    REQUIRE(final_s.best_ask_ok);
    REQUIRE(final_s.bid_count_ok);
    REQUIRE(final_s.ask_count_ok);

    std::cout << "[parity_replay] PASS\n"
              << "  rows_processed        = " << rows_processed << "\n"
              << "  snapshot_rows         = " << snapshot_rows << "\n"
              << "  incremental_rows      = " << incremental_rows << "\n"
              << "  checkpoints_run       = " << checkpoints << "\n"
              << "  checkpoint_failures   = " << checkpoint_failures << "\n"
              << "  ref bids total        = " << final_s.ref_bid_total
              << "  (in-window " << final_s.ref_bid_in_window
              << ", oow " << final_s.ref_bid_out_of_window << ")\n"
              << "  ref asks total        = " << final_s.ref_ask_total
              << "  (in-window " << final_s.ref_ask_in_window
              << ", oow " << final_s.ref_ask_out_of_window << ")\n"
              << "  ring best bid/ask     = "
              << (final_s.ring_best_bid_tick != 0
                      ? static_cast<double>(final_s.ring_best_bid_tick) *
                            book->ring.tick_size
                      : 0.0)
              << " / "
              << (final_s.ring_best_ask_tick != 0
                      ? static_cast<double>(final_s.ring_best_ask_tick) *
                            book->ring.tick_size
                      : 0.0)
              << "\n";

    // Sanity: most of the map must be in-window. Out-of-window trailing
    // deep levels are expected but should be a small fraction.
    REQUIRE(final_s.ref_bid_in_window > 10 * final_s.ref_bid_out_of_window);
    REQUIRE(final_s.ref_ask_in_window > 10 * final_s.ref_ask_out_of_window);
}
