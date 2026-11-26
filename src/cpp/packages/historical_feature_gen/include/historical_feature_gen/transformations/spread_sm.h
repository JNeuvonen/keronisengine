#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "market_data/strategy_state_machine.h"

namespace HistoricalFeatureGen {

class SpreadSM : public MarketData::StrategyStateMachine {
   public:
    std::string get_name() const override { return "spread"; }
    void configure(const nlohmann::json& params) override;
    void initialize_symbol_states() override;
    void react_impl(const MarketData::MarketEvent& event) override;
    std::optional<std::vector<float>> emit_state_impl() const override;
    std::vector<std::string> feature_names() const override;

   private:
    struct SymbolState {
        bool has_bba = false;
        double last_bid = 0.0;
        double last_ask = 0.0;
    };

    // Sorted map so emitted feature order is deterministic.
    std::map<std::string, SymbolState> symbol_states_;
};

}  // namespace HistoricalFeatureGen
