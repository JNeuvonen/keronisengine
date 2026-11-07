#include <atomic>
#include <future>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <sys/prctl.h>

#include "chrono_utils.h"
#include "datetime_utils.h"
#include "disk_logging/log.h"
#include "file.h"
#include "market_making/backtest_state.h"
#include "market_making/dtypes/engine_state_snapshot.h"
#include "market_making/engine.h"
#include "str_math.h"
#include "nlohmann/json.hpp"
#include "realtime_dashboard/dashboard.h"
#include "backtest_runner/config.h"
#ifdef BACKTEST_RUNNER_ENABLE_PACED_REPLAY
#include "backtest_runner/paced_backtest_view.h"
#include "backtest_runner/paced_replay_controller.h"
#endif
#include "market_data/state_machine_dispatcher.h"

void trading_log_callback(const std::vector<TradingLog> &logs) {
    for (const auto &log : logs) {
        std::cout << log.message << std::endl;
    }
}


namespace {

// Seed per-symbol precision for the backtest.
//
// exchange_symbol_infos is populated exclusively by Live::StateUpdater
// (initialize_exchange_info), which backtest_runner never instantiates -- so in
// a backtest the map is empty for every venue and
// Engine::get_price_precision falls through to its `return 0` default. Order
// fills then round every price to whole units.
//
// On crypto that is invisible: rounding BTCUSDT at ~95,000 to 0dp is a ~0.001%
// error. On a $6 equity it is ~8% per side, which is larger than any edge a
// strategy could have. Fills reported 8.00 and 13.00 for bars that actually
// traded 7.75 and 27.39.
//
// Scoped to polygon/stock deliberately. Crypto venues keep their current
// (wrong but long-standing) behaviour rather than having every existing
// backtest's numbers move underneath it as a side effect of this fix; that
// change deserves its own comparison against known runs.
constexpr double US_EQUITY_PRICE_TICK = 0.01;  // penny tick; mirrors
                                               // us_equity_catalog's
                                               // DEFAULT_PRICE_TICK
constexpr double US_EQUITY_LOT_SIZE = 1.0;     // whole shares

void seed_backtest_symbol_precision(const Core::SimulationInput &input) {
    std::unordered_map<std::string, std::vector<Core::SymbolExchangeInfo>>
        by_exchange_market;

    for (const auto &dataset : input.market_data.ohlcv_klines) {
        if (dataset.exchange_name != "polygon" ||
            dataset.market_type != "stock") {
            continue;
        }
        Core::SymbolExchangeInfo info;
        info.symbol = dataset.symbol;
        info.base = dataset.symbol;
        info.quote = "USD";
        info.type = "stock";
        info.is_active = true;
        info.precision.price = US_EQUITY_PRICE_TICK;
        info.precision.amount = US_EQUITY_LOT_SIZE;
        info.precision.base = US_EQUITY_LOT_SIZE;
        info.precision.quote = US_EQUITY_PRICE_TICK;
        info.precision_int.price =
            StrMath::step_size_to_precision(US_EQUITY_PRICE_TICK);
        info.precision_int.amount =
            StrMath::step_size_to_precision(US_EQUITY_LOT_SIZE);
        by_exchange_market[dataset.exchange_name + ":" + dataset.market_type]
            .push_back(std::move(info));
    }

    for (auto &[key, infos] : by_exchange_market) {
        const auto sep = key.find(':');
        Core::Engine::getInstance().add_symbol_exchange_info(
            key.substr(0, sep), key.substr(sep + 1), infos);
        std::cout << "[backtest] seeded precision for " << infos.size() << " "
                  << key << " symbols (price tick "
                  << US_EQUITY_PRICE_TICK << ")" << std::endl;
        if (!infos.empty()) {
            const auto probe = Core::Engine::getInstance().get_price_precision(
                infos.front().symbol, key.substr(0, sep), key.substr(sep + 1));
            std::cout << "[backtest] PROBE get_price_precision("
                      << infos.front().symbol << ") = " << probe
                      << " (expect 2)" << std::endl;
        }
    }
}

}  // namespace

int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <config_path>" << std::endl;
        return 1;
    }

    // Allow any same-uid process to attach with ptrace (gdb watchdog needs
    // this under YAMA ptrace_scope=1). Strictly an observability hook.
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);

    BacktestRunner::RunnerInput runner_input =
        BacktestRunner::Config::get_runner_input(argv);
    Core::SimulationInput& simulation_input = runner_input.simulation;

#ifndef BACKTEST_RUNNER_ENABLE_PACED_REPLAY
    if (runner_input.backtest_replay_mode ==
        BacktestRunner::BacktestReplayMode::Paced) {
        throw std::runtime_error(
            "paced replay requires BACKTEST_RUNNER_ENABLE_PACED_REPLAY=ON");
    }
#endif

    std::unordered_map<std::string, Core::FeeModel> fees_by_exchange_and_market;

    if (simulation_input.show_realtime_dashboard) {
        auto &view = Charting::Dashboard::getInstance();
        view.init(1280, 720, "Market Events Dashboard");
        view.set_buy_and_hold_enabled(!simulation_input.autofill_orders);
        view.update();
    }

    for (const auto &exchange : simulation_input.exchanges) {
        const auto &key = exchange.exchange_name + ":" + exchange.market_type;
        fees_by_exchange_and_market[key] =
            Core::FeeModel{.taker_fee_rate = exchange.taker_fee_rate,
                           .maker_fee_rate = exchange.maker_fee_rate};
    }

    auto &backtest_state = Core::BacktestState::getInstance();
    Core::Engine &engine = Core::Engine::getInstance();
    engine.initialize(
        trading_log_callback,
        [simulation_input](const Core::EngineStateSnapshot &snapshot) {
            Core::BacktestState::getInstance().update_engine_state_snapshot(
                snapshot);
            auto &view = Charting::Dashboard::getInstance();

            if (simulation_input.show_realtime_dashboard) {
                view.add_engine_state_snapshot(snapshot);
            }
        },
        Core::EngineConfig{
            .is_backtest = true,
            .is_strategy_development = simulation_input.is_strategy_development,
            .emit_trading_logs = simulation_input.emit_trading_logs,
            .fees_by_exchange_and_market = fees_by_exchange_and_market,
        });

    // SymbolPrecisionAccess is a separate singleton holding an Engine*, and
    // its only other caller is Live::StateUpdater -- which backtest_runner
    // never instantiates. Left unwired, every accessor short-circuits on
    // engine_ == nullptr and reports 0 decimal places, so Trade::to_json and
    // Order::to_json formatted every price and quantity as a whole number.
    // Fills themselves were always correct; the reports were not, which is
    // worse -- a backtest that lies only in its output looks like a working
    // backtest.
    engine.initialize_symbol_precision_access();

    seed_backtest_symbol_precision(simulation_input);

    backtest_state.add_sim_params(simulation_input);

    // Result dumps. When `output_dir` is set on the sim config, the runner
    // writes trades.json + summary.json + equity_curve.json under it. Empty
    // (default) suppresses all three — kept opt-in to match prior behavior.
    // Equity curve is registered before run_backtest() so per-sim-day flushes
    // land on disk during the run; trades + summary are emitted post-run.
    const std::string& output_dir = simulation_input.output_dir;
    const std::string trades_path =
        output_dir.empty() ? "" : output_dir + "/trades.json";
    const std::string summary_path =
        output_dir.empty() ? "" : output_dir + "/summary.json";
    const std::string equity_curve_path =
        output_dir.empty() ? "" : output_dir + "/equity_curve.json";
    if (!equity_curve_path.empty()) {
        backtest_state.set_equity_curve_dump_path(equity_curve_path);
    }

    // Equity curve was already flushed during the run; we re-emit it here so
    // the final sample lands even when the per-day boundary didn't trigger on
    // the last tick.
    auto dump_results = [&]() {
        engine.update_exposures_notional();
        // Trades reach backtest_state only through snapshots; flush the ones
        // completed after the last periodic snapshot.
        engine.emit_engine_state_snapshot();
        backtest_state.flush_fill_records();
        if (!trades_path.empty()) {
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& trade : backtest_state.get_all_backtest_trades()) {
                arr.push_back(trade.to_json());
            }
            std::ofstream out(trades_path);
            out << arr.dump(2);
            std::cout << "[backtest_runner] dumped " << arr.size()
                      << " trades to " << trades_path << std::endl;
        }
        if (!summary_path.empty()) {
            backtest_state.dump_summary_json(summary_path);
            std::cout << "[backtest_runner] dumped summary to "
                      << summary_path << std::endl;
        }
        if (!equity_curve_path.empty()) {
            backtest_state.dump_equity_curve_json(equity_curve_path);
            std::cout << "[backtest_runner] dumped equity curve to "
                      << equity_curve_path << std::endl;
        }
    };

    if (runner_input.backtest_replay_mode ==
        BacktestRunner::BacktestReplayMode::Paced) {
#ifdef BACKTEST_RUNNER_ENABLE_PACED_REPLAY
        BacktestRunner::PacedBacktestView view(
            simulation_input,
            runner_input.paced_replay.cockpit_root,
            runner_input.paced_replay.workspace_name,
            runner_input.paced_replay.legacy_workspace_path,
            runner_input.paced_replay.execution);
        if (!view.init()) {
            throw std::runtime_error("failed to initialize paced cockpit");
        }
        BacktestRunner::PacedReplayController controller(
            simulation_input.simulation_start_date_ms,
            simulation_input.simulation_end_date_ms,
            simulation_input.ohlcv_delay_ms,
            runner_input.paced_replay.initial_speed,
            runner_input.paced_replay.start_paused);
        long long visible_through_ms =
            simulation_input.simulation_start_date_ms;
        const auto observer = [&](const Core::MarketEvent& event) {
            view.observe_ohlcv_event(event, visible_through_ms);
        };
        auto prepare = std::async(std::launch::async, [&]() {
            backtest_state.prepare_paced_backtest(observer);
        });
        while (prepare.wait_for(std::chrono::milliseconds(0)) !=
               std::future_status::ready) {
            static_cast<void>(view.update_loading(
                backtest_state.get_market_data_loading_progress()));
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        prepare.get();
        std::cout << "[backtest_runner] paced replay starts "
                  << (runner_input.paced_replay.start_paused ? "paused"
                                                             : "running")
                  << " at " << runner_input.paced_replay.initial_speed << "x"
                  << std::endl;

        bool backtest_has_more = true;
        bool backtest_finished = false;
        while (view.update(controller, [&](long long target_time_ms) {
            visible_through_ms = target_time_ms;
            if (!backtest_has_more) return;
            backtest_has_more = backtest_state.advance_paced_backtest_through(
                target_time_ms, observer);
            if (!backtest_has_more) {
                backtest_state.finish_paced_backtest();
                backtest_finished = true;
                dump_results();
            }
        })) {
        }
        view.shutdown();
        if (!backtest_finished) {
            backtest_state.stop();
            std::cout << "[backtest_runner] paced window closed mid-run; "
                         "no engine results written"
                      << std::endl;
        }
#endif
    } else if (simulation_input.show_realtime_dashboard) {
        std::atomic<bool> backtest_done{false};
        std::thread backtest_thread([&]() {
            backtest_state.run_backtest();
            backtest_done.store(true);
        });
        auto &view = Charting::Dashboard::getInstance();
        // update() returns false once the window is closed. The loop outlives
        // the backtest so the finished charts stay inspectable.
        while (view.update()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        if (backtest_done.load()) {
            backtest_thread.join();
            dump_results();
        } else {
            // Closed mid-run: the trade set is partial and the thread still
            // owns it, so dumping would race. Abandon the run instead.
            backtest_thread.detach();
            std::cout << "[backtest_runner] window closed mid-run; "
                         "no results written" << std::endl;
        }
    } else {
        backtest_state.run_backtest();
        dump_results();
    }

    std::_Exit(0);
}
