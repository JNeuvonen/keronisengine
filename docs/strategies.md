# Strategies

Strategies are plain C++ functions registered by name with `KERONIS_REGISTER_STRATEGY`. You keep your proprietary strategies in your own directory or private repo and compile them into KeronisEngine without editing any engine file.

## Building external strategies

Pass `-DKERONIS_STRATEGY_DIRS` (semicolon-separated) to CMake:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DONNXRUNTIME_ROOT="$ORT" \
  -DKERONIS_STRATEGY_DIRS="/path/to/my_strategies;/path/to/my_state_machines"
cmake --build build -j"$(nproc)"
```

All `.cpp` files in those directories are compiled into `keronis_engine` and linked with `WHOLE_ARCHIVE` into `backtest_runner`, `live_infra`, and `historical_feature_gen` so static registration macros are never stripped.

---

## Binance ONNX Perp

Source: [`src/cpp/packages/strategies/binance_onnx_perp/binance_onnx_perp.cpp`](https://github.com/JNeuvonen/keronisengine/blob/main/src/cpp/packages/strategies/binance_onnx_perp/binance_onnx_perp.cpp)

This flagship example shows the standard predictive-model loop on crypto perpetuals:

1. **Streaming features:** Reads rolling taker-flow features from the registered `windowed_taker_flow` state machine.
2. **In-process ONNX scoring:** Loads the `.onnx` artifact from `strategy.strategy_config.strategy_models`, calls `model->predict(*features)` in `compute_predictions_callback`, and publishes the latest score lock-free via `ticker->atomic_signal_main.store(score, now_ms)`.
3. **Risk-checked execution:** `on_market_event` reads `atomic_signal_main.load()`, enters long via `Core::StrategyHelpers::submit_market_order` when `score > kEnterThreshold`, and exits via `Core::StrategyHelpers::close_trade_market` when `score < kExitThreshold`.

Runs standalone in `backtest_runner` and `PaperExchange`; live Binance execution routes through the configured `CCXT_PROXY_BASE_URL`.

```cpp
void compute_predictions(Core::Engine& engine, Core::Strategy& strategy) {
    auto sm = MarketData::StateMachineDispatcher::get_instance().get_machine(
        "windowed_taker_flow");
    if (!sm) return;

    auto features = sm->emit_state();
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

---

## Polymarket Digital Options

Source: [`src/cpp/packages/strategies/polymarket_digital/polymarket_digital.cpp`](https://github.com/JNeuvonen/keronisengine/blob/main/src/cpp/packages/strategies/polymarket_digital/polymarket_digital.cpp)

This flagship example shows cross-venue digital options market making / high-frequency pricing on short-dated YES/NO binary markets:

1. **Cross-venue reference price:** Declares `binance:swap` in `strategy.signal_exchanges` and reads live spot via `engine.get_price(ticker->underlying_symbol, "binance", "swap")`.
2. **Fair probability:** Computes `P(S_T > K)` from spot, `ticker->strike_price`, and remaining time to `ticker->event_end_time_ms` (replace `fair_prob_yes()` with `OnnxModel::predict()` to plug in an ML probability model).
3. **Native CLOB execution:** Iterates outcomes (`0 = Yes`, `1 = No`), checks `get_outcome_exchange_inventory(outcome)` and `read_outcome_bba(outcome)`, rounds to `order_price_min_tick_size`, and submits a risk-checked `LIMIT` buy tagged with `o.is_prediction_market_order = true`, `o.clob_id`, and `o.outcome_name`.

```cpp
const double p_yes =
    fair_prob_yes(*spot, ticker->strike_price, remaining_ms / 1000.0);
const double fair[2] = {p_yes, 1.0 - p_yes};
const double tick = ticker->order_price_min_tick_size.load();

for (int outcome = 0; outcome < 2; ++outcome) {
    if (ticker->get_outcome_exchange_inventory(outcome) >= kMaxOutcomePos) continue;
    const auto bba = ticker->read_outcome_bba(outcome);
    if (!bba || bba->best_ask <= 0.0) continue;

    const double edge = fair[outcome] - bba->best_ask;
    if (edge < kMinEdge) continue;

    // Construct risk-checked LIMIT order, set clob_id + outcome_name, and mark for submission.
}
```

---

## Quickstart example: SMA crossover

Source: [`src/cpp/packages/strategies/sma_crossover/sma_crossover.cpp`](https://github.com/JNeuvonen/keronisengine/blob/main/src/cpp/packages/strategies/sma_crossover/sma_crossover.cpp)

A self-contained 5/20 moving-average crossover on 1-minute bars used by `examples/quickstart_backtest/run.sh`. It requires no external model or state machine, making it the fastest smoke test for verifying a fresh build.

## How the callbacks work

- `strategy.strategy_callback`: invoked on market events and periodic state polls. Returns a vector of `Core::StrategyDecision` containing new orders and cancellations.
- `strategy.compute_predictions_callback`: invoked to pull streaming features from a registered `StrategyStateMachine`, score ONNX models, and write lock-free signals (`atomic_signal_main`).
- `strategy.required_bar_types`: declares which bar intervals (`BarClosePolicy`) the strategy needs.
- `strategy.signal_exchanges`: declares additional venues whose market data the strategy reads without trading on them (e.g. reading Binance perp prices while trading Polymarket).

## Rules to follow

- **Lock the ticker:** Always hold `std::lock_guard<std::mutex> lock(ticker->trading_mutex)` before reading `get_orders(false)` / `get_trades(false)` or constructing orders. For prediction-market strategies with dynamic markets, also hold `strategy.lock_on_pred_market_strat()` while iterating `strategy.trade_tickers`.
- **Always use `construct_order` (or `StrategyHelpers`):** `construct_order` enforces directional exposure caps, order-rate limits, reduce-only mode, and quote-pull gates automatically.
- **Quote holds (`quotes_pulled`):** When a quote hold is active (manual pull, stale `quote_guard` heartbeat, or `SIGTERM` shutdown), resting `LIMIT` / `LIMIT_MAKER` entry orders are refused and active resting entries are canceled, while close orders and `IOC`/`FOK`/`MARKET` orders are still allowed.

## Bundled reference strategies

- **`binance_onnx_perp`**: Binance perpetuals predictive-model strategy (`WindowedTakerFlowSM` → `OnnxModel::predict()` → `Core::StrategyHelpers`).
- **`polymarket_digital`**: Polymarket short-dated digital options strategy (Binance reference price → fair probability → native Polymarket CLOB execution).
- **`sma_crossover`**: 5/20 moving-average crossover on 1-minute bars (used by the quickstart backtest).
- **`test_quoter`**, **`test_taker`**, **`test_market_maker`**: Minimal order-routing test harnesses.
- **`discretionary`**: Command-driven execution with stops and take-profits.
