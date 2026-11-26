#include "market_data/state_machine_dispatcher.h"
#include "market_making/strategy_helpers.h"
#include "market_making/strategy_registry.h"
#include "onnx_model.h"

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace {

// Educational Binance perpetuals predictive-model strategy.
// Reads rolling taker-flow features from WindowedTakerFlowSM, scores an ONNX
// model in compute_predictions_callback, and executes entries/exits in
// on_market_event. Backtests and PaperExchange run standalone; live Binance
// execution routes through the configured CCXT proxy.
constexpr double kOrderQty = 0.01;
constexpr float kEnterThreshold = 0.65f;
constexpr float kExitThreshold = 0.45f;

std::mutex g_models_mu;
std::unordered_map<std::string, std::shared_ptr<OnnxModel>> g_models;

std::shared_ptr<OnnxModel> get_or_load_model(const std::string& path) {
    if (path.empty()) return nullptr;
    std::lock_guard<std::mutex> lock(g_models_mu);
    auto& slot = g_models[path];
    if (!slot) slot = std::make_shared<OnnxModel>(path.c_str());
    return slot;
}

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

std::vector<Core::StrategyDecision> on_market_event(
    Core::Engine& engine, Core::Strategy& strategy,
    std::optional<Core::MarketEvent>) {
    std::vector<Core::StrategyDecision> decisions;

    for (auto& [symbol, ticker] : strategy.trade_tickers) {
        std::lock_guard<std::mutex> lock(ticker->trading_mutex);
        if (!ticker->get_orders(false).empty()) continue;
        if (ticker->exchange_and_markets.empty()) continue;

        const auto [score, signal_ts] = ticker->atomic_signal_main.load();
        if (signal_ts <= 0) continue;

        const auto& em = ticker->exchange_and_markets.front();
        const auto price =
            engine.get_price(symbol, em.exchange_name, em.market_type);
        if (!price || *price <= 0.0) continue;

        Core::StrategyDecision decision;
        decision.symbol = symbol;
        const auto trades = ticker->get_trades(false);

        if (trades.empty() && score > kEnterThreshold) {
            Core::StrategyHelpers::submit_market_order(
                engine, strategy, *ticker, Core::OrderSide::BUY, *price,
                mpfr::mpreal(kOrderQty), decision);
        } else if (!trades.empty() && score < kExitThreshold) {
            Core::StrategyHelpers::close_trade_market(
                engine, strategy, *ticker, trades.front(), *price, decision);
        }

        if (!decision.orders.empty()) decisions.push_back(std::move(decision));
    }

    return decisions;
}

void bind_binance_onnx_perp(Core::Strategy& strategy) {
    strategy.strategy_callback = on_market_event;
    strategy.compute_predictions_callback = compute_predictions;
}

}  // namespace

KERONIS_REGISTER_STRATEGY("binance_onnx_perp", bind_binance_onnx_perp);
