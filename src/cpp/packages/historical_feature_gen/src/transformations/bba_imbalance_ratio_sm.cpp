#include "market_data/state_machine_registry.h"
#include "historical_feature_gen/transformations/bba_imbalance_ratio_sm.h"

#include <iostream>

namespace HistoricalFeatureGen {

void BBAImbalanceSM::configure(const nlohmann::json& params) {
    if (!params.contains("symbol_keys")) return;

    for (const auto& key : params["symbol_keys"]) {
        std::string k = key.get<std::string>();
        symbol_states_[k] = SymbolState{};
        std::cout << "[BBAImbalanceSM] configured symbol key: " << k << std::endl;
    }
}

void BBAImbalanceSM::initialize_symbol_states() {
    // No-op — configure() handles symbol registration
}

void BBAImbalanceSM::react_impl(const MarketData::MarketEvent& event) {
    if (event.type != MarketData::MarketEvent::BBA) return;

    auto it = symbol_states_.find(event.symbol_key());
    if (it == symbol_states_.end()) return;

    auto& state = it->second;
    state.bid_qty = event.bba->bid_qty;
    state.ask_qty = event.bba->ask_qty;
    state.last_update_ms = event.bba->timestamp_ms;

    if (event.bba->timestamp_ms > latest_event_ts_ms_) {
        latest_event_ts_ms_ = event.bba->timestamp_ms;
    }
}

std::optional<std::vector<float>> BBAImbalanceSM::emit_state_impl() const {
    if (latest_event_ts_ms_ == 0) return std::nullopt;

    for (const auto& [key, state] : symbol_states_) {
        if (state.last_update_ms == 0) return std::nullopt;
        if (latest_event_ts_ms_ - state.last_update_ms > STALENESS_THRESHOLD_MS) {
            return std::nullopt;
        }
    }

    std::vector<float> result;
    result.reserve(symbol_states_.size() + 1);

    double total_bid = 0.0;
    double total_ask = 0.0;

    for (const auto& [key, state] : symbol_states_) {
        double sum = state.bid_qty + state.ask_qty;
        float ratio = (sum == 0.0) ? 0.5f : static_cast<float>(state.bid_qty / sum);
        result.push_back(ratio);
        total_bid += state.bid_qty;
        total_ask += state.ask_qty;
    }

    double total_sum = total_bid + total_ask;
    float total_ratio = (total_sum == 0.0) ? 0.5f : static_cast<float>(total_bid / total_sum);
    result.push_back(total_ratio);

    return result;
}

std::vector<std::string> BBAImbalanceSM::feature_names() const {
    std::vector<std::string> names;
    names.reserve(symbol_states_.size() + 1);

    for (const auto& [key, _] : symbol_states_) {
        names.push_back(key + ":bba_imbalance_ratio");
    }
    names.push_back("total:bba_imbalance_ratio");

    return names;
}

}  // namespace HistoricalFeatureGen

namespace HistoricalFeatureGen {
KERONIS_REGISTER_STATE_MACHINE("bba_imbalance_ratio", BBAImbalanceSM);
}  // namespace HistoricalFeatureGen
