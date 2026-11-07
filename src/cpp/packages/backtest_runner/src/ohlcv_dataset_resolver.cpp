#include "backtest_runner/ohlcv_dataset_resolver.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <thread>

#include <date/date.h>
#include <nlohmann/json.hpp>

namespace BacktestRunner {
namespace {

bool is_valid_registry_name(const std::string& value) {
    if (value.empty() || value.size() > 64) return false;
    return std::all_of(value.begin(), value.end(), [](char character) {
        const unsigned char byte = static_cast<unsigned char>(character);
        return std::islower(byte) || std::isdigit(byte) || character == '_';
    });
}

bool is_valid_symbol(const std::string& symbol) {
    return !symbol.empty() && symbol != "." && symbol != ".." &&
           symbol.find('/') == std::string::npos &&
           symbol.find('\\') == std::string::npos;
}

std::filesystem::path monthly_partition_path(
    const std::filesystem::path& dataset_root, const std::string& symbol,
    long long timestamp_ms) {
    const auto time_point = std::chrono::system_clock::time_point(
        std::chrono::milliseconds(timestamp_ms));
    const std::string year_month = date::format("%Y-%m", time_point);
    return dataset_root / symbol / "parquets" /
           ("year=" + year_month.substr(0, 4)) /
           ("month=" + year_month.substr(5, 2)) /
           (year_month + ".parquet");
}

std::vector<std::string> load_symbol_group(
    const Core::SimulationInput& simulation_input,
    const Core::OhlcvUniverseSelection& universe) {
    if (!is_valid_registry_name(universe.namespace_name) ||
        !is_valid_registry_name(universe.symbol_group)) {
        throw std::runtime_error(
            "symbol_group universe requires valid namespace_name and "
            "symbol_group");
    }
    const std::filesystem::path path =
        std::filesystem::path(simulation_input.market_data_library_root_path) /
        "namespaces" / universe.namespace_name / "symbol_groups" /
        (universe.symbol_group + ".json");
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("Unable to open symbol group: " +
                                 path.string());
    }
    nlohmann::json json;
    input >> json;
    return json.at("symbols").get<std::vector<std::string>>();
}

std::vector<std::string> load_available_symbols(
    const Core::SimulationInput& simulation_input,
    const Core::OhlcvDatasetSelection& selection) {
    std::vector<std::string> candidates;
    std::error_code error;
    std::filesystem::directory_iterator iterator(selection.dataset_root,
                                                  error);
    const std::filesystem::directory_iterator end;
    while (!error && iterator != end) {
        if (iterator->is_directory(error) && !error) {
            const std::string symbol = iterator->path().filename().string();
            if (is_valid_symbol(symbol) && symbol.front() != '.') {
                candidates.push_back(symbol);
            }
        }
        iterator.increment(error);
    }
    if (error) {
        throw std::runtime_error("Unable to enumerate OHLCV dataset root: " +
                                 error.message());
    }

    const int configured_threads =
        simulation_input.market_data_replay.reader_threads;
    const unsigned int hardware_threads =
        std::max(1u, std::thread::hardware_concurrency());
    const std::size_t thread_count = std::min<std::size_t>(
        candidates.size(), configured_threads > 0
                               ? static_cast<std::size_t>(configured_threads)
                               : static_cast<std::size_t>(hardware_threads));
    std::vector<unsigned char> available(candidates.size(), 0);
    std::vector<std::error_code> availability_errors(candidates.size());
    std::atomic<std::size_t> next_index{0};
    std::vector<std::thread> workers;
    workers.reserve(thread_count);
    for (std::size_t thread_index = 0; thread_index < thread_count;
         ++thread_index) {
        workers.emplace_back([&]() {
            while (true) {
                const std::size_t index = next_index.fetch_add(1);
                if (index >= candidates.size()) return;
                available[index] = std::filesystem::is_regular_file(
                    monthly_partition_path(
                        selection.dataset_root, candidates[index],
                        simulation_input.simulation_start_date_ms),
                    availability_errors[index]);
            }
        });
    }
    for (std::thread& worker : workers) worker.join();

    std::vector<std::string> symbols;
    symbols.reserve(candidates.size());
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        if (availability_errors[index] &&
            availability_errors[index] !=
                std::make_error_code(std::errc::no_such_file_or_directory) &&
            availability_errors[index] !=
                std::make_error_code(std::errc::not_a_directory)) {
            throw std::runtime_error(
                "Unable to inspect OHLCV partition for " + candidates[index] +
                ": " + availability_errors[index].message());
        }
        if (available[index]) symbols.push_back(std::move(candidates[index]));
    }
    return symbols;
}

}  // namespace

void OhlcvDatasetResolver::resolve(Core::SimulationInput& simulation_input) {
    const auto& selection =
        simulation_input.market_data.ohlcv_dataset_selection;
    if (!selection.has_value()) return;
    if (!simulation_input.market_data.ohlcv_klines.empty()) {
        throw std::runtime_error(
            "market_data.ohlcv_klines and ohlcv_dataset_selection cannot "
            "both define datasets");
    }
    if (selection->dataset_root.empty() || selection->exchange_name.empty() ||
        selection->market_type.empty() || selection->interval_ms != 60000) {
        throw std::runtime_error(
            "ohlcv_dataset_selection requires dataset_root, exchange_name, "
            "market_type, and interval_ms=60000");
    }

    std::vector<std::string> symbols =
        resolve_symbols(simulation_input, *selection);
    std::sort(symbols.begin(), symbols.end());
    symbols.erase(std::unique(symbols.begin(), symbols.end()), symbols.end());
    for (const std::string& symbol : symbols) {
        if (!is_valid_symbol(symbol)) {
            throw std::runtime_error("Invalid OHLCV symbol: " + symbol);
        }
        simulation_input.market_data.ohlcv_klines.push_back({
            .path =
                (std::filesystem::path(selection->dataset_root) / symbol)
                    .string(),
            .symbol = symbol,
            .exchange_name = selection->exchange_name,
            .market_type = selection->market_type,
        });
    }
}

std::vector<std::string> OhlcvDatasetResolver::resolve_symbols(
    const Core::SimulationInput& simulation_input,
    const Core::OhlcvDatasetSelection& selection) {
    const auto& universe = selection.universe;
    if (universe.mode == "explicit_symbols") return universe.symbols;
    if (universe.mode == "symbol_group") {
        return load_symbol_group(simulation_input, universe);
    }
    if (universe.mode == "all_available_on_date") {
        return load_available_symbols(simulation_input, selection);
    }
    throw std::runtime_error("Unknown OHLCV universe mode: " + universe.mode);
}

}  // namespace BacktestRunner
