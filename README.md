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
| `strategies/` | Bundled examples: `sma_crossover`, `test_quoter`, `test_taker`, `test_market_maker`, `discretionary` |

## Streaming features → ONNX → order

```cpp
#include "market_data/state_machine_dispatcher.h"
#include "market_making/strategy_helpers.h"
#include "market_making/strategy_registry.h"
#include "onnx_model.h"

namespace {
OnnxModel& model() {
    static OnnxModel instance("model.onnx");
    return instance;
}

std::vector<Core::StrategyDecision> on_market_event(
    Core::Engine& engine, Core::Strategy& strategy,
    std::optional<Core::MarketEvent>) {
    auto sm = MarketData::StateMachineDispatcher::get_instance().get_machine("taker_flow");
    auto features = sm ? sm->emit_state() : std::nullopt;
    if (!features) return {};

    const float score = model().predict(*features);
    std::vector<Core::StrategyDecision> decisions;
    for (auto& [symbol, ticker] : strategy.trade_tickers) {
        std::lock_guard<std::mutex> lock(ticker->trading_mutex);
        const auto& em = ticker->exchange_and_markets.front();
        const auto price = engine.get_price(symbol, em.exchange_name, em.market_type);
        if (!price || !ticker->get_orders(false).empty()) continue;

        Core::StrategyDecision decision;
        decision.symbol = symbol;
        const auto trades = ticker->get_trades(false);
        if (trades.empty() && score > 0.65f) {
            Core::StrategyHelpers::submit_market_order(
                engine, strategy, *ticker, Core::OrderSide::BUY, *price,
                mpfr::mpreal(0.01), decision);
        } else if (!trades.empty() && score < 0.45f) {
            Core::StrategyHelpers::close_trade_market(
                engine, strategy, *ticker, trades.front(), *price, decision);
        }
        if (!decision.orders.empty()) decisions.push_back(std::move(decision));
    }
    return decisions;
}

void bind_ml_strategy(Core::Strategy& strategy) {
    strategy.strategy_callback = on_market_event;
    strategy.compute_predictions_callback = [](Core::Engine&, Core::Strategy&) {};
}
}  // namespace

KERONIS_REGISTER_STRATEGY("ml_taker", bind_ml_strategy);
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
./build/bin/live_infra_tests "[paper_exchange],[paper_trading],[quote_holds],[normalized_trade],[lighter_market_data],[ohlcv]"
```

Run the bundled `sma_crossover` backtest:

```bash
python3 -m venv .venv && .venv/bin/pip install pyarrow
PYTHON=.venv/bin/python ./examples/quickstart_backtest/run.sh ./build/bin/backtest_runner
```

See [Quickstart](https://engine.keronis.ai/quickstart/), [Architecture](https://engine.keronis.ai/architecture/), and [Live & Paper Trading](https://engine.keronis.ai/live-and-paper/).

## Status and disclaimer

Core engine, unit tests, streaming state machines, ONNX inference, `backtest_runner`, `historical_feature_gen`, and `PaperExchange` run standalone today. Live execution requires PostgreSQL, NATS, and (currently) an external proxy (`CCXT_PROXY_BASE_URL`, not published in this repo) for credential lookup and non-native venues; a standalone file-based credential path is next.

Provided as-is under Apache-2.0 with no warranty. Automated trading carries risk of financial loss.

## License

Apache-2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE). MPFR and GMP are LGPL and are always linked as shared libraries.
