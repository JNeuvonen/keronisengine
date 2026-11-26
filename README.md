# KeronisEngine

[![CI](https://github.com/JNeuvonen/keronisengine/actions/workflows/ci.yml/badge.svg)](https://github.com/JNeuvonen/keronisengine/actions/workflows/ci.yml)
[![Docs](https://github.com/JNeuvonen/keronisengine/actions/workflows/pages.yml/badge.svg)](https://engine.keronis.ai/)
[![License: Apache-2.0](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)

**Focus on finding alpha. Let KeronisEngine handle the execution.**

KeronisEngine is a C++20 trading engine with streaming feature state machines and in-process ONNX Runtime inference. Write a strategy and its features once in C++, then run the same code in offline feature generation (`historical_feature_gen`), event-driven backtesting (`backtest_runner`), paper trading (`PaperExchange`), and live trading (`live_infra`).

In production it ingests all of Binance's live market data (10,000+ messages/sec) while updating order books, computing streaming features, and scoring ONNX models on a single ~$100 dedicated server.

Documentation: [engine.keronis.ai](https://engine.keronis.ai/) (plain Markdown bundle for scripts/agents: [llms.txt](https://engine.keronis.ai/llms.txt), [llms-full.txt](https://engine.keronis.ai/llms-full.txt)).

## Packages

| Package | Description |
|---|---|
| `trading_engine` | `Engine`, `Strategy`, `TradeTicker`, `RingBufferBook`, order/trade lifecycle, risk limits, `BacktestState` |
| `market_data` | Partitioned Parquet OHLCV/trade readers, `LighterOrderBook`, `DepthHistory`, `StrategyStateMachine` |
| `live_infra` | Live process, `PaperExchange`, native clients for Hyperliquid, Aster, and Polymarket, `QuoteHolds` |
| `backtest_runner` | Event-driven replay of Parquet market data; writes `summary.json`, `trades.json`, `equity_curve.json` |
| `historical_feature_gen` | Replays registered state machines over historical data to produce Parquet training panels |
| `engine_control_server` | Localhost HTTP server for inspecting orders, inventory, books, and health |
| `strategies/` | Bundled examples: `sma_crossover`, `test_quoter`, `test_taker`, `test_market_maker`, `discretionary` |

## Architecture notes

The codebase is split into independent modules (`market_data`, `trading_engine`, `live_infra`, `backtest_runner`, `historical_feature_gen`, `engine_control_server`). `trading_engine` has no dependency on live sockets, PostgreSQL, or NATS; `LiveTradeTicker` (live) and `BacktestTradeTicker` (offline) are the execution seam.

- **Live runtime (`Live::StateUpdater`):** Owns `MarketDataStreamManager`, drains live market events through a lock-free `BlockingConcurrentQueue`, runs `AccountStreamManager` + REST reconciliation, hot-syncs strategy rows from PostgreSQL by fingerprint, persists state to disk, and coordinates cron standby handoffs via `/tmp/live_infra_state_lock` and SQLite heartbeats.
- **Order book (`RingBufferBook`):** Fixed tick-indexed flat array with 16-byte POD slots (`tick_tag`, `updated_sec`, `qty`), occupancy bitmap with `__builtin_ctzll` empty-word skipping, lazy OS page commitment, and runtime window sizing (19-bit for CEX books, 10-bit for Polymarket). Single-threaded `apply_bid` runs at ~1.5 ns/op on an Intel Core Ultra 9 285K with `-O3 -fno-fast-math -ffp-contract=off -frounding-math`.
- **Feature parity:** C++ `StrategyStateMachine` implementations (7 included, such as `six_venue_order` and `global_bba`) run offline in `historical_feature_gen` and online via `StateMachineDispatcher` + `SMSyncer`, feeding ONNX Runtime directly.
- **Execution simulation:** `BacktestState` replays Parquet events offline with configurable latency, fees, and Polymarket queue-position notional (`order_queue_notional_accumulated`). `PaperExchange` runs on its own thread inside `live_infra`, evaluating paper orders as taker at `ack + latency` and filling any resting remainder as maker against quotes and subsequent prints.
- **Live safety:** `QuoteHolds` persists quote-pull state across restarts and pulls resting entry quotes if the `quote_guard` heartbeat in `engine_health` goes stale (`IS_PROD=1`) or on `SIGTERM`.
- **Numerics:** Order quantities, fees, and PnL use a clean-room Apache-2.0 `mpreal` wrapper over dynamically linked MPFR/GMP.

## Build and test

Requirements (Linux): CMake >= 3.24, C++20 compiler, Ninja, [vcpkg](https://github.com/microsoft/vcpkg), Rust toolchain (for `src/rust/hyperliquid_signer`), and Python 3 with `pyarrow` in a venv for the quickstart data generator.

```bash
export VCPKG_ROOT=/path/to/vcpkg
ORT=$(./scripts/fetch_onnxruntime.sh)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DONNXRUNTIME_ROOT="$ORT"
cmake --build build -j"$(nproc)"

./build/bin/trading_engine_tests
./build/bin/live_infra_tests "[paper_exchange],[paper_trading],[quote_holds],[normalized_trade],[lighter_market_data],[ohlcv]"
```

Run the bundled `sma_crossover` backtest on synthetic 1-minute `BTCUSDT` Parquet bars:

```bash
python3 -m venv .venv && .venv/bin/pip install pyarrow
PYTHON=.venv/bin/python ./examples/quickstart_backtest/run.sh ./build/bin/backtest_runner
```

A `Dockerfile` and local PostgreSQL + NATS stack (`deploy/docker-compose.yml`) are also included. See [Quickstart](https://engine.keronis.ai/quickstart/) and [Live & Paper Trading](https://engine.keronis.ai/live-and-paper/).

## External strategies and state machines

Strategies and state machines self-register by name via static initializers. Point CMake at external source directories to compile your own code into the engine without forking this repository:

```bash
cmake -S . -B build ... -DKERONIS_STRATEGY_DIRS="/path/to/strategies;/path/to/state_machines"
```

Minimal strategy using [strategy_helpers.h](src/cpp/packages/trading_engine/include/market_making/strategy_helpers.h) (full runnable example in [sma_crossover.cpp](src/cpp/packages/strategies/sma_crossover/sma_crossover.cpp)):

```cpp
#include "market_making/strategy_helpers.h"
#include "market_making/strategy_registry.h"
#include "market_data/bar_close_policy.h"

namespace {
std::vector<Core::StrategyDecision> on_market_event(
    Core::Engine& engine, Core::Strategy& strategy,
    std::optional<Core::MarketEvent>) {
    std::vector<Core::StrategyDecision> decisions;
    for (auto& [symbol, ticker] : strategy.trade_tickers) {
        std::lock_guard<std::mutex> lock(ticker->trading_mutex);
        const auto& em = ticker->exchange_and_markets.front();
        const auto price = engine.get_price(symbol, em.exchange_name, em.market_type);
        if (!price || !ticker->get_orders(false).empty()) continue;

        Core::StrategyDecision decision;
        decision.symbol = symbol;
        if (ticker->get_trades(false).empty()) {
            Core::StrategyHelpers::submit_market_order(
                engine, strategy, *ticker, Core::OrderSide::BUY, *price,
                mpfr::mpreal(0.01), decision);
        }
        if (!decision.orders.empty()) decisions.push_back(std::move(decision));
    }
    return decisions;
}

void bind_example(Core::Strategy& strategy) {
    strategy.strategy_callback = on_market_event;
    strategy.compute_predictions_callback = [](Core::Engine&, Core::Strategy&) {};
    strategy.required_bar_types = {MarketData::BarClosePolicy::time_bars(60'000)};
}
}  // namespace

KERONIS_REGISTER_STRATEGY("example_strategy", bind_example);
```

## Status and disclaimer

The engine, state machines, backtester, and `PaperExchange` build and pass unit tests. Live execution still expects PostgreSQL, NATS, and (for non-native venues or proxy credential lookups) an external proxy not published in this repository.

This software is provided as-is under the Apache-2.0 license with no warranty. Automated trading carries risk of financial loss.

## License

Apache-2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE). MPFR and GMP are LGPL and are always linked as shared libraries.
