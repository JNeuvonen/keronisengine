#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "market_data/strategy_state_machine.h"

namespace HistoricalFeatureGen {

class BBAImbalanceSM : public MarketData::StrategyStateMachine {
   public:
    void configure(const nlohmann::json& params) override;
    void initialize_symbol_states() override;
    void react_impl(const MarketData::MarketEvent& event) override;
    std::optional<std::vector<float>> emit_state_impl() const override;
    std::vector<std::string> feature_names() const override;

   private:
    static constexpr long long STALENESS_THRESHOLD_MS = 5000;

    struct SymbolState {
        double bid_qty = 0.0;
        double ask_qty = 0.0;
        long long last_update_ms = 0;
    };

    // sorted map so emit order is deterministic
    std::map<std::string, SymbolState> symbol_states_;
    long long latest_event_ts_ms_ = 0;
};

}  // namespace HistoricalFeatureGen
