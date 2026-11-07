#pragma once

#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "market_data/strategy_state_machine.h"

namespace HistoricalFeatureGen {

class WindowedTakerFlowSM : public MarketData::StrategyStateMachine {
   public:
    static constexpr int NUM_BUCKETS = 1800;  // 1 bucket per second, 30 minutes

    void configure(const nlohmann::json& params) override;
    void initialize_symbol_states() override;
    void react_impl(const MarketData::MarketEvent& event) override;
    std::optional<std::vector<float>> emit_state_impl() const override;
    std::vector<std::string> feature_names() const override;

   private:
    struct Bucket {
        double buy_volume = 0.0;
        double sell_volume = 0.0;
        int trade_count = 0;

        void clear() {
            buy_volume = 0.0;
            sell_volume = 0.0;
            trade_count = 0;
        }
    };

    struct SymbolState {
        std::array<Bucket, NUM_BUCKETS> buckets{};
        long long last_sec = -1;
    };

    void clear_stale_buckets(SymbolState& state, long long current_sec) const;

    // sorted map so emit order is deterministic
    std::map<std::string, SymbolState> symbol_states_;
    std::vector<int> windows_sec_ = {30, 60, 300, 900, 1800};  // 30s, 1m, 5m, 15m, 30m
};

}  // namespace HistoricalFeatureGen
