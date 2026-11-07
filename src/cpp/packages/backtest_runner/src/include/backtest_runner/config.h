#pragma once

#include <array>
#include <string>

#include "market_making/dtypes/simulation_input.h"

namespace BacktestRunner {

enum class BacktestReplayMode { Cpu, Paced };

struct CockpitExecutionConfig {
    bool enabled = false;
    double starting_cash = 250000.0;
    std::array<double, 4> notional_presets_nav_percent{0.5, 1.0, 2.0, 4.0};
    double notional_adjustment_nav_percent = 1.0;
    double order_move_basis_points = 5.0;
    double max_order_nav_percent = 50.0;
    double maker_fee_rate = 0.0;
    bool allow_short = true;
    std::string results_path;
};

struct PacedReplayConfig {
    double initial_speed = 1.0;
    bool start_paused = true;
    std::string cockpit_root = "/tmp/trading_cockpit";
    std::string workspace_name = "main";
    std::string legacy_workspace_path;
    CockpitExecutionConfig execution;
};

struct RunnerInput {
    Core::SimulationInput simulation;
    BacktestReplayMode backtest_replay_mode = BacktestReplayMode::Cpu;
    PacedReplayConfig paced_replay;
};

class Config {
   public:
    static RunnerInput get_runner_input(char* argv[]);
};
}  // namespace BacktestRunner
