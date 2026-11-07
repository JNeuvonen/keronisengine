#pragma once

#include <array>
#include <atomic>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "market_data/strategy_state_machine.h"

namespace HistoricalFeatureGen {

class GlobalBBASM : public MarketData::StrategyStateMachine {
   public:
    void configure(const nlohmann::json& params) override;
    void initialize_symbol_states() override;
    std::string get_name() const override { return "global_bba"; }
    std::vector<std::string> feature_names() const override;
    std::vector<MarketData::SMMarketDataDependency>
    get_market_data_dependencies() const override;

    std::optional<double> get_mid_price(
        const std::string& symbol_key) const;

   protected:
    void react_impl(const MarketData::MarketEvent& event) override;
    std::optional<std::vector<float>> emit_state_impl() const override;

   private:
    static constexpr long long STALENESS_THRESHOLD_MS = 5000;
    static constexpr int BBA_HISTORY_SECS = 16;
    static constexpr std::array<int, 5> BBA_WINDOWS = {1, 2, 3, 5, 10};

    static constexpr std::array<double, 4> DEPTH_BANDS = {
        0.001, 0.002, 0.005, 0.01
    };

    static constexpr std::array<int, 3> CONCENTRATION_LEVELS = {1, 3, 10};
    static constexpr std::array<int, 3> MICROPRICE_LEVELS = {1, 3, 10};

    // 21 features per group × 3 groups = 63
    static constexpr size_t FEATURES_PER_GROUP = 21;
    static constexpr size_t MAX_GROUPS = 4;
    static constexpr size_t MAX_FEATURES = FEATURES_PER_GROUP * MAX_GROUPS;

    struct SymbolState {
        double bid_qty = 0.0;
        double ask_qty = 0.0;
        double bid_price = 0.0;
        double ask_price = 0.0;
        long long last_update_ms = 0;
    };

    struct BBASnapshot {
        float imbalance = 0.5f;
        bool valid = false;
    };

    struct L2Book {
        std::map<double, double> bids;
        std::map<double, double> asks;
        bool initialized = false;
        long long last_update_ms = 0;

        void apply_update(const std::vector<std::pair<double, double>>& bid_updates,
                          const std::vector<std::pair<double, double>>& ask_updates);
        void apply_snapshot(const std::vector<std::pair<double, double>>& bid_levels,
                            const std::vector<std::pair<double, double>>& ask_levels);
        void trim(double mid_price, double max_distance_pct);
        double bid_depth(double mid_price, double band_pct) const;
        double ask_depth(double mid_price, double band_pct) const;
        double bid_top_n_qty(int n) const;
        double ask_top_n_qty(int n) const;
        double microprice_offset(int n_levels, double mid_price) const;
    };

    struct AssetGroup {
        std::string name;
        std::vector<std::string> symbol_keys;
        std::array<BBASnapshot, BBA_HISTORY_SECS> history{};
        long long last_snapshot_sec = -1;
    };

    std::pair<bool, size_t> compute_features(float* out) const;

    void update_group_history(AssetGroup& group, float current_imbalance,
                              long long now_sec);
    float group_mean(const AssetGroup& group, long long now_sec,
                     int window_sec) const;

    void recompute_features();

    std::map<std::string, SymbolState> symbol_states_;
    std::map<std::string, L2Book> l2_books_;
    std::vector<AssetGroup> asset_groups_;
    long long latest_event_ts_ms_ = 0;
    long long last_recompute_ts_ = 0;

    // Seqlock cache for live mode
    std::atomic<uint32_t> seq_{0};
    std::atomic<float> cached_features_[MAX_FEATURES]{};
    std::atomic<bool> cached_valid_{false};
    size_t num_features_ = 0;

    // Atomic mid-price storage per asset group
    std::atomic<double> atomic_mid_prices_[MAX_GROUPS]{};
};

}  // namespace HistoricalFeatureGen
