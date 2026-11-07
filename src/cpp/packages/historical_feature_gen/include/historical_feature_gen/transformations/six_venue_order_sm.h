#pragma once

#include <array>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "market_data/strategy_state_machine.h"

namespace HistoricalFeatureGen {

class SixVenueOrderSM : public MarketData::StrategyStateMachine {
   public:
    void initialize_symbol_states() override;
    std::string get_name() const override { return "six_venue_order"; }
    std::vector<MarketData::SMMarketDataDependency>
    get_market_data_dependencies() const override;
    std::optional<std::vector<float>> emit_state_for_symbol(int symbol_idx) const override;
    std::vector<std::string> feature_names_for_symbol(int symbol_idx) const override;
    long long get_latest_event_ts_ms() const override { return latest_ts_ms_; }
    long long get_symbol_latest_event_ts_ms(int symbol_idx) const override;

   protected:
    void react_impl(const MarketData::MarketEvent& event) override;
    std::optional<std::vector<float>> emit_state_impl() const override;
    bool deserialize_state_impl(std::istream& in) override {
        (void)in;
        return false;
    }

   private:
    static constexpr int NUM_ASSETS = 2;
    static constexpr int NUM_VENUES = 6;
    static constexpr int NUM_FEEDS = NUM_ASSETS * NUM_VENUES;
    static constexpr int FLOW_BUCKETS = 32;

    struct FlowBucket {
        long long second = -1;
        double buy = 0.0;
        double sell = 0.0;
    };

    struct Feed {
        std::string symbol;
        std::string exchange;
        std::string market_type;
        double bid = 0.0;
        double ask = 0.0;
        double bid_qty = 0.0;
        double ask_qty = 0.0;
        long long bba_ts_ms = 0;
        long long l2_ts_ms = 0;
        long long snapshot_ts_ms = 0;
        long long trade_ts_ms = 0;
        bool has_snapshot = false;
        std::map<double, double> bids;
        std::map<double, double> asks;
        std::array<FlowBucket, FLOW_BUCKETS> flow{};
    };

    std::array<Feed, NUM_FEEDS> feeds_{};
    std::unordered_map<std::string, int> key_to_idx_;
    long long latest_ts_ms_ = 0;
};

}  // namespace HistoricalFeatureGen
