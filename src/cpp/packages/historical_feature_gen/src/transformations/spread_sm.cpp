#include "market_data/state_machine_registry.h"
#include "historical_feature_gen/transformations/spread_sm.h"

#include <iostream>

namespace HistoricalFeatureGen {

void SpreadSM::configure(const nlohmann::json& params) {
    if (!params.contains("symbol_keys")) return;

    for (const auto& key : params["symbol_keys"]) {
        std::string k = key.get<std::string>();
        symbol_states_[k] = SymbolState{};
        std::cout << "[SpreadSM] configured symbol key: " << k << std::endl;
    }
}

void SpreadSM::initialize_symbol_states() {
    // No-op — configure() handles symbol registration
}

void SpreadSM::react_impl(const MarketData::MarketEvent& event) {
    if (event.type != MarketData::MarketEvent::BBA) return;

    auto it = symbol_states_.find(event.symbol_key());
    if (it == symbol_states_.end()) return;

    auto& state = it->second;
    state.last_bid = event.bba->bid_price;
    state.last_ask = event.bba->ask_price;
}

std::optional<std::vector<float>> SpreadSM::emit_state_impl() const {
    std::vector<float> result;
    result.reserve(symbol_states_.size());

    for (const auto& [key, state] : symbol_states_) {
        result.push_back(static_cast<float>(state.last_ask - state.last_bid));
    }
    return result;
}

std::vector<std::string> SpreadSM::feature_names() const {
    std::vector<std::string> names;
    names.reserve(symbol_states_.size());

    for (const auto& [key, _] : symbol_states_) {
        names.push_back(key + ":spread");
    }
    return names;
}

}  // namespace HistoricalFeatureGen

namespace HistoricalFeatureGen {
KERONIS_REGISTER_STATE_MACHINE("spread", SpreadSM);
}  // namespace HistoricalFeatureGen
