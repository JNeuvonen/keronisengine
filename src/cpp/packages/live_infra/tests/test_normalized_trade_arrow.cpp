#include <memory>
#include <string>
#include <vector>

#include <arrow/api.h>
#include <catch2/catch_test_macros.hpp>

#include "market_data/normalized_trade.h"

namespace {

std::shared_ptr<arrow::Array> double_array(const std::vector<double>& values) {
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

std::shared_ptr<arrow::Array> id_array(const std::vector<std::string>& values) {
    arrow::LargeStringBuilder builder;
    for (const std::string& value : values) REQUIRE(builder.Append(value).ok());
    std::shared_ptr<arrow::Array> array;
    REQUIRE(builder.Finish(&array).ok());
    return array;
}

std::shared_ptr<arrow::Array> buyer_maker_array(
    const std::vector<bool>& values) {
    arrow::BooleanBuilder builder;
    for (bool value : values) REQUIRE(builder.Append(value).ok());
    std::shared_ptr<arrow::Array> array;
    REQUIRE(builder.Finish(&array).ok());
    return array;
}

std::shared_ptr<arrow::Table> trade_table(bool include_buyer_maker,
                                          const std::vector<bool>& buyer_maker) {
    arrow::FieldVector fields = {
        arrow::field("price", arrow::float64()),
        arrow::field("qty", arrow::float64()),
        arrow::field("timestamp", arrow::int64()),
        arrow::field("id", arrow::large_utf8()),
    };
    arrow::ArrayVector columns = {
        double_array({309.6, 310.1}),
        double_array({1.0, 2.0}),
        int64_array({1'785'744'000'054, 1'785'744'000'056}),
        id_array({"AAPL:8:0:1", "AAPL:8:0:2"}),
    };
    if (include_buyer_maker) {
        fields.push_back(arrow::field("is_buyer_maker", arrow::boolean()));
        columns.push_back(buyer_maker_array(buyer_maker));
    }
    return arrow::Table::Make(arrow::schema(fields), columns);
}

}  // namespace

TEST_CASE("Trade arrow conversion tolerates a missing aggressor side",
          "[normalized_trade_arrow]") {
    const auto trades = MarketData::NormalizedParsedTrade::from_arrow_table(
        trade_table(false, {}), "AAPL", "polygon", "stock");

    REQUIRE(trades.size() == 2);
    REQUIRE(trades[0].price == 309.6);
    REQUIRE(trades[0].quantity == 1.0);
    REQUIRE(trades[0].timestamp == 1'785'744'000'054);
    REQUIRE(trades[0].trade_id == "AAPL:8:0:1");
    REQUIRE(trades[0].is_buyer_maker);
    REQUIRE(trades[1].price == 310.1);
    REQUIRE(trades[1].quantity == 2.0);
    REQUIRE(trades[1].timestamp == 1'785'744'000'056);
    REQUIRE(trades[1].trade_id == "AAPL:8:0:2");
    REQUIRE(trades[1].is_buyer_maker);
}

TEST_CASE("Trade arrow conversion reads is_buyer_maker when present",
          "[normalized_trade_arrow]") {
    const auto trades = MarketData::NormalizedParsedTrade::from_arrow_table(
        trade_table(true, {false, true}), "AAPL", "polygon", "stock");

    REQUIRE(trades.size() == 2);
    REQUIRE_FALSE(trades[0].is_buyer_maker);
    REQUIRE(trades[1].is_buyer_maker);
    REQUIRE(trades[0].trade_id == "AAPL:8:0:1");
    REQUIRE(trades[1].trade_id == "AAPL:8:0:2");
}
