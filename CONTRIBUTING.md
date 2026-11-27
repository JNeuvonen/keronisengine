# Contributing to KeronisEngine

Thanks for your interest in contributing to KeronisEngine!

## Getting Started

1. Fork the repository and create a branch from `main`.
2. Build the project and verify tests pass (see [docs/quickstart.md](docs/quickstart.md)):
   ```bash
   cmake --build build -j"$(nproc)"
   ./build/bin/trading_engine_tests
   ./build/bin/live_infra_tests "[state_machines],[paper_exchange],[paper_trading],[quote_holds],[normalized_trade],[lighter_market_data],[ohlcv]"
   ./examples/quickstart_backtest/run.sh ./build/bin/backtest_runner
   ```
3. Run the public-tree hygiene check before opening a PR:
   ```bash
   ./scripts/check_public_tree.sh
   ```

## Guidelines

- **Licensing:** Contributions are licensed under Apache-2.0. Do not introduce GPL-licensed code or statically linked LGPL dependencies (MPFR/GMP must remain shared libraries).
- **Determinism:** Keep floating-point and `mpreal` price/PnL calculations deterministic; do not enable `-ffast-math`.
- **No secrets or binaries:** Never commit private keys, credentials, local absolute paths, or prebuilt binary artifacts.
- **Tests:** Add Catch2 unit tests for engine, market-data, or execution changes.
