# Quickstart

## Prerequisites (Linux)

- CMake >= 3.24, Ninja, C++20 compiler (GCC 13+ or Clang 17+)
- [vcpkg](https://github.com/microsoft/vcpkg)
- Rust toolchain (`cargo`) for `src/rust/hyperliquid_signer`
- Python 3 with `pyarrow` in a virtualenv (only needed for the quickstart sample-data generator)

## Build from source

```bash
export VCPKG_ROOT=/path/to/vcpkg
ORT=$(./scripts/fetch_onnxruntime.sh)

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DONNXRUNTIME_ROOT="$ORT"

cmake --build build -j"$(nproc)"
```

Build outputs under `build/bin/`:

| Binary | Purpose |
|---|---|
| `backtest_runner` | Event-driven historical replay from a JSON config |
| `live_infra` | Live and paper trading process |
| `historical_feature_gen` | Offline state-machine feature generation |
| `export_strategy_config` | Export a strategy row from PostgreSQL to backtest `config.json` |
| `trading_engine_tests` | Core engine Catch2 test suite |
| `live_infra_tests` | Live infra, `PaperExchange`, `QuoteHolds`, and market-data Catch2 tests |

## Run tests

```bash
./build/bin/trading_engine_tests
./build/bin/live_infra_tests "[paper_exchange],[paper_trading],[quote_holds],[normalized_trade],[lighter_market_data],[ohlcv]"
```

## Run the example backtest

`examples/quickstart_backtest/run.sh` generates 6 hours of synthetic 1-minute `BTCUSDT` Parquet bars under a temporary directory and runs the bundled `sma_crossover` strategy through `backtest_runner`:

```bash
python3 -m venv .venv
.venv/bin/pip install pyarrow
PYTHON=.venv/bin/python ./examples/quickstart_backtest/run.sh ./build/bin/backtest_runner
```

`backtest_runner` writes `summary.json`, `trades.json`, and `equity_curve.json` to `output_dir` and prints periodic NAV/PnL lines:

```text
[BACKTEST 2026-01-01 05:59:59.999UTC] NAV: 100087 | PnL: +82.5 | Fees: 2.1 | MaxDD: 0.00% | Trades: 5 (W:5 L:0 100%) | ...
[backtest_runner] dumped 5 trades to .../results/trades.json
[backtest_runner] dumped summary to .../results/summary.json
[backtest_runner] dumped equity curve to .../results/equity_curve.json
```

## Docker build

```bash
docker build -t keronisengine .
docker run --rm keronisengine trading_engine_tests
docker run --rm keronisengine ./examples/quickstart_backtest/run.sh /src/build/bin/backtest_runner
```

Note: building the Docker image from scratch compiles Arrow, Parquet, and vcpkg dependencies and takes 30+ minutes on a cold cache.
