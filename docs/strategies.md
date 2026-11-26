# Writing a Strategy

Strategies self-register by name with `KERONIS_REGISTER_STRATEGY`. You do **not** need to edit any engine source file to add a strategy.

## External Strategy Directories

Keep your strategies in your own private repository and point CMake at them:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DONNXRUNTIME_ROOT="$ORT" \
  -DKERONIS_STRATEGY_DIRS="/path/to/my_strategies;/path/to/my_state_machines"
cmake --build build -j"$(nproc)"
```

Every `.cpp` under `KERONIS_STRATEGY_DIRS` is compiled into `keronis_engine` and linked with `WHOLE_ARCHIVE` into `backtest_runner`, `live_infra`, and `historical_feature_gen`.

## Anatomy of a Strategy

See [`src/cpp/packages/strategies/sma_crossover/sma_crossover.cpp`](https://github.com/JNeuvonen/keronisengine/blob/main/src/cpp/packages/strategies/sma_crossover/sma_crossover.cpp) for a complete runnable strategy. The key hooks are:

```cpp
#include "market_making/strategy_registry.h"
#include "market_making/engine.h"
#include "market_making/strategy.h"
#include "market_making/ticker.h"

namespace {

std::vector<Core::StrategyDecision> on_market_event(
    Core::Engine& engine,
    Core::Strategy& strategy,
    std::optional<Core::MarketEvent> event) {
    std::vector<Core::StrategyDecision> decisions;
    const long long now_ms = engine.get_time();

    for (auto& [symbol, ticker_ptr] : strategy.trade_tickers) {
        std::lock_guard<std::mutex> lock(ticker_ptr->trading_mutex);
        if (!ticker_ptr->get_orders(false).empty()) continue;

        const auto& em = ticker_ptr->exchange_and_markets.front();
        const auto price = engine.get_price(symbol, em.exchange_name, em.market_type);
        if (!price.has_value()) continue;

        // Build Core::CreateOrderParams, call ticker_ptr->construct_order(...),
        // then ticker_ptr->mark_order_to_be_created(engine, order, decision, strategy).
    }
    return decisions;
}

void bind_my_strategy(Core::Strategy& strategy) {
    strategy.strategy_callback = on_market_event;
    strategy.compute_predictions_callback = [](Core::Engine&, Core::Strategy&) {};
}

}  // namespace

KERONIS_REGISTER_STRATEGY("my_strategy", bind_my_strategy);
```

## Key Conventions

- **Locking:** Always lock `ticker_ptr->trading_mutex` before inspecting `get_orders(false)` / `get_trades(false)` or calling `construct_order`.
- **Order construction:** Always go through `ticker_ptr->construct_order(engine, strategy, params)` so pre-trade risk limits, order-rate caps, reduce-only checks, and quote-pull gates are enforced consistently.
- **Quote holds (`quotes_pulled`):** When `strategy.quotes_pulled` is set (via `PULL_SIGNAL`, `GUARD_STALE`, or `SHUTDOWN`), resting `LIMIT` / `LIMIT_MAKER` entry orders are refused and existing resting entries are swept/canceled, while close orders and `IOC`/`FOK`/`MARKET` orders remain allowed.
- **Bundled reference strategies:**
  - `sma_crossover` — bar-driven moving average crossover (used by the quickstart backtest)
  - `test_quoter` — resting limit bid quoting against BBA
  - `test_taker` — market taker flow against BBA
  - `test_market_maker` — two-sided post-only quoting around BBA
  - `discretionary` — command-driven discretionary execution with stops and take-profits
