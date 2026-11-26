#include "historical_feature_gen/transformations/spread_sm.h"
#include "market_data/state_machine_registry.h"

namespace HistoricalFeatureGen {

void SpreadSM::configure(const nlohmann::json& params) {
    if (!params.contains("symbol_keys")) return;
    symbol_states_.clear();
    for (const auto& key : params["symbol_keys"]) {
        symbol_states_[key.get<std::string>()] = SymbolState{};
    }
}

void SpreadSM::initialize_symbol_states() {}

void SpreadSM::react_impl(const MarketData::MarketEvent& event) {
    if (event.type != MarketData::MarketEvent::BBA || !event.bba.has_value()) {
        return;
    }
    auto it = symbol_states_.find(event.symbol_key());
    if (it == symbol_states_.end()) return;

    const double bid = event.bba->bid_price;
    const double ask = event.bba->ask_price;
    if (bid <= 0.0 || ask <= 0.0 || ask < bid) return;

    it->second.last_bid = bid;
    it->second.last_ask = ask;
    it->second.has_bba = true;
}

std::optional<std::vector<float>> SpreadSM::emit_state_impl() const {
    if (symbol_states_.empty()) return std::nullopt;
    std::vector<float> result;
    result.reserve(symbol_states_.size());
    for (const auto& [key, state] : symbol_states_) {
        if (!state.has_bba) return std::nullopt;
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

KERONIS_REGISTER_STATE_MACHINE("spread", SpreadSM);

}  // namespace HistoricalFeatureGen
