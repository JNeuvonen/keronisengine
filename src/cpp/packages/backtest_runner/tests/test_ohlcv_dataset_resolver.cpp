#include <chrono>
#include <filesystem>
#include <fstream>

#include <catch2/catch_test_macros.hpp>

#include "backtest_runner/ohlcv_dataset_resolver.h"

namespace {

std::filesystem::path unique_root() {
    return std::filesystem::temp_directory_path() /
           ("ohlcv_dataset_resolver_" +
            std::to_string(std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count()));
}

void create_month(const std::filesystem::path& root,
                  const std::string& symbol) {
    const std::filesystem::path path =
        root / symbol / "parquets" / "year=2026" / "month=09" /
        "2026-09.parquet";
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path).put('\n');
}

Core::SimulationInput base_input(const std::filesystem::path& data_root,
                                 const std::filesystem::path& dataset_root) {
    Core::SimulationInput input{};
    input.simulation_start_date_ms = 1789565100000;
    input.market_data_library_root_path = data_root.string();
    input.market_data.ohlcv_dataset_selection =
        Core::OhlcvDatasetSelection{
            .dataset_root = dataset_root.string(),
            .exchange_name = "polygon",
            .market_type = "stock",
            .interval_ms = 60000,
        };
    return input;
}

}  // namespace

TEST_CASE("all-available OHLCV selection is date-scoped and sorted",
          "[backtest_runner][dataset_resolver]") {
    const std::filesystem::path root = unique_root();
    const std::filesystem::path datasets = root / "bars";
    create_month(datasets, "ZZZ");
    create_month(datasets, "AAA");
    std::filesystem::create_directories(datasets / "MISSING");

    Core::SimulationInput input = base_input(root, datasets);
    input.market_data.ohlcv_dataset_selection->universe.mode =
        "all_available_on_date";
    input.market_data_replay.reader_threads = 2;
    BacktestRunner::OhlcvDatasetResolver::resolve(input);

    REQUIRE(input.market_data.ohlcv_klines.size() == 2);
    REQUIRE(input.market_data.ohlcv_klines[0].symbol == "AAA");
    REQUIRE(input.market_data.ohlcv_klines[1].symbol == "ZZZ");
    std::filesystem::remove_all(root);
}

TEST_CASE("symbol groups resolve from the market-data registry",
          "[backtest_runner][dataset_resolver]") {
    const std::filesystem::path root = unique_root();
    const std::filesystem::path datasets = root / "bars";
    const std::filesystem::path group =
        root / "namespaces" / "cockpit" / "symbol_groups" / "focus.json";
    std::filesystem::create_directories(group.parent_path());
    {
        std::ofstream output(group);
        output << R"({"name":"focus","symbols":["SPY","AAPL","SPY"]})";
    }

    Core::SimulationInput input = base_input(root, datasets);
    auto& universe =
        input.market_data.ohlcv_dataset_selection->universe;
    universe.mode = "symbol_group";
    universe.namespace_name = "cockpit";
    universe.symbol_group = "focus";
    BacktestRunner::OhlcvDatasetResolver::resolve(input);

    REQUIRE(input.market_data.ohlcv_klines.size() == 2);
    REQUIRE(input.market_data.ohlcv_klines[0].symbol == "AAPL");
    REQUIRE(input.market_data.ohlcv_klines[1].symbol == "SPY");
    std::filesystem::remove_all(root);
}

TEST_CASE("explicit datasets and a selector cannot both define OHLCV",
          "[backtest_runner][dataset_resolver]") {
    const std::filesystem::path root = unique_root();
    Core::SimulationInput input = base_input(root, root / "bars");
    input.market_data.ohlcv_dataset_selection->universe.mode =
        "explicit_symbols";
    input.market_data.ohlcv_dataset_selection->universe.symbols = {"AAPL"};
    input.market_data.ohlcv_klines.push_back(
        {.path = "/tmp/SPY",
         .symbol = "SPY",
         .exchange_name = "polygon",
         .market_type = "stock"});

    REQUIRE_THROWS(BacktestRunner::OhlcvDatasetResolver::resolve(input));
}

TEST_CASE("dataset selection rejects unsupported bar intervals",
          "[backtest_runner][dataset_resolver]") {
    const std::filesystem::path root = unique_root();
    Core::SimulationInput input = base_input(root, root / "bars");
    input.market_data.ohlcv_dataset_selection->interval_ms = 5000;
    input.market_data.ohlcv_dataset_selection->universe.mode =
        "explicit_symbols";
    input.market_data.ohlcv_dataset_selection->universe.symbols = {"AAPL"};

    REQUIRE_THROWS(BacktestRunner::OhlcvDatasetResolver::resolve(input));
}

TEST_CASE("legacy simulation configs need no replay selection fields",
          "[backtest_runner][dataset_resolver]") {
    const nlohmann::json json = {
        {"market_data", {{"ohlcv_klines", nlohmann::json::array()}}},
        {"read_all_market_data_at_once", true},
    };

    Core::SimulationInput input = json.get<Core::SimulationInput>();
    BacktestRunner::OhlcvDatasetResolver::resolve(input);

    REQUIRE(input.read_all_market_data_at_once);
    REQUIRE(input.market_data_replay.load_mode.empty());
    REQUIRE_FALSE(input.market_data.ohlcv_dataset_selection.has_value());
}
