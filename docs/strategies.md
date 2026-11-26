# Strategies

Strategies are C++ functions registered by name with `KERONIS_REGISTER_STRATEGY`. You do not need to modify engine source files to add a strategy.

## Compiling external strategies

Keep your strategy sources in a separate directory or repository and pass `-DKERONIS_STRATEGY_DIRS` to CMake:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DONNXRUNTIME_ROOT="$ORT" \
  -DKERONIS_STRATEGY_DIRS="/path/to/my_strategies;/path/to/my_state_machines"
cmake --build build -j"$(nproc)"
```

CMake globs all `.cpp` files under `KERONIS_STRATEGY_DIRS` into `keronis_engine`, which is linked with `WHOLE_ARCHIVE` into `backtest_runner`, `live_infra`, and `historical_feature_gen` so static initializers are preserved.

## Strategy structure

A binder function attaches callbacks and bar requirements to a `Core::Strategy` instance:

- `strategy.strategy_callback`: called on market events or periodic state polls; returns a vector of `Core::StrategyDecision` (orders to submit or cancel).
- `strategy.compute_predictions_callback`: called before decision evaluation to run state-machine reads and ONNX model scoring.
- `strategy.required_bar_types`: list of `MarketData::BarClosePolicy` (for example `MarketData::BarClosePolicy::time_bars(60'000)`).

For simple market entries and exits, `market_making/strategy_helpers.h` wraps `CreateOrderParams`, `construct_order`, and `mark_order_to_be_created`. See `src/cpp/packages/strategies/sma_crossover/sma_crossover.cpp` for a complete runnable example.

## Locking and order lifecycle

1. Always acquire `std::lock_guard<std::mutex> lock(ticker_ptr->trading_mutex)` before inspecting `get_orders(false)` / `get_trades(false)` or constructing orders.
2. Always construct orders through `ticker_ptr->construct_order(engine, strategy, params)` so exposure caps, order-rate limits, reduce-only rules, and quote-pull gates are applied uniformly.
3. When `strategy.quotes_pulled` is true (set via `QuotesPullReason::PULL_SIGNAL`, `GUARD_STALE`, or `SHUTDOWN`), resting `LIMIT` and `LIMIT_MAKER` entry orders are refused and active resting entries are canceled. Close orders and `IOC`/`FOK`/`MARKET` orders remain permitted.

## Bundled reference strategies

- `sma_crossover`: 5/20 moving-average crossover on 1-minute bars (used by `examples/quickstart_backtest/`).
- `test_quoter`: posts a single resting limit bid one tick below best bid.
- `test_taker`: sends market buy orders against best ask.
- `test_market_maker`: posts two-sided post-only quotes around BBA.
- `discretionary`: command-driven execution with stop-loss and take-profit management.
