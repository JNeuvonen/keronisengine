// StrategyConfig::from_json is the one parser for a strategy: backtests feed
// it config.json entries, live feeds it DB rows normalized by
// PQUtils::strategy_rows_to_config.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "common_types/strategy.h"
#include "postgre_utils.h"

using Catch::Matchers::ContainsSubstring;
using nlohmann::json;

namespace {

json quoter_config() {
    return json::parse(R"({
        "id": 181,
        "strategy_name": "example-quoter",
        "strategy_type": "crypto_swap",
        "candle_interval_ms": 1000,
        "is_using_percentage_max_position": true,
        "percentage_max_position": 100,
        "max_directional_exposure": 3,
        "virtual_balance": 10000000,
        "num_max_orders_10_sec": 1000000,
        "needs_to_run_realtime": true,
        "model_schedule_path": "/data/fold_schedule.json",
        "exchanges_list": [{"exchange_name": "binance", "market_type": "swap", "account_id": 1}],
        "trade_tickers": [{"symbol": "BTCUSDT", "base_asset": "BTC", "quote_asset": "USDT"}],
        "models": [{"name": "pricer", "path_to_model": "/models/pricer.onnx"}]
    })");
}

// A strategy row as fetch_from_postgres returns it: every column text, NULL
// as "", exchanges_list a JSON string holding the array.
json quoter_row() {
    return json{
        {"id", "181"},
        {"strategy_name", "example-quoter"},
        {"strategy_type", "crypto_swap"},
        {"backtest_json_format_version", "3.0"},
        {"backtest_json_file", ""},
        {"version", "2.0"},
        {"created_at", "2026-10-26 00:00:00+00"},
        {"candle_interval_ms", "1000"},
        {"is_disabled", "f"},
        {"is_using_all_available_symbols", "f"},
        {"is_using_percentage_max_position", "t"},
        {"percentage_max_position", "7.5"},
        {"nominal_max_position", ""},
        {"max_directional_exposure", "3"},
        {"virtual_balance", "10000000"},
        {"num_max_orders_10_sec", "1000000"},
        {"skip_in_backtest", "f"},
        {"main_exchange", "binance"},
        {"account_mode", "shared"},
        {"series_slug", ""},
        {"series_slugs", "null"},
        {"model_schedule_path", ""},
        {"expected_decision_interval_ms", ""},
        {"exchanges_list",
         R"("[{\"exchange_name\": \"binance\", \"market_type\": \"swap\", \"account_id\": 90002}]")"},
    };
}

}  // namespace

TEST_CASE("StrategyConfig::from_json reads a backtest config entry",
          "[strategy_config]") {
    const auto strategy = StrategyConfig::from_json(quoter_config());
    CHECK(strategy.id == 181);
    CHECK(strategy.strategy_name == "example-quoter");
    CHECK(strategy.percentage_max_position == 1.0);
    CHECK(strategy.is_using_percentage_max_position);
    CHECK(strategy.max_directional_exposure == 3.0);
    CHECK(strategy.needs_to_run_realtime);
    REQUIRE(strategy.exchanges_list.size() == 1);
    CHECK(strategy.exchanges_list[0].account_id == 1);
    REQUIRE(strategy.trade_tickers.size() == 1);
    CHECK(strategy.trade_tickers[0].symbol == "BTCUSDT");
    CHECK(strategy.trade_tickers[0].strategy_id == 181);
    REQUIRE(strategy.strategy_models.size() == 1);
    CHECK(strategy.strategy_models[0].path_to_model == "/models/pricer.onnx");
}

TEST_CASE("StrategyConfig::from_json applies the column defaults",
          "[strategy_config]") {
    const auto strategy = StrategyConfig::from_json(
        json{{"id", 7}, {"strategy_name", "test_taker"}});
    CHECK(strategy.is_using_all_symbols);
    CHECK(strategy.main_exchange == "binance");
    CHECK(strategy.account_mode == "shared");
    CHECK(strategy.percentage_max_position == 0.0);
    CHECK(strategy.exchanges_list.empty());
    CHECK(strategy.trade_tickers.empty());
}

TEST_CASE("StrategyConfig::from_json rejects unknown keys at every level",
          "[strategy_config]") {
    auto config = quoter_config();
    config["percentage_max_postion"] = 50;
    CHECK_THROWS_WITH(StrategyConfig::from_json(config),
                      ContainsSubstring("percentage_max_postion"));

    config = quoter_config();
    config["trade_tickers"][0]["qoute_asset"] = "USDT";
    CHECK_THROWS_WITH(StrategyConfig::from_json(config),
                      ContainsSubstring("qoute_asset"));

    config = quoter_config();
    config["exchanges_list"][0]["acount_id"] = 1;
    CHECK_THROWS_WITH(StrategyConfig::from_json(config),
                      ContainsSubstring("acount_id"));

    config = quoter_config();
    config["skip_in_backtest"] = false;
    CHECK_THROWS_WITH(StrategyConfig::from_json(config),
                      ContainsSubstring("skip_in_backtest"));
}

TEST_CASE("StrategyConfig::from_json rejects missing required keys",
          "[strategy_config]") {
    auto config = quoter_config();
    config.erase("id");
    CHECK_THROWS_WITH(StrategyConfig::from_json(config),
                      ContainsSubstring("'id'"));

    config = quoter_config();
    config.erase("strategy_name");
    CHECK_THROWS_WITH(StrategyConfig::from_json(config),
                      ContainsSubstring("strategy_name"));

    config = quoter_config();
    config["trade_tickers"][0].erase("symbol");
    CHECK_THROWS_WITH(StrategyConfig::from_json(config),
                      ContainsSubstring("symbol"));
}

TEST_CASE("StrategyConfig::from_json rejects wrongly typed values",
          "[strategy_config]") {
    auto config = quoter_config();
    config["candle_interval_ms"] = "1000";
    CHECK_THROWS_WITH(StrategyConfig::from_json(config),
                      ContainsSubstring("candle_interval_ms"));

    config = quoter_config();
    config["candle_interval_ms"] = 1000.5;
    CHECK_THROWS(StrategyConfig::from_json(config));

    config = quoter_config();
    config["is_disabled"] = 0;
    CHECK_THROWS_WITH(StrategyConfig::from_json(config),
                      ContainsSubstring("is_disabled"));

    config = quoter_config();
    config["trade_tickers"] = json::object();
    CHECK_THROWS_WITH(StrategyConfig::from_json(config),
                      ContainsSubstring("trade_tickers"));
}

TEST_CASE("strategy_rows_to_config normalizes a DB row for from_json",
          "[strategy_config]") {
    const std::vector<json> tickers = {
        {{"id", "11"},
         {"strategy_id", "181"},
         {"symbol", "ETHUSDT"},
         {"base_asset", "ETH"},
         {"quote_asset", "USDT"},
         {"created_at", "2026-10-26 00:00:00+00"},
         {"is_active", "t"},
         {"order_min_size", ""},
         {"event_start_time_ms", "1760000000000"},
         {"outcome_names", R"(["Up", "Down"])"},
         {"clob_token_ids", "{}"}},
    };
    const std::vector<json> models = {
        {{"name", "pricer"}, {"path_to_model", "/models/pricer.onnx"}}};
    const auto strategy = StrategyConfig::from_json(
        PQUtils::strategy_rows_to_config(quoter_row(), tickers, models));

    CHECK(strategy.id == 181);
    CHECK(strategy.percentage_max_position == 7.5 / 100);
    CHECK(strategy.nominal_max_position == 0.0);
    CHECK_FALSE(strategy.is_using_all_symbols);
    CHECK(strategy.series_slugs.empty());
    REQUIRE(strategy.exchanges_list.size() == 1);
    CHECK(strategy.exchanges_list[0].exchange_name == "binance");
    CHECK(strategy.exchanges_list[0].account_id == 90002);
    REQUIRE(strategy.trade_tickers.size() == 1);
    const auto& ticker = strategy.trade_tickers[0];
    CHECK(ticker.id == 11);
    CHECK(ticker.strategy_id == 181);
    CHECK(ticker.is_active);
    CHECK(ticker.order_min_size == 0.0);
    CHECK(ticker.event_start_time_ms == 1760000000000LL);
    CHECK(ticker.outcome_names == std::vector<std::string>{"Up", "Down"});
    CHECK(ticker.clob_token_ids.empty());
    REQUIRE(strategy.strategy_models.size() == 1);
    CHECK(strategy.strategy_models[0].name == "pricer");
}

TEST_CASE("strategy_rows_to_config keeps NULL text as empty",
          "[strategy_config]") {
    auto row = quoter_row();
    row["main_exchange"] = "";
    row["account_mode"] = "";
    row["is_disabled"] = "";
    row["series_slugs"] = R"(["btc-up-or-down-5m"])";
    const auto strategy =
        StrategyConfig::from_json(PQUtils::strategy_rows_to_config(row, {}, {}));
    CHECK(strategy.main_exchange.empty());
    CHECK(strategy.account_mode.empty());
    CHECK_FALSE(strategy.is_disabled);
    CHECK(strategy.series_slugs ==
          std::vector<std::string>{"btc-up-or-down-5m"});
}

TEST_CASE("strategy_rows_to_config reads an unwrapped exchanges_list, dropping unread keys",
          "[strategy_config]") {
    auto row = quoter_row();
    row["exchanges_list"] =
        R"([{"exchange_name": "hyperliquid", "market_type": "swap", "symbols": ["SKHX"]}])";
    const auto strategy =
        StrategyConfig::from_json(PQUtils::strategy_rows_to_config(row, {}, {}));
    REQUIRE(strategy.exchanges_list.size() == 1);
    CHECK(strategy.exchanges_list[0].exchange_name == "hyperliquid");
    CHECK_FALSE(strategy.exchanges_list[0].account_id.has_value());
}

TEST_CASE("strategy_rows_to_config refuses formats other than 3.0",
          "[strategy_config]") {
    auto row = quoter_row();
    row["backtest_json_format_version"] = "2.0";
    CHECK_THROWS_WITH(PQUtils::strategy_rows_to_config(row, {}, {}),
                      ContainsSubstring("only 3.0"));
}
