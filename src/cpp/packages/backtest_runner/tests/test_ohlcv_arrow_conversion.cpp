#include <memory>
#include <vector>

#include <arrow/api.h>
#include <catch2/catch_test_macros.hpp>

#include "ohlcv_dataset_tick.h"

namespace {

std::shared_ptr<arrow::Array> timestamp_array(
    const std::vector<long long>& values) {
    arrow::TimestampBuilder builder(arrow::timestamp(arrow::TimeUnit::MILLI),
                                    arrow::default_memory_pool());
    for (long long value : values) REQUIRE(builder.Append(value).ok());
    std::shared_ptr<arrow::Array> array;
    REQUIRE(builder.Finish(&array).ok());
    return array;
}

std::shared_ptr<arrow::Array> double_array(
    const std::vector<double>& values) {
    arrow::DoubleBuilder builder;
    for (double value : values) REQUIRE(builder.Append(value).ok());
    std::shared_ptr<arrow::Array> array;
    REQUIRE(builder.Finish(&array).ok());
    return array;
}

std::shared_ptr<arrow::Array> int64_array(
    const std::vector<long long>& values) {
    arrow::Int64Builder builder;
    for (long long value : values) REQUIRE(builder.Append(value).ok());
    std::shared_ptr<arrow::Array> array;
    REQUIRE(builder.Finish(&array).ok());
    return array;
}

std::shared_ptr<arrow::ChunkedArray> chunks(
    std::shared_ptr<arrow::Array> first,
    std::shared_ptr<arrow::Array> second) {
    return std::make_shared<arrow::ChunkedArray>(
        arrow::ArrayVector{std::move(first), std::move(second)});
}

}  // namespace

TEST_CASE("OHLCV Arrow conversion reads every table chunk",
          "[ohlcv_arrow_conversion]") {
    const auto timestamps = chunks(timestamp_array({1'000, 2'000}),
                                   timestamp_array({3'000, 4'000}));
    const auto close_timestamps = chunks(
        timestamp_array({1'999, 2'999}), timestamp_array({3'999, 4'999}));
    const auto prices = chunks(double_array({10.0, 20.0}),
                               double_array({30.0, 40.0}));
    const auto volumes = chunks(double_array({100.0, 200.0}),
                                double_array({300.0, 400.0}));
    const auto trades = chunks(int64_array({1, 2}), int64_array({3, 4}));
    const auto schema = arrow::schema({
        arrow::field("kline_open_time_ms",
                     arrow::timestamp(arrow::TimeUnit::MILLI)),
        arrow::field("close_price", arrow::float64()),
        arrow::field("open_price", arrow::float64()),
        arrow::field("high_price", arrow::float64()),
        arrow::field("low_price", arrow::float64()),
        arrow::field("volume", arrow::float64()),
        arrow::field("kline_close_time_ms",
                     arrow::timestamp(arrow::TimeUnit::MILLI)),
        arrow::field("quote_asset_volume", arrow::float64()),
        arrow::field("number_of_trades", arrow::int64()),
        arrow::field("taker_buy_base_volume", arrow::float64()),
        arrow::field("taker_buy_quote_volume", arrow::float64()),
    });
    const auto table = arrow::Table::Make(
        schema,
        {timestamps, prices, prices, prices, prices, volumes, close_timestamps,
         volumes, trades, volumes, volumes});

    const std::vector<MarketData::OhlcvDatasetTick> converted =
        MarketData::OhlcvDatasetTick::from_arrow_table(table);

    REQUIRE(converted.size() == 4);
    REQUIRE(converted[0].kline_open_time == 1'000);
    REQUIRE(converted[1].close_price == 20.0);
    REQUIRE(converted[2].kline_open_time == 3'000);
    REQUIRE(converted[3].kline_close_time == 4'999);
    REQUIRE(converted[3].number_of_trades == 4);
}
