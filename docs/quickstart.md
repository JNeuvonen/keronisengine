# Quickstart

Get KeronisEngine built, run the unit test suite, and execute your first end-to-end backtest in minutes.

## Option 1: Docker (Easiest)

The repository includes a self-contained `Dockerfile` (Ubuntu 24.04 + vcpkg + Rust + ONNX Runtime):

```bash
docker build -t keronisengine .

# Run core engine tests
docker run --rm keronisengine trading_engine_tests

# Run the bundled SMA crossover backtest
docker run --rm keronisengine ./examples/quickstart_backtest/run.sh /src/build/bin/backtest_runner
```

## Option 2: Native Build (Linux)

### Prerequisites

- Linux (Ubuntu 24.04 recommended; `backtest_runner` and `live_infra` target POSIX/Linux)
- CMake ≥ 3.24, Ninja, C++20 compiler (GCC 13+ or Clang 17+)
- [vcpkg](https://github.com/microsoft/vcpkg)
- Rust toolchain (`cargo`) for `src/rust/hyperliquid_signer`
- `python3` + `pyarrow` (`python3 -m venv .venv && .venv/bin/pip install pyarrow`) for generating the quickstart sample Parquet data

### Build

```bash
export VCPKG_ROOT=/path/to/vcpkg
ORT=$(./scripts/fetch_onnxruntime.sh)

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DONNXRUNTIME_ROOT="$ORT"

cmake --build build -j"$(nproc)"
```

This produces four binaries under `build/bin/`:

| Binary | Purpose |
|---|---|
| `backtest_runner` | Event-driven historical replay |
| `live_infra` | Live and paper trading runtime |
| `historical_feature_gen` | Offline state-machine feature generation |
| `export_strategy_config` | Export a live DB strategy row to backtest `config.json` |

### Run Tests

```bash
# Core trading engine suite
./build/bin/trading_engine_tests

# Offline live_infra unit tests (PaperExchange, quote holds, Lighter book, etc.)
./build/bin/live_infra_tests "[paper_exchange],[paper_trading],[quote_holds],[normalized_trade],[lighter_market_data],[ohlcv]"
```

### Run the Quickstart Backtest

`examples/quickstart_backtest/run.sh` generates 6 hours of synthetic 1-minute `BTCUSDT` Parquet bars and runs the bundled `sma_crossover` strategy through `backtest_runner`:

```bash
./examples/quickstart_backtest/run.sh ./build/bin/backtest_runner
```

Expected output includes periodic NAV/PnL snapshots and a final summary similar to:

```text
[BACKTEST 2026-01-01 05:59:59.999UTC] NAV: 100084 | PnL: +79.7 | Fees: 2.5 | MaxDD: 0.00% | Trades: 6 (W:5 L:1 83%) | ...
```

## Next Steps

- Understand the [Architecture](architecture.md)
- Write your own strategy in [Writing a Strategy](strategies.md)
- Spin up Postgres + NATS for [Live & Paper Trading](live-and-paper.md)
