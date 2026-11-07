#include "backtest_runner/config.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

#include "backtest_runner/ohlcv_dataset_resolver.h"
#include "datetime_utils.h"
#include "file.h"
#include "strict_json.h"

BacktestRunner::RunnerInput BacktestRunner::Config::get_runner_input(
    char* argv[]) {
    RunnerInput input;
    Core::SimulationInput& simulation_input = input.simulation;

    std::string config_path = argv[1];
    std::string config_str = CommonUtils::read_file(config_path);
    nlohmann::json config = nlohmann::json::parse(config_str);

    simulation_input = config.get<Core::SimulationInput>();
    const std::string replay_mode =
        config.value("backtest_replay_mode", "cpu");
    for (const char* removed :
         {"use_strategies_from_db", "load_live_trades_from_db",
          "run_incremental_replay", "write_engine_state_snapshots_to_db",
          "is_full_replay_sim", "backtest_bar_symbols", "snapshot_strategy_ids",
          "prep_command"}) {
        if (config.contains(removed)) {
            throw std::runtime_error(
                std::string("config key '") + removed +
                "' is no longer supported; a backtest runs exactly the "
                "strategies in its \"strategies\" list");
        }
    }
    if (!config.contains("strategies")) {
        throw std::runtime_error("config needs a \"strategies\" list");
    }
    std::set<int> strategy_ids;
    for (const auto& strategy :
         StrictJson::array_at(config, "strategies", "config")) {
        simulation_input.strategies.push_back(
            StrategyConfig::from_json(strategy));
        if (!strategy_ids.insert(simulation_input.strategies.back().id)
                 .second) {
            throw std::runtime_error(
                "config lists strategy id " +
                std::to_string(simulation_input.strategies.back().id) +
                " twice");
        }
    }
    if (simulation_input.strategies.empty() && replay_mode != "paced") {
        throw std::runtime_error("config's \"strategies\" list is empty");
    }
    for (const auto& account :
         StrictJson::array_at(config, "discretionary_accounts", "config")) {
        const std::string context = "discretionary_accounts";
        StrictJson::check_keys(
            account,
            {"account_id", "paper", "enabled", "max_directional_exposure",
             "max_order_notional", "num_max_orders_10_sec",
             "num_max_orders_1_min", "num_max_orders_1_hour"},
            context);
        simulation_input.discretionary_accounts.push_back({
            .account_id =
                StrictJson::require<long long>(account, "account_id", context),
            .paper = StrictJson::require<bool>(account, "paper", context),
            .enabled = StrictJson::require<bool>(account, "enabled", context),
            .max_directional_exposure = StrictJson::require<double>(
                account, "max_directional_exposure", context),
            .max_order_notional = StrictJson::require<double>(
                account, "max_order_notional", context),
            .num_max_orders_10_sec = StrictJson::require<int>(
                account, "num_max_orders_10_sec", context),
            .num_max_orders_1_min = StrictJson::require<int>(
                account, "num_max_orders_1_min", context),
            .num_max_orders_1_hour = StrictJson::require<int>(
                account, "num_max_orders_1_hour", context),
        });
    }
    if (replay_mode == "cpu") {
        input.backtest_replay_mode = BacktestReplayMode::Cpu;
    } else if (replay_mode == "paced") {
        input.backtest_replay_mode = BacktestReplayMode::Paced;
    } else {
        throw std::runtime_error("Unknown backtest_replay_mode: " +
                                 replay_mode);
    }

    if (config.contains("paced_replay")) {
        const auto& paced = config.at("paced_replay");
        input.paced_replay.initial_speed =
            paced.value("initial_speed", 1.0);
        input.paced_replay.start_paused =
            paced.value("start_paused", true);
        input.paced_replay.cockpit_root = paced.value(
            "cockpit_root", std::string("/tmp/trading_cockpit"));
        input.paced_replay.workspace_name =
            paced.value("workspace_name", std::string("main"));
        input.paced_replay.legacy_workspace_path = paced.value(
            "legacy_workspace_path",
            paced.value("cockpit_state_path", std::string{}));
        if (paced.contains("execution")) {
            const auto& execution = paced.at("execution");
            auto& target = input.paced_replay.execution;
            for (const char* deprecated : {"notional_presets",
                                           "max_order_notional",
                                           "max_gross_notional"}) {
                if (execution.contains(deprecated)) {
                    throw std::runtime_error(
                        std::string("paced_replay.execution.") +
                        deprecated + " uses retired absolute sizing");
                }
            }
            if (execution.contains(
                    "max_gross_exposure_nav_percent")) {
                throw std::runtime_error(
                    "paced_replay.execution no longer uses an aggregate "
                    "exposure cap");
            }
            target.enabled = execution.value("enabled", false);
            target.starting_cash =
                execution.value("starting_cash", target.starting_cash);
            target.notional_adjustment_nav_percent = execution.value(
                "notional_adjustment_nav_percent",
                target.notional_adjustment_nav_percent);
            target.order_move_basis_points = execution.value(
                "order_move_basis_points",
                target.order_move_basis_points);
            target.max_order_nav_percent = execution.value(
                "max_order_nav_percent", target.max_order_nav_percent);
            target.maker_fee_rate = execution.value(
                "maker_fee_rate", target.maker_fee_rate);
            target.allow_short =
                execution.value("allow_short", target.allow_short);
            target.results_path =
                execution.value("results_path", std::string{});
            if (execution.contains("notional_presets_nav_percent")) {
                target.notional_presets_nav_percent =
                    execution.at("notional_presets_nav_percent")
                        .get<std::array<double, 4>>();
            }
        }
    }
    if (!std::isfinite(input.paced_replay.initial_speed) ||
        input.paced_replay.initial_speed <= 0.0) {
        throw std::runtime_error(
            "paced_replay.initial_speed must be positive");
    }
    const CockpitExecutionConfig& execution =
        input.paced_replay.execution;
    const auto positive_finite = [](double value) {
        return std::isfinite(value) && value > 0.0;
    };
    if (execution.enabled &&
        (!positive_finite(execution.starting_cash) ||
         !positive_finite(execution.notional_adjustment_nav_percent) ||
         !positive_finite(execution.order_move_basis_points) ||
         !positive_finite(execution.max_order_nav_percent) ||
         execution.notional_adjustment_nav_percent >
             execution.max_order_nav_percent ||
         !std::isfinite(execution.maker_fee_rate) ||
         execution.maker_fee_rate < 0.0 ||
         !std::all_of(execution.notional_presets_nav_percent.begin(),
                      execution.notional_presets_nav_percent.end(),
                      [&](double value) {
                          return positive_finite(value) &&
                                 value <= execution.max_order_nav_percent;
                      }))) {
        throw std::runtime_error(
            "paced_replay.execution requires positive finite cash, "
            "order_move_basis_points, NAV-percent presets, adjustment, "
            "and limits; presets and adjustment must not exceed "
            "max_order_nav_percent, and maker_fee_rate must be "
            "non-negative");
    }
    if (input.backtest_replay_mode == BacktestReplayMode::Paced &&
        simulation_input.show_realtime_dashboard) {
        throw std::runtime_error(
            "paced replay cannot use show_realtime_dashboard");
    }
    const auto& replay = simulation_input.market_data_replay;
    if (!replay.load_mode.empty() && replay.load_mode != "eager" &&
        replay.load_mode != "streaming") {
        throw std::runtime_error("Unknown market_data_replay.load_mode: " +
                                 replay.load_mode);
    }
    if (replay.reader_threads < 0 || replay.reader_threads > 256 ||
        replay.chunk_duration_ms < 60000 ||
        replay.chunk_duration_ms > 3600000 ||
        replay.cache_window_ms < 0 ||
        (replay.cache_window_ms > 0 &&
         (replay.cache_window_ms < replay.chunk_duration_ms ||
          replay.cache_window_ms % replay.chunk_duration_ms != 0))) {
        throw std::runtime_error(
            "market_data_replay requires reader_threads in [0, 256] and "
            "chunk_duration_ms in [60000, 3600000], cache_window_ms zero "
            "or a positive multiple of chunk_duration_ms");
    }
    if (replay.load_mode == "streaming" &&
        simulation_input.read_all_market_data_at_once) {
        throw std::runtime_error(
            "streaming market_data_replay cannot use "
            "read_all_market_data_at_once=true");
    }
    if (replay.load_mode == "eager") {
        simulation_input.read_all_market_data_at_once = true;
    }
    OhlcvDatasetResolver::resolve(simulation_input);

    return input;
}
