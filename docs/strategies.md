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

## Complete example: SMA crossover

Here is a complete, working 5/20 moving-average crossover strategy (adapted from `src/cpp/packages/strategies/sma_crossover/sma_crossover.cpp`). It runs identically in `backtest_runner` and `live_infra`:

```cpp
#include "market_making/strategy_helpers.h"
#include "market_making/strategy_registry.h"
#include "market_data/bar_close_policy.h"

#include <deque>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace {

constexpr size_t kFast = 5, kSlow = 20;
constexpr long long kBarMs = 60'000;

struct Window {
    long long last_ms = -1;
    std::deque<double> closes;
    std::optional<double> prev_fast, prev_slow;
};

std::mutex g_mu;
std::unordered_map<std::string, Window> g_windows;

double mean_last(const std::deque<double>& v, size_t n) {
    double s = 0.0;
    for (size_t i = v.size() - n; i < v.size(); ++i) s += v[i];
    return s / static_cast<double>(n);
}

std::vector<Core::StrategyDecision> on_market_event(
    Core::Engine& engine, Core::Strategy& strategy,
    std::optional<Core::MarketEvent>) {
    std::vector<Core::StrategyDecision> decisions;
    const long long now_ms = engine.get_time();

    for (auto& [symbol, ticker] : strategy.trade_tickers) {
        std::lock_guard<std::mutex> lock(ticker->trading_mutex);
        if (ticker->exchange_and_markets.empty()) continue;

        const auto& em = ticker->exchange_and_markets.front();
        const auto price = engine.get_price(symbol, em.exchange_name, em.market_type);
        if (!price || *price <= 0.0) continue;

        bool crossed_up = false, crossed_down = false;
        {
            std::lock_guard<std::mutex> hlock(g_mu);
            auto& w = g_windows[std::to_string(strategy.strategy_config.id) + ":" + symbol];
            if (w.last_ms >= 0 && now_ms - w.last_ms < kBarMs) continue;
            w.last_ms = now_ms;
            w.closes.push_back(*price);
            if (w.closes.size() > kSlow) w.closes.pop_front();
            if (w.closes.size() < kSlow) continue;

            const double fast = mean_last(w.closes, kFast);
            const double slow = mean_last(w.closes, kSlow);
            if (w.prev_fast && w.prev_slow) {
                crossed_up = (*w.prev_fast <= *w.prev_slow) && (fast > slow);
                crossed_down = (*w.prev_fast >= *w.prev_slow) && (fast < slow);
            }
            w.prev_fast = fast;
            w.prev_slow = slow;
        }

        if (!ticker->get_orders(false).empty()) continue;
        const auto trades = ticker->get_trades(false);

        Core::StrategyDecision decision;
        decision.symbol = symbol;
        if (trades.empty() && crossed_up) {
            Core::StrategyHelpers::submit_market_order(
                engine, strategy, *ticker, Core::OrderSide::BUY, *price,
                mpfr::mpreal(0.01), decision);
        } else if (!trades.empty() && crossed_down) {
            Core::StrategyHelpers::close_trade_market(
                engine, strategy, *ticker, trades.front(), *price, decision);
        }
        if (!decision.orders.empty()) decisions.push_back(std::move(decision));
    }
    return decisions;
}

void bind_sma_crossover(Core::Strategy& strategy) {
    strategy.strategy_callback = on_market_event;
    strategy.compute_predictions_callback = [](Core::Engine&, Core::Strategy&) {};
    strategy.required_bar_types = {MarketData::BarClosePolicy::time_bars(kBarMs)};
}

}  // namespace

KERONIS_REGISTER_STRATEGY("sma_crossover", bind_sma_crossover);
```

## How the callbacks work

- `strategy.strategy_callback`: invoked on market events and periodic state polls. Returns a vector of `Core::StrategyDecision` containing new orders and cancellations.
- `strategy.compute_predictions_callback`: invoked before decision evaluation to pull streaming features from a registered `StrategyStateMachine` and score ONNX models.
- `strategy.required_bar_types`: declares which bar intervals (`BarClosePolicy`) the strategy needs.

## Rules to follow

- **Lock the ticker:** Always hold `std::lock_guard<std::mutex> lock(ticker->trading_mutex)` before reading `get_orders(false)` / `get_trades(false)` or constructing orders.
- **Always use `construct_order` (or `StrategyHelpers`):** `construct_order` enforces directional exposure caps, order-rate limits, reduce-only mode, and quote-pull gates automatically.
- **Quote holds (`quotes_pulled`):** When a quote hold is active (manual pull, stale `quote_guard` heartbeat, or `SIGTERM` shutdown), resting `LIMIT` / `LIMIT_MAKER` entry orders are refused and active resting entries are canceled, while close orders and `IOC`/`FOK`/`MARKET` orders are still allowed.

## Bundled reference strategies

- `sma_crossover`: 5/20 moving-average crossover on 1-minute bars (used by the quickstart backtest).
- `test_quoter`: posts a resting limit bid one tick below the best bid/ask (BBA).
- `test_taker`: sends market buy orders against the best ask.
- `test_market_maker`: posts two-sided post-only quotes around the BBA.
- `discretionary`: command-driven execution with stops and take-profits.
