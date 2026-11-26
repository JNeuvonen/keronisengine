# KeronisEngine

[![CI](https://github.com/JNeuvonen/keronisengine/actions/workflows/ci.yml/badge.svg)](https://github.com/JNeuvonen/keronisengine/actions/workflows/ci.yml)
[![Docs](https://github.com/JNeuvonen/keronisengine/actions/workflows/pages.yml/badge.svg)](https://engine.keronis.ai/)
[![Docker](https://github.com/JNeuvonen/keronisengine/actions/workflows/docker.yml/badge.svg)](https://github.com/JNeuvonen/keronisengine/pkgs/container/keronisengine)
[![License: Apache-2.0](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)

**Process all of Binance's live market data — 10,000+ messages/sec with continuous streaming state-machine features and in-process ONNX predictions — on a single ~$100 dedicated server.**

KeronisEngine is an open-source C++20 trading engine built for live/backtest parity. Your strategy functions, streaming feature state machines, and ONNX models run identically in offline Parquet feature generation (`historical_feature_gen`), event-driven backtesting (`backtest_runner`), depth-based paper trading (`PaperExchange`), and live execution (`live_infra`).

**[Home & Docs (engine.keronis.ai)](https://engine.keronis.ai/)** · **[Quickstart](https://engine.keronis.ai/quickstart/)** · **[Architecture](https://engine.keronis.ai/architecture/)** · **[Comparison](https://engine.keronis.ai/comparison/)** · **[Agent Docs (`llms.txt`)](https://engine.keronis.ai/llms.txt)**

---

## Under the Hood

Most open-source trading frameworks either hide slow Python loops behind a fast core, or stop at toy order books. KeronisEngine is the actual C++20 production stack extracted with its plugin registries intact:

- **`RingBufferBook` (`trading_engine/.../ring_buffer_book.h`):** Flat tick-indexed ring buffer with compile-time-checked 16-byte POD slots (`tick_tag`, `updated_sec`, `qty`), an occupancy bitmap with `__builtin_ctzll` empty-word skipping for depth walks, lazy OS page commitment (`mmap` zero-pages until written), and runtime-configurable windows (19-bit for CEX books, 10-bit for Polymarket). Single-threaded `apply_bid` benchmarks at **~1.5 ns/op (~650M ops/s)** on an Intel Core Ultra 9 285K under strict IEEE-754 flags (`-fno-fast-math -ffp-contract=off -frounding-math`).
- **Zero train/serve feature skew:** Streaming C++ `StrategyStateMachine`s (7 bundled, including `six_venue_order`, `global_bba`, `taker_flow`, and `windowed_taker_flow`) run in `historical_feature_gen` to build Parquet training panels, then run the exact same C++ code live and in backtest to feed in-process ONNX Runtime inference.
- **Realistic fill simulation (`PaperExchange` & `BacktestState`):** `PaperExchange` evaluates every paper order as a taker at `ack + latency`, then fills the resting remainder as a maker against resting quotes and subsequent trade prints—with cancels immediately preventing later prints from filling. `BacktestState` also models queue-position notional (`order_queue_notional_accumulated`) for Polymarket books.
- **Crash-resilient live safety (`QuoteHolds`):** Quote pulls are persisted as durable state—not fire-and-forget commands—so an engine that crashes and restarts comes back with resting entry quotes still pulled. In production (`IS_PROD=1`), a stale `quote_guard` heartbeat automatically pulls resting quotes while still allowing closes and risk-reducing exits.
- **High-precision MPFR accounting & native venues:** Order quantities, fees, and realized PnL use a clean-room Apache-2.0 `mpreal` wrapper over dynamically linked MPFR/GMP. Native venue clients include Hyperliquid (with a bundled Rust signer crate), Aster, and Polymarket, plus Lighter L2 order-book streams.

---

## 60-Second Quickstart

### Option 1: Prebuilt Docker Image (GHCR)

```bash
# Run the core C++ test suite (225 test cases)
docker run --rm ghcr.io/jneuvonen/keronisengine:latest trading_engine_tests

# Run the bundled SMA crossover backtest on synthetic 1m BTCUSDT Parquet bars
docker run --rm ghcr.io/jneuvonen/keronisengine:latest \
  ./examples/quickstart_backtest/run.sh /src/build/bin/backtest_runner
```

*(Or build locally with `docker build -t keronisengine .`.)*

### Option 2: Native Build (Linux)

Requirements: CMake ≥ 3.24, C++20 compiler, Ninja, [vcpkg](https://github.com/microsoft/vcpkg), Rust toolchain (for `hyperliquid_signer`), and Python 3 with `pyarrow` (`pip install pyarrow` in a venv, for the quickstart data generator).

```bash
export VCPKG_ROOT=/path/to/vcpkg
ORT=$(./scripts/fetch_onnxruntime.sh)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DONNXRUNTIME_ROOT="$ORT"
cmake --build build -j"$(nproc)"

./build/bin/trading_engine_tests
./examples/quickstart_backtest/run.sh ./build/bin/backtest_runner
```

### Local Paper / Live Stack

Spin up localhost-bound PostgreSQL (auto-initialized with [deploy/schema.sql](deploy/schema.sql) and [deploy/seed_paper_strategy.sql](deploy/seed_paper_strategy.sql)) and NATS:

```bash
cd deploy
cp .env.example .env
cp accounts.example.json accounts.json
docker compose up -d
```

See [docs/live-and-paper.md](docs/live-and-paper.md) for running `live_infra` against `PaperExchange` or live venues.

---

## Writing a Strategy (Without Forking)

Drop a `.cpp` file into your own private directory and pass `-DKERONIS_STRATEGY_DIRS=/path/to/my/strategies` to CMake. Strategies and state machines self-register via static initializers—no engine files need editing.

Here is the core of the bundled runnable [sma_crossover.cpp](src/cpp/packages/strategies/sma_crossover/sma_crossover.cpp):

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
        const auto trades = ticker->get_trades(false);
        if (trades.empty() && crossed_up(strategy.strategy_config.id, symbol, *price)) {
            Core::StrategyHelpers::submit_market_order(
                engine, strategy, *ticker, Core::OrderSide::BUY, *price,
                mpfr::mpreal(0.01), decision);
        } else if (!trades.empty() && crossed_down(strategy.strategy_config.id, symbol, *price)) {
            Core::StrategyHelpers::close_trade_market(
                engine, strategy, *ticker, trades.front(), *price, decision);
        }
        if (!decision.orders.empty()) decisions.push_back(std::move(decision));
    }
    return decisions;
}

void bind_sma_crossover(Core::Strategy& strategy) {
    strategy.strategy_callback = on_market_event;
    strategy.compute_predictions_callback = [](Core::Engine&, Core::Strategy&) {};
    strategy.required_bar_types = {MarketData::BarClosePolicy::time_bars(60'000)};
}
}  // namespace

KERONIS_REGISTER_STRATEGY("sma_crossover", bind_sma_crossover);
```

---

## Packages

| Package | Role |
|---|---|
| `trading_engine` | Core `Engine`, `Strategy`, `TradeTicker`, `RingBufferBook`, risk caps, and `BacktestState` |
| `market_data` | OHLCV Parquet datasets, trades, `LighterOrderBook`, `DepthHistory`, and `StrategyStateMachine` |
| `live_infra` | Live runtime, `PaperExchange`, native clients (Hyperliquid, Aster, Polymarket), `QuoteHolds` |
| `backtest_runner` | Event-driven Parquet replay emitting `summary.json`, `trades.json`, and `equity_curve.json` |
| `historical_feature_gen` | Replays registered state machines over historical data to build ML training sets |
| `engine_control_server` | Localhost HTTP introspection and optional control API |
| `strategies/` | Bundled examples: `sma_crossover`, `test_quoter`, `test_taker`, `test_market_maker`, `discretionary` |

---

## Risk Disclaimer

KeronisEngine is provided **as-is** under the Apache-2.0 license with no warranty. Automated trading involves substantial risk of financial loss. Always validate strategies in `backtest_runner`, then `PaperExchange` / testnet with `IS_ORDER_CREATION_DISABLED=1`, before connecting live capital.

## License

Apache-2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE). MPFR and GMP are LGPL and are always linked dynamically as shared libraries.
