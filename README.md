# KeronisEngine

An open-source C++ trading engine with prediction models in the loop. One
strategy class runs against live venues and in the backtester with the same
code, and its state machines and ONNX models produce features and predictions
at the same timestamps in both.

> **Status: pre-release.** The engine, strategy registry, state machines and
> backtester build and pass their tests. Live trading still expects the
> maintainers' own config and credential setup; a file-based setup with a
> local Postgres and docker-compose is next.

## What's here

| Package | What it does |
|---|---|
| `trading_engine` | Strategies, tickers, order and trade lifecycle, inventory, backtest state |
| `market_data` | Bars, trades, order books, state machines (streaming feature computation) |
| `live_infra` | The live process; native venue clients for Hyperliquid, Aster and Polymarket (other venues through a proxy, not yet published) |
| `backtest_runner` | Event-driven replay of recorded market data through the same strategies |
| `historical_feature_gen` | Runs state machines over history to build training sets |
| `engine_control_server` | HTTP control and introspection of a running engine |
| `strategies/` | Example strategies: `test_quoter`, `test_taker`, `test_market_maker`, `discretionary` |

## Build

Requirements: CMake ≥ 3.24, a C++20 compiler, Ninja, [vcpkg](https://github.com/microsoft/vcpkg)
and a Rust toolchain (for the Hyperliquid signer).

```bash
export VCPKG_ROOT=/path/to/vcpkg
ORT=$(./scripts/fetch_onnxruntime.sh)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
  -DONNXRUNTIME_ROOT=$ORT
cmake --build build
./build/bin/trading_engine_tests
```

MPFR and GMP are LGPL and are always linked as shared libraries.

## Writing a strategy

A strategy registers itself by name; the engine never needs editing. See
[test_taker](src/cpp/packages/strategies/test_taker) for a complete one.

```cpp
#include "market_making/strategy_registry.h"

namespace {
std::vector<Core::StrategyDecision> on_market_event(
        Core::Engine& engine, Core::Strategy& strategy,
        std::optional<Core::MarketEvent> event) {
    return {};  // post and cancel orders here
}

void bind_my_strategy(Core::Strategy& strategy) {
    strategy.strategy_callback = on_market_event;
}
}  // namespace

KERONIS_REGISTER_STRATEGY("my-strategy", bind_my_strategy);
```

Point `-DKERONIS_STRATEGY_DIRS=/path/to/my/strategies` at your own sources to
build them into the engine without forking this repository. State machines
register the same way with `KERONIS_REGISTER_STATE_MACHINE`.

## License

Apache-2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE).
