#include "ohlcv_dataset_tick.h"

#include <arrow/array.h>    // Required for all array types
#include <arrow/builder.h>  // Optional, for builders
#include <arrow/io/api.h>
#include <arrow/memory_pool.h>
#include <arrow/table.h>
#include <arrow/table.h>  // For arrow::Table
#include <arrow/type.h>   // Required for Arrow types like DoubleType
#include <parquet/arrow/reader.h>

#include <stdexcept>

#include "datetime_utils.h"
#include "market_data/bar_close_policy.h"

nlohmann::json MarketData::OhlcvDatasetTick::to_json() const {
    return {{"kline_open_time", kline_open_time},
            {"close_price", close_price},
            {"open_price", open_price},
            {"high_price", high_price},
            {"low_price", low_price},
            {"volume", volume},
            {"kline_close_time", kline_close_time},
            {"quote_asset_volume", quote_asset_volume},
            {"number_of_trades", number_of_trades},
            {"taker_buy_base_volume", taker_buy_base_volume},
            {"taker_buy_quote_volume", taker_buy_quote_volume}};
}

std::vector<MarketData::OhlcvDatasetTick>
MarketData::OhlcvDatasetTick::from_arrow_table(
    const std::shared_ptr<arrow::Table>& table) {
    if (!table) return {};

    std::vector<OhlcvDatasetTick> ticks;
    ticks.reserve(table->num_rows());

    arrow::TableBatchReader reader(*table);
    while (true) {
        std::shared_ptr<arrow::RecordBatch> batch;
        const arrow::Status status = reader.ReadNext(&batch);
        if (!status.ok()) {
            throw std::runtime_error("Failed to read OHLCV Arrow batch: " +
                                     status.ToString());
        }
        if (batch == nullptr) break;

        auto require_column = [&](const std::string& name) {
            const std::shared_ptr<arrow::Array> column =
                batch->GetColumnByName(name);
            if (column == nullptr) {
                throw std::runtime_error("Missing OHLCV Arrow column: " +
                                         name);
            }
            return column;
        };
        auto timestamp_value = [](const std::shared_ptr<arrow::Array>& column,
                                  int64_t index) {
            if (column->type_id() == arrow::Type::TIMESTAMP) {
                return std::static_pointer_cast<arrow::TimestampArray>(column)
                    ->Value(index);
            }
            if (column->type_id() == arrow::Type::INT64) {
                return std::static_pointer_cast<arrow::Int64Array>(column)
                    ->Value(index);
            }
            throw std::runtime_error("OHLCV timestamp column has type " +
                                     column->type()->ToString());
        };
        auto get_f64 = [&](const std::string& name) {
            const std::shared_ptr<arrow::Array> column = require_column(name);
            if (column->type_id() != arrow::Type::DOUBLE) {
                throw std::runtime_error("OHLCV column " + name +
                                         " has type " +
                                         column->type()->ToString());
            }
            return std::static_pointer_cast<arrow::DoubleArray>(column);
        };

        const std::shared_ptr<arrow::Array> open_time =
            require_column("kline_open_time_ms");
        const std::shared_ptr<arrow::Array> close_time =
            require_column("kline_close_time_ms");
        const auto open = get_f64("open_price");
        const auto close = get_f64("close_price");
        const auto high = get_f64("high_price");
        const auto low = get_f64("low_price");
        const auto volume = get_f64("volume");
        const auto quote_volume = get_f64("quote_asset_volume");
        const std::shared_ptr<arrow::Array> trade_count_column =
            require_column("number_of_trades");
        if (trade_count_column->type_id() != arrow::Type::INT64) {
            throw std::runtime_error(
                "OHLCV column number_of_trades has type " +
                trade_count_column->type()->ToString());
        }
        const auto trade_count =
            std::static_pointer_cast<arrow::Int64Array>(trade_count_column);
        // Older writers (and data_library.binance_swap_bars) name these
        // taker_buy_*_asset_volume; both schemas live in the library.
        auto get_f64_or_legacy = [&](const std::string& name,
                                     const std::string& legacy_name) {
            return batch->GetColumnByName(name) != nullptr
                       ? get_f64(name)
                       : get_f64(legacy_name);
        };
        const auto taker_base_volume = get_f64_or_legacy(
            "taker_buy_base_volume", "taker_buy_base_asset_volume");
        const auto taker_quote_volume = get_f64_or_legacy(
            "taker_buy_quote_volume", "taker_buy_quote_asset_volume");

        for (int64_t index = 0; index < batch->num_rows(); ++index) {
            ticks.emplace_back(
                timestamp_value(open_time, index), close->Value(index),
                open->Value(index), high->Value(index), low->Value(index),
                volume->Value(index), timestamp_value(close_time, index),
                quote_volume->Value(index), trade_count->Value(index),
                taker_base_volume->Value(index),
                taker_quote_volume->Value(index), true);
        }
    }

    return ticks;
}

bool MarketData::OhlcvDatasetTick::is_closed_tick(
    const MarketData::BarClosePolicy& bar_close_policy,
    bool is_live_synced, long long buffer_ms) const {
    if (!is_live_synced) {
        return is_closed;
    }

    if (bar_close_policy.is_time_bar) {
        return is_closed || DatetimeUtils::get_current_time_ms() >=
                                kline_close_time + buffer_ms;
    }
    return is_closed;
}

std::vector<MarketData::OhlcvDatasetTick>
MarketData::OhlcvDatasetTick::from_cpp_proxy_res(
    const nlohmann::json& json, const std::string& exchange_name,
    const std::string& market_type, long long interval_ms) {
    if (!json.contains("data")) {
        return {};
    }

    std::vector<OhlcvDatasetTick> ticks;
    long long now_ms = DatetimeUtils::get_current_time_ms();
    for (const auto& tick : json.at("data")) {
        OhlcvDatasetTick ohlcv_tick;

        ohlcv_tick.kline_open_time = std::stoll(tick[0].get<std::string>());
        ohlcv_tick.close_price = std::stod(tick[4].get<std::string>());
        ohlcv_tick.open_price = std::stod(tick[1].get<std::string>());
        ohlcv_tick.high_price = std::stod(tick[2].get<std::string>());
        ohlcv_tick.low_price = std::stod(tick[3].get<std::string>());
        ohlcv_tick.volume = std::stod(tick[5].get<std::string>());
        ohlcv_tick.kline_close_time = ohlcv_tick.kline_open_time + interval_ms;
        ohlcv_tick.quote_asset_volume = std::stod(tick[7].get<std::string>());
        ohlcv_tick.number_of_trades = std::stol(tick[8].get<std::string>());
        ohlcv_tick.taker_buy_base_volume =
            std::stod(tick[9].get<std::string>());
        ohlcv_tick.taker_buy_quote_volume =
            std::stod(tick[10].get<std::string>());
        ohlcv_tick.is_closed = true;

        if (ohlcv_tick.kline_close_time > now_ms) {
            continue;
        }

        ticks.push_back(ohlcv_tick);
    }
    return ticks;
}

MarketData::OhlcvDatasetTick MarketData::OhlcvDatasetTick::from_simdjson(
    const simdjson::dom::element& data) {
    OhlcvDatasetTick ohlcv_tick;

    auto k = data["k"];

    ohlcv_tick.kline_open_time = k["t"].get_uint64().value();
    ohlcv_tick.close_price =
        std::stod(std::string(k["c"].get_string().value()));
    ohlcv_tick.open_price = std::stod(std::string(k["o"].get_string().value()));
    ohlcv_tick.high_price = std::stod(std::string(k["h"].get_string().value()));
    ohlcv_tick.low_price = std::stod(std::string(k["l"].get_string().value()));
    ohlcv_tick.volume = std::stod(std::string(k["v"].get_string().value()));
    ohlcv_tick.kline_close_time = k["T"].get_uint64().value();

    if (ohlcv_tick.kline_close_time % 2 != 0) {
        ohlcv_tick.kline_close_time += 1;
    }

    ohlcv_tick.quote_asset_volume =
        std::stod(std::string(k["q"].get_string().value()));
    ohlcv_tick.number_of_trades = k["n"].get_uint64().value();
    ohlcv_tick.taker_buy_base_volume =
        std::stod(std::string(k["V"].get_string().value()));
    ohlcv_tick.taker_buy_quote_volume =
        std::stod(std::string(k["Q"].get_string().value()));
    ohlcv_tick.is_closed = k["x"].get_bool().value();

    return ohlcv_tick;
}
