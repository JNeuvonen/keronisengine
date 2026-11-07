#pragma once

#include <chrono>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace Db {

struct Dataset {
    int id;
    std::string relative_path;
    std::string dataset_type;

    bool is_mexc_dataset;
    bool is_binance_dataset;
    bool is_crypto_dataset;
    bool is_stock_market_dataset;
    bool is_alternative_dataset;
    bool is_solana_defi_dataset;
    bool is_concatenated_dataset;
    bool is_yfinance_dataset;

    std::optional<std::chrono::system_clock::time_point> start_date;
    std::optional<std::chrono::system_clock::time_point> end_date;

    int num_rows;
    std::string interval;
    std::string price_column;
    std::string open_price_column;
    std::string high_price_column;
    std::string low_price_column;
    std::string close_price_column;
    std::string timeseries_column;
    std::string target_column;
    std::string volume_column;
    std::string funding_column;
    std::string funding_time_column;
    std::string symbol;
    std::string quote_asset;
    std::string base_asset;

    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point updated_at;

    nlohmann::json to_json() const {
        nlohmann::json j;
        j["id"] = id;
        j["relative_path"] = relative_path;
        j["dataset_type"] = dataset_type;
        j["is_mexc_dataset"] = is_mexc_dataset;
        j["is_binance_dataset"] = is_binance_dataset;
        j["is_crypto_dataset"] = is_crypto_dataset;
        j["is_alternative_dataset"] = is_alternative_dataset;
        j["is_solana_defi_dataset"] = is_solana_defi_dataset;
        j["is_concatenated_dataset"] = is_concatenated_dataset;
        j["is_yfinance_dataset"] = is_yfinance_dataset;
        j["num_rows"] = num_rows;
        j["interval"] = interval;
        j["price_column"] = price_column;
        j["open_price_column"] = open_price_column;
        j["high_price_column"] = high_price_column;
        j["low_price_column"] = low_price_column;
        j["close_price_column"] = close_price_column;
        j["timeseries_column"] = timeseries_column;
        j["target_column"] = target_column;
        j["volume_column"] = volume_column;
        j["funding_column"] = funding_column;
        j["funding_time_column"] = funding_time_column;
        j["symbol"] = symbol;
        j["quote_asset"] = quote_asset;
        j["base_asset"] = base_asset;
        return j;
    }

    static Dataset from_json(const nlohmann::json& j) {
        Dataset dataset;

        dataset.id = j.contains("id") ? j["id"].get<int>() : 0;
        dataset.relative_path = j.contains("relative_path")
                                    ? j["relative_path"].get<std::string>()
                                    : "";
        dataset.dataset_type = j.contains("dataset_type")
                                   ? j["dataset_type"].get<std::string>()
                                   : "";

        dataset.is_mexc_dataset = j.contains("is_mexc_dataset")
                                      ? j["is_mexc_dataset"].get<bool>()
                                      : false;
        dataset.is_binance_dataset = j.contains("is_binance_dataset")
                                         ? j["is_binance_dataset"].get<bool>()
                                         : false;
        dataset.is_crypto_dataset = j.contains("is_crypto_dataset")
                                        ? j["is_crypto_dataset"].get<bool>()
                                        : false;
        dataset.is_alternative_dataset =
            j.contains("is_alternative_dataset")
                ? j["is_alternative_dataset"].get<bool>()
                : false;
        dataset.is_solana_defi_dataset =
            j.contains("is_solana_defi_dataset")
                ? j["is_solana_defi_dataset"].get<bool>()
                : false;
        dataset.is_concatenated_dataset =
            j.contains("is_concatenated_dataset")
                ? j["is_concatenated_dataset"].get<bool>()
                : false;
        dataset.is_yfinance_dataset = j.contains("is_yfinance_dataset")
                                          ? j["is_yfinance_dataset"].get<bool>()
                                          : false;

        dataset.num_rows =
            j.contains("num_rows") ? j["num_rows"].get<int>() : 0;
        dataset.interval =
            j.contains("interval") ? j["interval"].get<std::string>() : "";
        dataset.price_column = j.contains("price_column")
                                   ? j["price_column"].get<std::string>()
                                   : "";
        dataset.open_price_column =
            j.contains("open_price_column")
                ? j["open_price_column"].get<std::string>()
                : "";
        dataset.high_price_column =
            j.contains("high_price_column")
                ? j["high_price_column"].get<std::string>()
                : "";
        dataset.low_price_column =
            j.contains("low_price_column")
                ? j["low_price_column"].get<std::string>()
                : "";
        dataset.close_price_column =
            j.contains("close_price_column")
                ? j["close_price_column"].get<std::string>()
                : "";
        dataset.timeseries_column =
            j.contains("timeseries_column")
                ? j["timeseries_column"].get<std::string>()
                : "";
        dataset.target_column = j.contains("target_column")
                                    ? j["target_column"].get<std::string>()
                                    : "";
        dataset.volume_column = j.contains("volume_column")
                                    ? j["volume_column"].get<std::string>()
                                    : "";
        dataset.funding_column = j.contains("funding_column")
                                     ? j["funding_column"].get<std::string>()
                                     : "";
        dataset.funding_time_column =
            j.contains("funding_time_column")
                ? j["funding_time_column"].get<std::string>()
                : "";
        dataset.symbol =
            j.contains("symbol") ? j["symbol"].get<std::string>() : "";
        dataset.quote_asset = j.contains("quote_asset")
                                  ? j["quote_asset"].get<std::string>()
                                  : "";
        dataset.base_asset =
            j.contains("base_asset") ? j["base_asset"].get<std::string>() : "";

        return dataset;
    }
};

}  // namespace Db
