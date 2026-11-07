#include <catch2/catch_all.hpp>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>

#include "market_data/bar_types_constant.h"
#include "market_data/ohlcv_bar_meta.h"
#include "market_data/ohlcv_dataset.h"
#include "ohlcv_dataset_tick.h"

TEST_CASE("rescale_for_split adjusts only bars before the cutoff",
          "[split_rescale]") {
    const char* data_root = std::getenv("DATA_DIR_ROOT");
    if (data_root == nullptr) {
        SKIP("DATA_DIR_ROOT not set");
    }
    MarketData::OhlcvBarMeta meta;
    meta.symbol = "SPLIT_STUB_" + std::to_string(::getpid());
    meta.exchange_name = "polygon";
    meta.market_type = "stock";
    meta.bar_close_policy = MarketData::ONE_MIN_EXCHANGE_BAR;
    MarketData::OhlcvDataset dataset(meta, std::chrono::system_clock::now(),
                                     false, false);

    // 2023-11-15 10:00 ET: past, inside regular hours (stock datasets drop
    // bars outside them).
    const long long cutoff = 1'700'060'400'000LL;
    auto bar = [](long long open_ms, double close, double volume) {
        MarketData::OhlcvDatasetTick tick;
        tick.kline_open_time = open_ms;
        tick.kline_close_time = open_ms + 60'000;
        tick.open_price = tick.high_price = tick.low_price = close;
        tick.close_price = close;
        tick.volume = volume;
        tick.quote_asset_volume = close * volume;
        return tick;
    };
    dataset.add_new_kline(bar(cutoff - 120'000, 100.0, 10.0), true);
    dataset.add_new_kline(bar(cutoff - 60'000, 102.0, 20.0), true);
    dataset.add_new_kline(bar(cutoff, 51.0, 40.0), true);

    dataset.rescale_for_split(cutoff, 2.0);

    REQUIRE(dataset.klines.size() == 3);
    CHECK(dataset.klines[0].close_price == 50.0);
    CHECK(dataset.klines[1].close_price == 51.0);
    CHECK(dataset.klines[2].close_price == 51.0);
    CHECK(dataset.klines[0].volume == 20.0);
    CHECK(dataset.klines[2].volume == 40.0);
    CHECK(dataset.klines[0].quote_asset_volume == 1000.0);
    const auto closes = dataset.get_close_prices();
    REQUIRE(closes.size() == 3);
    CHECK(closes[0] == 50.0);
    CHECK(closes[2] == 51.0);

    std::filesystem::remove_all(
        std::string(data_root) +
        "/derived/bars/polygon/stock/exchange/60000_ms/" + meta.symbol);
}
