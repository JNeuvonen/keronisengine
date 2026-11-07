#include "market_data/state_machine_registry.h"
#include "historical_feature_gen/transformations/windowed_taker_flow_sm.h"

#include <algorithm>
#include <iostream>

namespace HistoricalFeatureGen {

void WindowedTakerFlowSM::configure(const nlohmann::json& params) {
    if (!params.contains("symbol_keys")) return;

    for (const auto& key : params["symbol_keys"]) {
        std::string k = key.get<std::string>();
        symbol_states_[k] = SymbolState{};
        std::cout << "[WindowedTakerFlowSM] configured symbol key: " << k
                  << std::endl;
    }

    if (params.contains("windows_sec")) {
        windows_sec_.clear();
        for (const auto& w : params["windows_sec"]) {
            int ws = w.get<int>();
            if (ws < 1 || ws > NUM_BUCKETS) {
                throw std::runtime_error("windows_sec values must be in [1, " +
                                         std::to_string(NUM_BUCKETS) + "]");
            }
            windows_sec_.push_back(ws);
        }
    }

    std::cout << "[WindowedTakerFlowSM] windows_sec: [";
    for (size_t i = 0; i < windows_sec_.size(); ++i) {
        if (i > 0) std::cout << ", ";
        std::cout << windows_sec_[i];
    }
    std::cout << "]" << std::endl;
}

void WindowedTakerFlowSM::initialize_symbol_states() {}

void WindowedTakerFlowSM::clear_stale_buckets(SymbolState& state,
                                              long long current_sec) const {
    if (state.last_sec < 0) {
        // First event — clear everything
        for (auto& b : state.buckets) b.clear();
        state.last_sec = current_sec;
        return;
    }

    long long gap = current_sec - state.last_sec;
    if (gap <= 0) return;  // same second or out-of-order (ignore)

    if (gap >= NUM_BUCKETS) {
        // Entire buffer is stale
        for (auto& b : state.buckets) b.clear();
    } else {
        // Clear buckets from (last_sec + 1) to current_sec
        for (long long s = state.last_sec + 1; s <= current_sec; ++s) {
            state.buckets[static_cast<size_t>(s % NUM_BUCKETS)].clear();
        }
    }
    state.last_sec = current_sec;
}

void WindowedTakerFlowSM::react_impl(const MarketData::MarketEvent& event) {
    if (event.type != MarketData::MarketEvent::TRADE) return;

    auto it = symbol_states_.find(event.symbol_key());
    if (it == symbol_states_.end()) return;

    auto& state = it->second;
    long long ts_sec = event.trade->timestamp_ms / 1000;

    clear_stale_buckets(state, ts_sec);

    auto& bucket = state.buckets[static_cast<size_t>(ts_sec % NUM_BUCKETS)];
    double notional = event.trade->price * event.trade->qty;
    if (event.trade->is_buyer_maker) {
        bucket.sell_volume += notional;
    } else {
        bucket.buy_volume += notional;
    }
    bucket.trade_count++;
}

std::optional<std::vector<float>> WindowedTakerFlowSM::emit_state_impl() const {
    size_t n_windows = windows_sec_.size();
    // 4 per window + (n_windows - 1) ratios
    size_t features_per_symbol =
        n_windows * 4 + (n_windows > 1 ? n_windows - 1 : 0);
    std::vector<float> result;
    result.reserve(symbol_states_.size() * features_per_symbol);

    for (const auto& [key, state] : symbol_states_) {
        if (state.last_sec < 0) {
            for (size_t i = 0; i < features_per_symbol; ++i) {
                result.push_back(0.0f);
            }
            continue;
        }

        // Compute per-window aggregates
        std::vector<double> win_total_vol(n_windows);
        for (size_t wi = 0; wi < n_windows; ++wi) {
            int window = windows_sec_[wi];
            double buy_vol = 0.0;
            double sell_vol = 0.0;
            int count = 0;

            long long start_sec = state.last_sec - window + 1;
            for (long long s = start_sec; s <= state.last_sec; ++s) {
                if (s < 0) continue;
                const auto& b =
                    state.buckets[static_cast<size_t>(s % NUM_BUCKETS)];
                buy_vol += b.buy_volume;
                sell_vol += b.sell_volume;
                count += b.trade_count;
            }

            double total = buy_vol + sell_vol;
            win_total_vol[wi] = total;

            result.push_back(total > 0.0 ? static_cast<float>(buy_vol / total)
                                         : 0.0f);
            result.push_back(static_cast<float>(count));
            result.push_back(static_cast<float>(buy_vol));
            result.push_back(static_cast<float>(sell_vol));
        }

        // Volume ratios: each window vs the largest (last) window
        if (n_windows > 1) {
            double base_vol = win_total_vol[n_windows - 1];
            for (size_t wi = 0; wi < n_windows - 1; ++wi) {
                result.push_back(
                    base_vol > 0.0
                        ? static_cast<float>(win_total_vol[wi] / base_vol)
                        : 0.0f);
            }
        }
    }
    return result;
}

std::vector<std::string> WindowedTakerFlowSM::feature_names() const {
    size_t n_windows = windows_sec_.size();
    size_t features_per_symbol =
        n_windows * 4 + (n_windows > 1 ? n_windows - 1 : 0);
    std::vector<std::string> names;
    names.reserve(symbol_states_.size() * features_per_symbol);

    for (const auto& [key, _] : symbol_states_) {
        for (int window : windows_sec_) {
            std::string prefix = key + ":";
            std::string suffix = "_" + std::to_string(window) + "s";
            names.push_back(prefix + "buy_ratio" + suffix);
            names.push_back(prefix + "trade_count" + suffix);
            names.push_back(prefix + "buy_volume" + suffix);
            names.push_back(prefix + "sell_volume" + suffix);
        }

        // Ratio names: vol_ratio_{small}s_vs_{large}s
        if (n_windows > 1) {
            std::string base_suffix =
                std::to_string(windows_sec_[n_windows - 1]) + "s";
            for (size_t wi = 0; wi < n_windows - 1; ++wi) {
                names.push_back(key + ":vol_ratio_" +
                                std::to_string(windows_sec_[wi]) + "s_vs_" +
                                base_suffix);
            }
        }
    }
    return names;
}

}  // namespace HistoricalFeatureGen

namespace HistoricalFeatureGen {
KERONIS_REGISTER_STATE_MACHINE("windowed_taker_flow", WindowedTakerFlowSM);
}  // namespace HistoricalFeatureGen
