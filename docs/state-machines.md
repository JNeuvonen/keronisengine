# State Machines & ONNX

Streaming features are implemented as C++ subclasses of `MarketData::StrategyStateMachine`. The same class runs in `historical_feature_gen` (offline Parquet training sets), `backtest_runner`, and `live_infra`, avoiding feature drift between training and serving.

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

Seven reference state machines are included under `src/cpp/packages/historical_feature_gen/src/transformations/`: `spread_sm`, `taker_flow_sm`, `windowed_taker_flow_sm`, `bba_imbalance_ratio_sm`, `global_bba_sm`, `six_venue_order_sm`, and `polymarket_trade_flow_sm`.

## Live lifecycle hooks

- `on_trade_stream_disconnected(symbol, exchange_name, market_type)`: called from the stream health-check thread when a trade feed drops.
- `run_background_work()`: called roughly once per second from the syncer thread for slow work (such as REST catch-up) that must not block the event dispatcher.
- `serialize_state_impl()` / `deserialize_state_impl()`: optional binary state persistence across restarts.

## ONNX Runtime inference

`common/include/onnx_model.h` wraps ONNX Runtime C++ sessions for in-process scoring. Export a trained model to `.onnx`, reference it in `StrategyConfig` (`models` or `model_schedule_path`), and score the feature vector returned by your state machine inside `compute_predictions_callback`.
