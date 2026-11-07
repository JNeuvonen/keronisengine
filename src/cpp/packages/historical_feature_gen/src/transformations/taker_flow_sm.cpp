#include "market_data/state_machine_registry.h"
#include "historical_feature_gen/transformations/taker_flow_sm.h"

#include <iostream>

namespace HistoricalFeatureGen {

void TakerFlowSM::configure(const nlohmann::json& params) {
    if (!params.contains("symbol_keys")) return;

    for (const auto& key : params["symbol_keys"]) {
        std::string k = key.get<std::string>();
        symbol_states_[k] = SymbolState{};
        std::cout << "[TakerFlowSM] configured symbol key: " << k << std::endl;
    }
}

void TakerFlowSM::initialize_symbol_states() {
    // No-op — configure() handles symbol registration
}

void TakerFlowSM::react_impl(const MarketData::MarketEvent& event) {
    if (event.type != MarketData::MarketEvent::TRADE) return;

    auto it = symbol_states_.find(event.symbol_key());
    if (it == symbol_states_.end()) return;

    auto& state = it->second;
    double notional = event.trade->price * event.trade->qty;

    if (event.trade->is_buyer_maker) {
        state.sell_volume += notional;
    } else {
        state.buy_volume += notional;
    }
    state.trade_count++;
}

std::optional<std::vector<float>> TakerFlowSM::emit_state_impl() const {
    std::vector<float> result;
    result.reserve(symbol_states_.size() * 4);

    for (const auto& [key, state] : symbol_states_) {
        double total = state.buy_volume + state.sell_volume;
        float buy_ratio = total > 0.0 ? static_cast<float>(state.buy_volume / total) : 0.0f;
        result.push_back(buy_ratio);
        result.push_back(static_cast<float>(state.trade_count));
        result.push_back(static_cast<float>(state.buy_volume));
        result.push_back(static_cast<float>(state.sell_volume));
    }
    return result;
}

std::vector<std::string> TakerFlowSM::feature_names() const {
    std::vector<std::string> names;
    names.reserve(symbol_states_.size() * 4);

    for (const auto& [key, _] : symbol_states_) {
        names.push_back(key + ":buy_ratio");
        names.push_back(key + ":trade_count");
        names.push_back(key + ":buy_volume");
        names.push_back(key + ":sell_volume");
    }
    return names;
}

}  // namespace HistoricalFeatureGen

namespace HistoricalFeatureGen {
KERONIS_REGISTER_STATE_MACHINE("taker_flow", TakerFlowSM);
}  // namespace HistoricalFeatureGen
