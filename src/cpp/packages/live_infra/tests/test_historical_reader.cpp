#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <catch2/catch_all.hpp>

#include "market_data/historical_reader.h"

using MarketData::HistoricalDataRequest;
using MarketData::HistoricalStreamType;

namespace {

// Resolve dev market_data_library/raw/trades/binance/swap/BTCUSDT path,
// or skip the test if the operator's local data isn't available.
// Production CI would have synthetic fixtures; here we lean on the
// operator's real data directory because parquet fixture-writing is
// not in MVP scope.
//
// Captured once at process start so subsequent tests' setenv calls
// (e.g., the missing-dataset test below) don't break this lookup.
const std::string& original_data_dir_root() {
    static const std::string val = []() {
        const char* env = std::getenv("DATA_DIR_ROOT");
        return env ? std::string(env) : std::string();
    }();
    return val;
}

bool real_btcusdt_dataset_available() {
    if (original_data_dir_root().empty()) return false;
    std::string p = original_data_dir_root() +
                    "/raw/trades/binance/swap/BTCUSDT/parquets";
    return std::filesystem::exists(p);
}

// Re-set DATA_DIR_ROOT to the original (process-start) value. Call from
// any test that wants real data after another test mutated the env.
void restore_data_dir_root() {
    if (!original_data_dir_root().empty()) {
        setenv("DATA_DIR_ROOT", original_data_dir_root().c_str(), 1);
    }
}

// Force static-init capture BEFORE any test runs. Without this, Catch2's
// random test order can let the missing-dataset test run first and
// mutate DATA_DIR_ROOT before original_data_dir_root() is first called,
// poisoning the captured value.
[[maybe_unused]] const std::string& kForceCaptureAtInit =
    original_data_dir_root();

}  // namespace

TEST_CASE("MarketData::dataset_path builds canonical layout",
          "[historical_reader]") {
    setenv("DATA_DIR_ROOT", "/tmp/fake_dir", 1);
    HistoricalDataRequest req{
        .exchange = "binance",
        .market_type = "swap",
        .symbol = "BTCUSDT",
        .type = HistoricalStreamType::AggrTrades,
        .start_ts_ms = 0,
        .end_ts_ms = 0,
    };
    REQUIRE(MarketData::dataset_path(req) ==
            "/tmp/fake_dir/raw/trades/binance/swap/BTCUSDT");
}

TEST_CASE("MarketData::read_historical throws on missing dataset",
          "[historical_reader]") {
    setenv("DATA_DIR_ROOT", "/tmp/definitely_does_not_exist_12345", 1);
    HistoricalDataRequest req{
        .exchange = "binance",
        .market_type = "swap",
        .symbol = "NOSUCHSYMBOL",
        .type = HistoricalStreamType::AggrTrades,
        .start_ts_ms = 0,
        .end_ts_ms = 0,
    };
    try {
        MarketData::read_historical(req);
        FAIL("expected std::runtime_error for missing dataset");
    } catch (const std::runtime_error& e) {
        // Error message must include the attempted path so the operator
        // can see WHERE the reader looked.
        std::string msg = e.what();
        REQUIRE(msg.find("/tmp/definitely_does_not_exist_12345") !=
                std::string::npos);
        REQUIRE(msg.find("NOSUCHSYMBOL") != std::string::npos);
    }
}

TEST_CASE("MarketData::read_historical happy path on real BTCUSDT data",
          "[historical_reader][requires_data]") {
    restore_data_dir_root();
    if (!real_btcusdt_dataset_available()) {
        SKIP("DATA_DIR_ROOT/raw/trades/binance/swap/BTCUSDT/parquets not "
             "present — skipping (set up fixtures or run on a machine "
             "with the operator's market_data_library)");
    }

    // Read 5 minutes of historical trades. The trades table grows fast
    // enough that any 5-minute window in 2025+ will have hundreds of
    // events; we just assert non-empty + sorted, not specific counts.
    constexpr long long kStartMs = 1735689600000LL;  // 2025-01-01 00:00 UTC
    constexpr long long kEndMs = kStartMs + 5 * 60 * 1000LL;  // +5 min

    HistoricalDataRequest req{
        .exchange = "binance",
        .market_type = "swap",
        .symbol = "BTCUSDT",
        .type = HistoricalStreamType::AggrTrades,
        .start_ts_ms = kStartMs,
        .end_ts_ms = kEndMs,
    };
    auto events = MarketData::read_historical(req);
    REQUIRE(events.size() > 0);

    // All events are TRADE variants with the right symbol.
    for (const auto& ev : events) {
        REQUIRE(ev.type == MarketData::MarketEvent::TRADE);
        REQUIRE(ev.trade.has_value());
        REQUIRE(ev.trade->symbol == "BTCUSDT");
        REQUIRE(ev.trade->exchange_name == "binance");
        REQUIRE(ev.trade->market_type == "swap");
        REQUIRE(ev.trade->price > 0.0);
        REQUIRE(ev.trade->qty > 0.0);
    }

    // Sorted by timestamp ascending (read_historical's contract).
    REQUIRE(std::is_sorted(events.begin(), events.end(),
                           [](const auto& a, const auto& b) {
                               return a.trade->timestamp_ms <
                                      b.trade->timestamp_ms;
                           }));

    // Bounds enforced: every timestamp inside [start_ts_ms, end_ts_ms).
    REQUIRE(events.front().trade->timestamp_ms >= kStartMs);
    REQUIRE(events.back().trade->timestamp_ms < kEndMs);
}

TEST_CASE("MarketData::read_historical empty range returns empty vector",
          "[historical_reader][requires_data]") {
    restore_data_dir_root();
    if (!real_btcusdt_dataset_available()) {
        SKIP("market_data_library not available");
    }
    // Far future — no data present.
    constexpr long long kFutureMs = 9999999999000LL;
    HistoricalDataRequest req{
        .exchange = "binance",
        .market_type = "swap",
        .symbol = "BTCUSDT",
        .type = HistoricalStreamType::AggrTrades,
        .start_ts_ms = kFutureMs,
        .end_ts_ms = kFutureMs + 60 * 1000LL,
    };
    auto events = MarketData::read_historical(req);
    REQUIRE(events.empty());
}
