#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "market_data/strategy_state_machine.h"

namespace HistoricalFeatureGen {

class TakerFlowSM : public MarketData::StrategyStateMachine {
   public:
    void configure(const nlohmann::json& params) override;
    void initialize_symbol_states() override;
    void react_impl(const MarketData::MarketEvent& event) override;
    std::optional<std::vector<float>> emit_state_impl() const override;
    std::vector<std::string> feature_names() const override;

   private:
    struct SymbolState {
        double buy_volume = 0.0;
        double sell_volume = 0.0;
        long long trade_count = 0;
    };

    // sorted map so emit order is deterministic
    std::map<std::string, SymbolState> symbol_states_;
};

}  // namespace HistoricalFeatureGen
