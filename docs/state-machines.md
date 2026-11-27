# State Machines & ONNX

Streaming features are implemented as C++ subclasses of `MarketData::StrategyStateMachine`. The same compiled class runs in `historical_feature_gen` (offline Parquet training panels) and `live_infra` (paper and live execution via `StateMachineDispatcher` and `MD_STATE_MACHINES`), eliminating feature-code drift between offline training and live serving. (`backtest_runner` consumes `OhlcvDataset` bars, order books, and precomputed `market_data.predictions` generated from those panels.)

## Bundled teaching state machines

Two clean, unit-tested reference state machines are included under `src/cpp/packages/historical_feature_gen/src/transformations/` (verified in `test_example_state_machines.cpp`):

- **`windowed_taker_flow`** (`windowed_taker_flow_sm.cpp`): Maintains a 1-second ring buffer of taker buy/sell notional and trade counts per configured `symbol_keys` (format `SYMBOL:exchange:market_type`, e.g. `BTCUSDT:binance:swap`) across configurable `windows_sec` (e.g. `[5, 30, 300]`). Emits `buy_ratio`, `trade_count`, `buy_volume`, `sell_volume` per window plus cross-window volume ratios. Used by the `binance_onnx_perp` example strategy.
- **`spread`** (`spread_sm.cpp`): Minimal BBA spread tracker (`ask - bid`) per configured symbol key. Guards `emit_state_impl()` with `has_bba` so it returns `std::nullopt` until every configured symbol has received a valid BBA quote—demonstrating safe warmup handling.

## Implementing and registering a state machine

Subclass `MarketData::StrategyStateMachine`, override `initialize_symbol_states()`, `react_impl()`, and `emit_state_impl()`, and register the type with `KERONIS_REGISTER_STATE_MACHINE(name, Type)`:

```cpp
#include "market_data/state_machine_registry.h"
#include "market_data/strategy_state_machine.h"

namespace {

class MyFeatureSM : public MarketData::StrategyStateMachine {
   public:
    std::string get_name() const override { return "my_features"; }

    std::vector<std::string> feature_names() const override {
        return {"last_price"};
    }

    void initialize_symbol_states() override { last_price_ = 0.0f; }

   protected:
    void react_impl(const MarketData::MarketEvent& event) override {
        if (event.type == MarketData::MarketEvent::Type::TRADE &&
            event.trade.has_value()) {
            last_price_ = static_cast<float>(event.trade->price);
        }
    }

    std::optional<std::vector<float>> emit_state_impl() const override {
        if (last_price_ <= 0.0f) return std::nullopt;
        return std::vector<float>{last_price_};
    }

   private:
    float last_price_ = 0.0f;
};

}  // namespace

KERONIS_REGISTER_STATE_MACHINE("my_features", MyFeatureSM);
```

Compile your private state machines into the engine without forking by passing their directory in `-DKERONIS_STRATEGY_DIRS`.

## Live lifecycle hooks

- `on_trade_stream_disconnected(symbol, exchange_name, market_type)`: called from the stream health-check thread when a trade feed drops.
- `run_background_work()`: called roughly once per second from the syncer thread for slow work (such as REST catch-up) that must not block the event dispatcher.
- `serialize_state_impl()` / `deserialize_state_impl()`: optional binary state persistence across restarts.

## ONNX Runtime inference

`common/include/onnx_model.h` wraps ONNX Runtime C++ sessions for in-process scoring. Export a trained model to `.onnx`, reference it in `StrategyConfig` (`strategy_models` or `model_schedule_path`), and score the feature vector returned by your state machine inside `compute_predictions_callback`, publishing the result lock-free via `ticker->atomic_signal_main.store(score, now_ms)`. See [Binance ONNX Perp](strategies.md#binance-onnx-perp) for a complete compiled example.
