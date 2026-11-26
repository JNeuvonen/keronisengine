# State Machines & ONNX Inference

To eliminate train/serve skew, KeronisEngine computes streaming features inside C++ `MarketData::StrategyStateMachine` classes that run identically in:

1. `historical_feature_gen` (offline training panel generation)
2. `backtest_runner` (historical strategy replay)
3. `live_infra` (live and paper trading)

## Registering a State Machine

Subclass `MarketData::StrategyStateMachine`, implement its required virtual methods (`initialize_symbol_states`, `react_impl`, `emit_state_impl`), and register the type with `KERONIS_REGISTER_STATE_MACHINE(name, Type)`:

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

See the bundled state machines under `src/cpp/packages/historical_feature_gen/src/transformations/` (`spread_sm.cpp`, `taker_flow_sm.cpp`, `windowed_taker_flow_sm.cpp`, `bba_imbalance_ratio_sm.cpp`, `global_bba_sm.cpp`, `six_venue_order_sm.cpp`, `polymarket_trade_flow_sm.cpp`) for complete examples.

## Lifecycle Hooks

`MarketData::StrategyStateMachine` provides hooks for live resilience:

- `on_trade_stream_disconnected(symbol, exchange_name, market_type)` — invoked by the stream health monitor when a trade feed drops.
- `run_background_work()` — called approximately once per second from the syncer thread for slow work (such as REST catch-up) that must not block the event dispatcher.

## ONNX Model Inference

`common/include/onnx_model.h` wraps ONNX Runtime C++ sessions for low-latency in-process inference:

- Export your trained model (LightGBM, XGBoost, PyTorch, etc.) to `.onnx`.
- Configure `models` or `model_schedule_path` on your `StrategyConfig`.
- Score the feature vector produced by your `StrategyStateMachine` inside `compute_predictions_callback` or `on_market_event`.
