#include "market_data/normalized_trade.h"

#include <arrow/array.h>    // Required for all array types
#include <arrow/builder.h>  // Optional, for builders
#include <arrow/compute/api.h>
#include <arrow/io/api.h>
#include <arrow/memory_pool.h>
#include <arrow/table.h>
#include <arrow/table.h>  // For arrow::Table
#include <arrow/type.h>   // Required for Arrow types like DoubleType
#include <parquet/arrow/reader.h>

#include <filesystem>
#include <iostream>
#include <regex>

#include "market_data/aggregate_trade.h"
#include "market_making/utils.h"

namespace fs = std::filesystem;

namespace MarketData {

NormalizedParsedTrade NormalizedParsedTrade::from_json(
    const nlohmann::json& json, const std::string& exchange_name,
    const std::string& market_type) {
    NormalizedParsedTrade ret;
    ret.exchange_name = exchange_name;
    ret.market_type = market_type;
    if (exchange_name == "binance") {
        if (market_type == "spot") {
            const auto& data = json["data"];
            ret.symbol = data["s"];
            ret.price = std::stod(data["p"].get<std::string>());
            ret.quantity = std::stod(data["q"].get<std::string>());
            ret.timestamp = data["E"];
            ret.is_buyer_maker = data["m"];
            ret.trade_id = std::to_string(data["t"].get<long long>());
        } else if (market_type == "swap") {
            const auto& data = json["data"];
            ret.symbol = data["s"].get<std::string>();
            ret.price = std::stod(data["p"].get<std::string>());
            ret.quantity = std::stod(data["q"].get<std::string>());
            ret.timestamp = data["E"].get<long long>();
            ret.is_buyer_maker = data["m"].get<bool>();
            ret.trade_id = std::to_string(data["t"].get<long long>());
        }
    } else if (exchange_name == "bybit") {
    }

    ret.parsed_timestamp =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();

    return ret;
}

NormalizedParsedTrade NormalizedParsedTrade::from_simdjson(
    const simdjson::dom::element& data, const std::string& exchange_name,
    const std::string& market_type) {
    NormalizedParsedTrade ret;
    ret.exchange_name = exchange_name;
    ret.market_type = market_type;

    if (exchange_name == "binance") {
        if (market_type == "spot") {
            ret.symbol = std::string(data["s"].get_string().value());
            ret.price = std::stod(std::string(data["p"].get_string().value()));
            ret.quantity =
                std::stod(std::string(data["q"].get_string().value()));
            ret.timestamp = data["E"].get_uint64().value();
            ret.is_buyer_maker = data["m"].get_bool().value();
            ret.trade_id = std::to_string(data["t"].get_uint64().value());
        } else if (market_type == "swap") {
            ret.symbol = std::string(data["s"].get_string().value());
            ret.price = std::stod(std::string(data["p"].get_string().value()));
            ret.quantity =
                std::stod(std::string(data["q"].get_string().value()));
            ret.timestamp = data["E"].get_uint64().value();
            ret.is_buyer_maker = data["m"].get_bool().value();
            ret.trade_id = std::to_string(data["t"].get_uint64().value());
            ret.trade_time_ms = data["T"].get_int64().value();
        }
    } else if (exchange_name == "gateio") {
        std::string currency_pair =
            std::string(data["currency_pair"].get_string().value());
        currency_pair.erase(
            std::remove(currency_pair.begin(), currency_pair.end(), '_'),
            currency_pair.end());
        ret.symbol = currency_pair;

        ret.price = std::stod(std::string(data["price"].get_string().value()));
        ret.quantity =
            std::stod(std::string(data["amount"].get_string().value()));

        std::string create_time_ms_str =
            std::string(data["create_time_ms"].get_string().value());
        ret.timestamp = static_cast<long long>(std::stod(create_time_ms_str));

        std::string side = std::string(data["side"].get_string().value());
        ret.is_buyer_maker = (side == "sell");

        ret.trade_id = std::to_string(data["id"].get_uint64().value());
    }

    ret.parsed_timestamp =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    ret.latency_ms = ret.parsed_timestamp - ret.timestamp;
    return ret;
}

}  // namespace MarketData

nlohmann::json MarketData::NormalizedParsedTrade::to_json() const {
    return {
        {"symbol", symbol},           {"exchange_name", exchange_name},
        {"market_type", market_type}, {"price", price},
        {"quantity", quantity},       {"parsed_timestamp", parsed_timestamp},
        {"timestamp", timestamp},     {"is_buyer_maker", is_buyer_maker},
        {"trade_id", trade_id},
    };
}

using arrow::compute::Cast;
using arrow::compute::CastOptions;

std::shared_ptr<arrow::Array> ensure_decimal128(
    const std::shared_ptr<arrow::Array>& arr) {
    using arrow::Decimal128Type;
    using arrow::Decimal256Type;

    if (arr->type_id() == arrow::Type::DECIMAL128) {
        return arr;
    }

    if (arr->type_id() == arrow::Type::DECIMAL256) {
        auto from_type = std::static_pointer_cast<Decimal256Type>(arr->type());

        // Clamp to max Decimal128 precision (38)
        int32_t precision = std::min(from_type->precision(), 38);
        int32_t scale = from_type->scale();
        auto to_type = arrow::decimal128(precision, scale);

        arrow::Result<arrow::Datum> cast_result = arrow::compute::Cast(
            arrow::Datum(arr), to_type, arrow::compute::CastOptions::Safe());

        if (!cast_result.ok()) {
            throw std::runtime_error(
                "Failed to cast from Decimal256 to Decimal128: " +
                cast_result.status().ToString());
        }

        return cast_result.ValueOrDie().make_array();
    }

    throw std::runtime_error("Expected decimal array type");
}

std::vector<MarketData::NormalizedParsedTrade>
MarketData::NormalizedParsedTrade::from_arrow_table(
    const std::shared_ptr<arrow::Table>& table, const std::string& symbol,
    const std::string& exchange_name, const std::string& market_type) {
    if (!table) {
        return {};
    }

    std::vector<NormalizedParsedTrade> trades;
    int64_t num_rows = table->num_rows();
    trades.reserve(num_rows);

    // Handle float64 arrays instead of decimal
    auto price_arr = std::static_pointer_cast<arrow::DoubleArray>(
        table->GetColumnByName("price")->chunk(0));
    auto qty_arr = std::static_pointer_cast<arrow::DoubleArray>(
        table->GetColumnByName("qty")->chunk(0));

    auto ts_arr = std::static_pointer_cast<arrow::Int64Array>(
        table->GetColumnByName("timestamp")->chunk(0));
    auto maker_col = table->GetColumnByName("is_buyer_maker");
    const uint8_t* maker_bits = nullptr;
    if (maker_col) {
        auto maker_arr = std::static_pointer_cast<arrow::BooleanArray>(
            maker_col->chunk(0));
        maker_bits = maker_arr->values()->data();
    }
    auto trade_id_arr = std::static_pointer_cast<arrow::LargeStringArray>(
        table->GetColumnByName("id")->chunk(0));

    // Raw data access for float64
    const double* price_vals = price_arr->raw_values();
    const double* qty_vals = qty_arr->raw_values();
    const int64_t* ts_vals = ts_arr->raw_values();

    for (int64_t i = 0; i < num_rows; ++i) {
        MarketData::NormalizedParsedTrade trade;
        trade.symbol = symbol;
        trade.exchange_name = exchange_name;
        trade.market_type = market_type;

        trade.price = price_vals[i];
        trade.quantity = qty_vals[i];
        trade.parsed_timestamp = ts_vals[i];
        trade.timestamp = ts_vals[i];
        // Missing column: side is unknown, so taker-buy volume stays zero.
        trade.is_buyer_maker =
            maker_bits ? arrow::bit_util::GetBit(maker_bits, i) : true;
#if ARROW_VERSION_MAJOR >= 14
        trade.trade_id = std::string(trade_id_arr->GetView(i));
#else
        trade.trade_id = trade_id_arr->GetString(i);
#endif
        trades.push_back(trade);
    }

    return trades;
}

std::string MarketData::NormalizedParsedTrade::get_key() const {
    return Core::Utils::get_symbol_exchange_market_key(symbol, exchange_name,
                                                       market_type);
}

std::vector<MarketData::NormalizedParsedTrade>
MarketData::NormalizedParsedTrade::from_list_of_aggregated_trades(
    const nlohmann::json& trades, const std::string& symbol,
    const std::string& exchange_name, const std::string& market_type) {
    std::vector<MarketData::NormalizedParsedTrade> ret;
    for (const auto& trade : trades) {
        AggregateTrade agg_trade = AggregateTrade::from_json(
            trade, symbol, exchange_name, market_type);
        const auto& normalized_trades = agg_trade.to_normalized_trades();
        ret.insert(ret.end(), normalized_trades.begin(),
                   normalized_trades.end());
    }
    return ret;
}

std::vector<MarketData::NormalizedParsedTrade>
MarketData::NormalizedParsedTrade::from_list_of_gateio_trades(
    const nlohmann::json& trades, const std::string& symbol,
    const std::string& exchange_name, const std::string& market_type) {
    std::vector<MarketData::NormalizedParsedTrade> ret;
    ret.reserve(trades.size());
    for (const auto& trade : trades) {
        NormalizedParsedTrade t;
        t.symbol = symbol;
        t.exchange_name = exchange_name;
        t.market_type = market_type;
        t.price = std::stod(trade["price"].get<std::string>());
        t.quantity = std::stod(trade["amount"].get<std::string>());
        std::string create_time_ms_str =
            trade["create_time_ms"].get<std::string>();
        t.timestamp = static_cast<long long>(std::stod(create_time_ms_str));
        t.parsed_timestamp = t.timestamp;
        t.is_buyer_maker = trade["side"].get<std::string>() == "sell";
        t.trade_id = trade["id"].get<std::string>();
        ret.push_back(t);
    }
    return ret;
}

std::vector<MarketData::NormalizedParsedTrade>
MarketData::NormalizedParsedTrade::from_list_of_binance_individual_trades(
    const nlohmann::json& trades, const std::string& symbol,
    const std::string& exchange_name, const std::string& market_type) {
    std::vector<MarketData::NormalizedParsedTrade> ret;
    ret.reserve(trades.size());
    for (const auto& trade : trades) {
        NormalizedParsedTrade t;
        t.symbol = symbol;
        t.exchange_name = exchange_name;
        t.market_type = market_type;
        t.price = std::stod(trade["price"].get<std::string>());
        t.quantity = std::stod(trade["qty"].get<std::string>());
        t.timestamp = trade["time"].get<long long>();
        t.parsed_timestamp = t.timestamp;
        t.is_buyer_maker = trade["isBuyerMaker"].get<bool>();
        t.trade_id = std::to_string(trade["id"].get<long long>());
        ret.push_back(t);
    }
    return ret;
}
