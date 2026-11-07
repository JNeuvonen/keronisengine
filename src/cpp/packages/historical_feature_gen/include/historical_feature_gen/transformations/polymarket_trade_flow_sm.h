#pragma once

#include <array>
#include <deque>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "market_data/strategy_state_machine.h"

namespace HistoricalFeatureGen {

class PolymarketTradeFlowSM : public MarketData::StrategyStateMachine {
   public:
    static constexpr int NUM_BUCKETS = 300;  // 5 minutes of 1-second buckets
    static constexpr int NUM_WINDOWS = 5;
    static constexpr std::array<int, NUM_WINDOWS> WINDOWS_SEC = {5, 15, 30, 60,
                                                                 300};
    static constexpr int MAX_CLOSED_HISTORY = 1000;

    struct FlowSnapshot {
        std::array<double, NUM_WINDOWS> buy_volume{};
        std::array<double, NUM_WINDOWS> sell_volume{};
        std::array<double, NUM_WINDOWS> ofi{};
        std::array<int, NUM_WINDOWS> trade_count{};
    };

    struct ClosedEventProfile {
        std::string condition_id;
        double total_buy_volume = 0.0;
        double total_sell_volume = 0.0;
        double total_volume = 0.0;
        double volume_per_sec = 0.0;
        double buy_ratio = 0.0;
        int total_trade_count = 0;
        double trades_per_sec = 0.0;
        double duration_sec = 0.0;
        long long close_time_ms = 0;
    };

    // Live flow query per asset_id. Seqlock-protected.
    [[nodiscard]] std::optional<FlowSnapshot> get_flow(
        const std::string& asset_id) const;

    // Rolling average of last n closed events (0 = all history).
    [[nodiscard]] std::optional<ClosedEventProfile> get_closed_avg(
        int n = 0) const;

    std::string get_name() const override { return "polymarket_trade_flow"; }
    void initialize_symbol_states() override {}

   protected:
    void react_impl(const MarketData::MarketEvent& event) override;
    std::optional<std::vector<float>> emit_state_impl() const override {
        return std::nullopt;
    }
    void serialize_state_impl(std::ostream& out) const override;
    bool deserialize_state_impl(std::istream& in) override;

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

    struct OutcomeState {
        std::array<Bucket, NUM_BUCKETS> buckets{};
        long long last_sec = -1;
        double cumulative_buy = 0.0;
        double cumulative_sell = 0.0;
        int cumulative_count = 0;
    };

    struct EventState {
        std::string condition_id;
        long long first_trade_ts_ms = 0;
        long long last_trade_ts_ms = 0;
        std::unordered_map<std::string, int> asset_to_outcome;
        std::vector<OutcomeState> outcomes;
    };

    void clear_stale_buckets(OutcomeState& state, long long current_sec) const;
    void close_event(const std::string& condition_id, long long close_ts);

    // condition_id → live state
    std::unordered_map<std::string, EventState> event_states_;
    // asset_id → condition_id (reverse lookup for get_flow)
    std::unordered_map<std::string, std::string> asset_to_condition_;

    // Persisted across restarts
    std::deque<ClosedEventProfile> closed_history_;
    std::unordered_set<std::string> already_closed_;

    mutable std::shared_mutex map_mutex_;
};

}  // namespace HistoricalFeatureGen
