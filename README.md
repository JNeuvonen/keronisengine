# KeronisEngine

[![CI](https://github.com/JNeuvonen/keronisengine/actions/workflows/ci.yml/badge.svg)](https://github.com/JNeuvonen/keronisengine/actions/workflows/ci.yml)
[![Docs](https://github.com/JNeuvonen/keronisengine/actions/workflows/pages.yml/badge.svg)](https://engine.keronis.ai/)
[![License: Apache-2.0](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)

KeronisEngine is an open-source C++20 execution and real-time ML inference engine for electronic markets. It runs streaming feature state machines, in-process ONNX Runtime models, order books, risk checks, and venue execution from a single binary—supporting sub-millisecond tick-to-prediction-to-order paths on commodity hardware.

Built for crypto spot/perpetuals and prediction markets (including native Polymarket CLOB execution and queue-notional backtesting), its modular market-data, multi-outcome ticker, and execution interfaces extend cleanly to new exchanges or sports-betting venues. In production, a single ~$100 Linux server ingests full-exchange crypto feeds (10,000+ messages/sec across Binance) while updating order books, streaming features, and continuous ONNX predictions.

Documentation: **[engine.keronis.ai](https://engine.keronis.ai/)** · Machine-readable Markdown: **[llms.txt](https://engine.keronis.ai/llms.txt)** / **[llms-full.txt](https://engine.keronis.ai/llms-full.txt)**

## Core capabilities

- **Sub-ms inference & order book:** Flat tick-indexed `RingBufferBook` (~1.5 ns/op `apply_bid` on Core Ultra 9 285K under strict IEEE-754 flags), lock-free event queues, streaming C++ `StrategyStateMachine`s, and in-process `OnnxModel::predict()`.
- **Unified feature & strategy code:** The same C++ state machines and strategy callbacks run in `historical_feature_gen`, `backtest_runner`, `PaperExchange`, and `live_infra`.
- **Crypto & prediction markets:** Native execution for Polymarket (CLOB WebSockets, EIP-712 bulk signing, multi-outcome tickers, queue-notional backtesting), Hyperliquid (bundled Rust signer), and Aster; native L2 market data for Lighter; extensible to new exchanges or sports-betting venues.
- **Out-of-tree plugins:** Register strategies (`KERONIS_REGISTER_STRATEGY`) and state machines (`KERONIS_REGISTER_STATE_MACHINE`) from external directories (`-DKERONIS_STRATEGY_DIRS=...`) without forking this repository.
- **Production safety:** Durable `QuoteHolds` surviving restarts, `quote_guard` heartbeat enforcement, cron standby handoff, disk crash recovery, high-precision MPFR accounting, and a localhost HTTP control server.

## Packages

| Package | Description |
|---|---|
| `trading_engine` | `Core::Engine`, `Core::Strategy`, `Core::TradeTicker`, `RingBufferBook`, risk/rate limits, `BacktestState` |
| `market_data` | Partitioned Parquet OHLCV/trade readers, `LighterOrderBook`, `DepthHistory`, `StrategyStateMachine`, `StateMachineDispatcher` |
| `live_infra` | `Live::StateUpdater`, `PaperExchange`, `QuoteHolds`, `AccountStreamManager`, native clients (Polymarket, Hyperliquid, Aster) |
| `backtest_runner` | Offline Parquet replay; outputs `summary.json`, `trades.json`, `equity_curve.json` |
| `historical_feature_gen` | Offline replay of registered state machines to build Parquet ML training sets |
| `engine_control_server` | Embedded localhost HTTP server for inspecting orders, inventory, books, and health |
| `strategies/` | Flagship examples: `polymarket_digital` (market making), `binance_onnx_perp` (predictive ML), `cross_venue_arb` (arbitrage); plus `sma_crossover`, `test_quoter`, `test_taker`, `test_market_maker`, `discretionary` |
| `src/py/cpp_live_proxy` | Bundled FastAPI venue proxy for CCXT crypto exchanges (Binance, Bybit, OKX, Gate.io, Bitget, Coinbase), US equities (IBKR), user-data WebSockets, and direct-signing credentials |

## Streaming features → ONNX → order

Excerpt from [`binance_onnx_perp.cpp`](src/cpp/packages/strategies/binance_onnx_perp/binance_onnx_perp.cpp) (also see [`polymarket_digital.cpp`](src/cpp/packages/strategies/polymarket_digital/polymarket_digital.cpp) for cross-venue Polymarket CLOB execution):

```cpp
void compute_predictions(Core::Engine& engine, Core::Strategy& strategy) {
    auto sm = MarketData::StateMachineDispatcher::get_instance().get_machine(
        "windowed_taker_flow");
    auto features = sm ? sm->emit_state() : std::nullopt;
    if (!features || features->empty()) return;

    float score = 0.5f;
    if (!strategy.strategy_config.strategy_models.empty()) {
        auto model = get_or_load_model(
            strategy.strategy_config.strategy_models.front().path_to_model);
        if (model && features->size() == model->get_input_size()) {
            score = model->predict(*features);
        }
    }

    const long long now_ms = engine.get_time();
    for (auto& [symbol, ticker] : strategy.trade_tickers) {
        ticker->atomic_signal_main.store(score, now_ms);
    }
}
```

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
./build/bin/live_infra_tests "[state_machines],[paper_exchange],[paper_trading],[quote_holds],[normalized_trade],[lighter_market_data],[ohlcv]"
```

Run the bundled `sma_crossover` backtest:

```bash
python3 -m venv .venv && .venv/bin/pip install pyarrow
PYTHON=.venv/bin/python ./examples/quickstart_backtest/run.sh ./build/bin/backtest_runner
```

See [Quickstart](https://engine.keronis.ai/quickstart/), [Architecture](https://engine.keronis.ai/architecture/), and [Live & Paper Trading](https://engine.keronis.ai/live-and-paper/).

## Status and disclaimer

The full stack ships in this repository: C++20 engine, 260+ Catch2 tests, streaming feature state machines, ONNX Runtime inference, `backtest_runner`, `historical_feature_gen`, `PaperExchange`, `deploy/` PostgreSQL schema and seed templates, and `src/py/cpp_live_proxy` for CCXT exchanges, IBKR equities, and venue credentials.

Provided as-is under Apache-2.0 with no warranty. Automated trading carries risk of financial loss.

## License

Apache-2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE). MPFR and GMP are LGPL and are always linked as shared libraries.
