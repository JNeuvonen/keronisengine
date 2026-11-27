# Quickstart

## Native build (Linux)

### Prerequisites

- CMake >= 3.24, Ninja, C++20 compiler (GCC 13+ or Clang 17+)
- [vcpkg](https://github.com/microsoft/vcpkg)
- Rust toolchain (`cargo`) for the Hyperliquid order signer crate (`src/rust/hyperliquid_signer`)
- Python 3 with `pyarrow` in a virtualenv (only used by `examples/quickstart_backtest/generate_sample_data.py`)

### Compile & test

With `VCPKG_ROOT` exported (or system dependencies installed), `make test` fetches ONNX Runtime, configures CMake/Ninja, compiles all six binaries, and runs the unit tests:

```bash
export VCPKG_ROOT=/path/to/vcpkg
make test
```

Or invoke CMake directly:

```bash
export VCPKG_ROOT=/path/to/vcpkg
ORT=$(./scripts/fetch_onnxruntime.sh)

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DONNXRUNTIME_ROOT="$ORT"

cmake --build build -j"$(nproc)"
```

This builds six binaries under `build/bin/`:

| Binary | Purpose |
|---|---|
| `backtest_runner` | Event-driven historical replay from a JSON config |
| `live_infra` | Live and paper trading runtime |
| `historical_feature_gen` | Offline state-machine feature generator |
| `export_strategy_config` | Export a live PostgreSQL strategy row to backtest `config.json` |
| `trading_engine_tests` | Core engine Catch2 unit tests |
| `live_infra_tests` | Live infra, `PaperExchange`, `QuoteHolds`, and market-data tests |

### Run tests

```bash
# Run both suites via Makefile
make test

# Or invoke the binaries directly:
# Core trading engine suite (225 test cases)
./build/bin/trading_engine_tests

# Offline live_infra unit tests (42 test cases)
./build/bin/live_infra_tests "[paper_exchange],[paper_trading],[quote_holds],[normalized_trade],[lighter_market_data],[ohlcv],[state_machines]"
```

Why the tag filter on `live_infra_tests`? The unfiltered binary also contains live-network and exchange-testnet integration tests that require API keys and live WebSocket connections.

### Run the example backtest

`examples/quickstart_backtest/run.sh` generates 6 hours of synthetic 1-minute `BTCUSDT` Parquet bars and runs `sma_crossover` through `backtest_runner`:

```bash
python3 -m venv .venv
.venv/bin/pip install pyarrow
PYTHON=.venv/bin/python ./examples/quickstart_backtest/run.sh ./build/bin/backtest_runner
```

Output includes periodic NAV/PnL lines and dumps `summary.json`, `trades.json`, and `equity_curve.json`:

```text
[BACKTEST 2026-01-01 05:59:59.999UTC] NAV: 100087 | PnL: +82.5 | Fees: 2.1 | MaxDD: 0.00% | Trades: 5 (W:5 L:0 100%) | ...
[backtest_runner] dumped 5 trades to .../results/trades.json
[backtest_runner] dumped summary to .../results/summary.json
[backtest_runner] dumped equity curve to .../results/equity_curve.json
```
