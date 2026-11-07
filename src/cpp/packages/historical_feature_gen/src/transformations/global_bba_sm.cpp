#include "market_data/state_machine_registry.h"
#include "historical_feature_gen/transformations/global_bba_sm.h"

#include <algorithm>
#include <cmath>

namespace HistoricalFeatureGen {

void GlobalBBASM::configure(const nlohmann::json& /* params */) {
    asset_groups_ = {
        {"btc", {"BTCUSDT:binance:swap"}, {}, -1},
        {"eth", {"ETHUSDT:binance:swap"}, {}, -1},
        {"xrp", {"XRPUSDT:binance:swap"}, {}, -1},
    };

    for (const auto& group : asset_groups_) {
        for (const auto& key : group.symbol_keys) {
            symbol_states_[key] = SymbolState{};
            l2_books_[key] = L2Book{};
        }
    }

    num_features_ = asset_groups_.size() * FEATURES_PER_GROUP;
}

void GlobalBBASM::initialize_symbol_states() {
    configure(nlohmann::json::object());
}

// --- L2Book ---

void GlobalBBASM::L2Book::apply_update(
    const std::vector<std::pair<double, double>>& bid_updates,
    const std::vector<std::pair<double, double>>& ask_updates) {
    for (const auto& [price, qty] : bid_updates) {
        if (qty == 0.0) {
            bids.erase(price);
        } else {
            bids[price] = qty;
        }
    }
    for (const auto& [price, qty] : ask_updates) {
        if (qty == 0.0) {
            asks.erase(price);
        } else {
            asks[price] = qty;
        }
    }
    if (!initialized && (!bids.empty() || !asks.empty())) {
        initialized = true;
    }
}

void GlobalBBASM::L2Book::apply_snapshot(
    const std::vector<std::pair<double, double>>& bid_levels,
    const std::vector<std::pair<double, double>>& ask_levels) {
    bids.clear();
    asks.clear();
    for (const auto& [price, qty] : bid_levels) {
        if (qty > 0.0) bids[price] = qty;
    }
    for (const auto& [price, qty] : ask_levels) {
        if (qty > 0.0) asks[price] = qty;
    }
    initialized = true;
}

void GlobalBBASM::L2Book::trim(double mid_price, double max_distance_pct) {
    if (mid_price <= 0.0) return;
    double min_price = mid_price * (1.0 - max_distance_pct);
    double max_price = mid_price * (1.0 + max_distance_pct);

    auto bid_it = bids.begin();
    while (bid_it != bids.end() && bid_it->first < min_price) {
        bid_it = bids.erase(bid_it);
    }

    auto ask_it = asks.upper_bound(max_price);
    asks.erase(ask_it, asks.end());
}

double GlobalBBASM::L2Book::bid_depth(double mid_price, double band_pct) const {
    double min_price = mid_price * (1.0 - band_pct);
    double total = 0.0;
    for (auto it = bids.rbegin(); it != bids.rend(); ++it) {
        if (it->first < min_price) break;
        total += it->second;
    }
    return total;
}

double GlobalBBASM::L2Book::ask_depth(double mid_price, double band_pct) const {
    double max_price = mid_price * (1.0 + band_pct);
    double total = 0.0;
    for (const auto& [price, qty] : asks) {
        if (price > max_price) break;
        total += qty;
    }
    return total;
}

double GlobalBBASM::L2Book::bid_top_n_qty(int n) const {
    double total = 0.0;
    int count = 0;
    for (auto it = bids.rbegin(); it != bids.rend() && count < n;
         ++it, ++count) {
        total += it->second;
    }
    return total;
}

double GlobalBBASM::L2Book::ask_top_n_qty(int n) const {
    double total = 0.0;
    int count = 0;
    for (const auto& [price, qty] : asks) {
        if (count >= n) break;
        total += qty;
        ++count;
    }
    return total;
}

double GlobalBBASM::L2Book::microprice_offset(int n_levels,
                                              double mid_price) const {
    if (mid_price <= 0.0 || bids.empty() || asks.empty()) return 0.0;

    double weighted_sum = 0.0;
    double total_qty = 0.0;

    int count = 0;
    for (auto it = bids.rbegin(); it != bids.rend() && count < n_levels;
         ++it, ++count) {
        weighted_sum += it->first * it->second;
        total_qty += it->second;
    }

    count = 0;
    for (const auto& [price, qty] : asks) {
        if (count >= n_levels) break;
        weighted_sum += price * qty;
        total_qty += qty;
        ++count;
    }

    if (total_qty == 0.0) return 0.0;
    double microprice = weighted_sum / total_qty;
    return (microprice - mid_price) / mid_price;
}

// --- History helpers ---

void GlobalBBASM::update_group_history(AssetGroup& group,
                                       float current_imbalance,
                                       long long now_sec) {
    if (now_sec != group.last_snapshot_sec) {
        if (group.last_snapshot_sec >= 0) {
            long long start = group.last_snapshot_sec + 1;
            long long end =
                std::min(now_sec, group.last_snapshot_sec + BBA_HISTORY_SECS);
            for (long long s = start; s <= end; ++s) {
                int idx = static_cast<int>(s % BBA_HISTORY_SECS);
                group.history[idx] = BBASnapshot{};
            }
        }
        group.last_snapshot_sec = now_sec;
    }
    int idx = static_cast<int>(now_sec % BBA_HISTORY_SECS);
    group.history[idx] = {current_imbalance, true};
}

float GlobalBBASM::group_mean(const AssetGroup& group, long long now_sec,
                              int window_sec) const {
    float sum = 0.0f;
    int count = 0;
    for (int i = 1; i <= window_sec && i < BBA_HISTORY_SECS; ++i) {
        int idx = static_cast<int>((now_sec - i) % BBA_HISTORY_SECS);
        if (idx < 0) idx += BBA_HISTORY_SECS;
        if (group.history[idx].valid) {
            sum += group.history[idx].imbalance;
            ++count;
        }
    }
    return (count > 0) ? sum / static_cast<float>(count) : 0.5f;
}

// --- React ---

void GlobalBBASM::react_impl(const MarketData::MarketEvent& event) {
    if (event.type == MarketData::MarketEvent::BBA) {
        auto it = symbol_states_.find(event.symbol_key());
        if (it == symbol_states_.end()) return;

        auto& state = it->second;
        state.bid_qty = event.bba->bid_qty;
        state.ask_qty = event.bba->ask_qty;
        state.bid_price = event.bba->bid_price;
        state.ask_price = event.bba->ask_price;
        state.last_update_ms = event.bba->timestamp_ms;

        if (state.bid_price > 0.0 && state.ask_price > 0.0) {
            double mid = (state.bid_price + state.ask_price) / 2.0;
            const auto& sym_key = it->first;
            for (size_t gi = 0; gi < asset_groups_.size(); ++gi) {
                for (const auto& gk : asset_groups_[gi].symbol_keys) {
                    if (gk == sym_key) {
                        atomic_mid_prices_[gi].store(mid,
                                                     std::memory_order_relaxed);
                    }
                }
            }
        }

        if (event.bba->timestamp_ms > latest_event_ts_ms_) {
            latest_event_ts_ms_ = event.bba->timestamp_ms;
        }

        long long now_sec = event.bba->timestamp_ms / 1000;
        const auto& key = event.symbol_key();
        for (auto& group : asset_groups_) {
            for (const auto& gk : group.symbol_keys) {
                if (gk == key) {
                    double sum = state.bid_qty + state.ask_qty;
                    float ratio = (sum == 0.0)
                                      ? 0.5f
                                      : static_cast<float>(state.bid_qty / sum);
                    update_group_history(group, ratio, now_sec);
                    break;
                }
            }
        }

    } else if (event.type == MarketData::MarketEvent::L2_UPDATE) {
        auto key = event.symbol_key();
        auto it = l2_books_.find(key);
        if (it == l2_books_.end()) return;

        auto& book = it->second;
        if (event.l2_update->sub_type ==
            MarketData::L2UpdateEvent::FULL_REFRESH) {
            book.apply_snapshot(event.l2_update->update.bids,
                                event.l2_update->update.asks);
        } else {
            book.apply_update(event.l2_update->update.bids,
                              event.l2_update->update.asks);
        }
        book.last_update_ms = event.l2_update->update.timestamp;

        if (event.l2_update->update.timestamp > latest_event_ts_ms_) {
            latest_event_ts_ms_ = event.l2_update->update.timestamp;
        }

        auto sym_it = symbol_states_.find(key);
        if (sym_it != symbol_states_.end() && sym_it->second.bid_price > 0.0 &&
            sym_it->second.ask_price > 0.0) {
            double mid =
                (sym_it->second.bid_price + sym_it->second.ask_price) / 2.0;
            book.trim(mid, 0.05);
        }
    }

    if (is_live_mode() && latest_event_ts_ms_ - last_recompute_ts_ >= 10) {
        last_recompute_ts_ = latest_event_ts_ms_;
        recompute_features();
    }
}

// --- Feature computation (pure read-only) ---

std::pair<bool, size_t> GlobalBBASM::compute_features(float* out) const {
    if (latest_event_ts_ms_ == 0) return {false, 0};

    for (const auto& [key, state] : symbol_states_) {
        if (state.last_update_ms == 0) return {false, 0};
        if (latest_event_ts_ms_ - state.last_update_ms >
            STALENESS_THRESHOLD_MS) {
            return {false, 0};
        }
    }

    for (const auto& [key, book] : l2_books_) {
        if (book.last_update_ms == 0) return {false, 0};
        if (latest_event_ts_ms_ - book.last_update_ms >
            STALENESS_THRESHOLD_MS) {
            return {false, 0};
        }
    }

    long long now_sec = latest_event_ts_ms_ / 1000;
    size_t idx = 0;

    for (const auto& group : asset_groups_) {
        float group_ratio = 0.5f;
        for (const auto& key : group.symbol_keys) {
            const auto& state = symbol_states_.at(key);
            double sum = state.bid_qty + state.ask_qty;
            float ratio =
                (sum == 0.0) ? 0.5f : static_cast<float>(state.bid_qty / sum);
            out[idx++] = ratio;
            group_ratio = ratio;
        }

        for (int window : BBA_WINDOWS) {
            float mean = group_mean(group, now_sec, window);
            out[idx++] = group_ratio - mean;
        }

        double mid_price = 0.0;
        if (!group.symbol_keys.empty()) {
            const auto& state = symbol_states_.at(group.symbol_keys[0]);
            if (state.bid_price > 0.0 && state.ask_price > 0.0) {
                mid_price = (state.bid_price + state.ask_price) / 2.0;
            }
        }

        for (size_t b = 0; b < DEPTH_BANDS.size(); ++b) {
            double total_bid_depth = 0.0;
            double total_ask_depth = 0.0;
            if (mid_price > 0.0) {
                for (const auto& key : group.symbol_keys) {
                    auto book_it = l2_books_.find(key);
                    if (book_it != l2_books_.end() &&
                        book_it->second.initialized) {
                        total_bid_depth += book_it->second.bid_depth(
                            mid_price, DEPTH_BANDS[b]);
                        total_ask_depth += book_it->second.ask_depth(
                            mid_price, DEPTH_BANDS[b]);
                    }
                }
            }
            double total = total_bid_depth + total_ask_depth;
            out[idx++] = (total == 0.0)
                             ? 0.5f
                             : static_cast<float>(total_bid_depth / total);
        }

        double bid_50bp = 0.0;
        double ask_50bp = 0.0;
        if (mid_price > 0.0) {
            for (const auto& key : group.symbol_keys) {
                auto book_it = l2_books_.find(key);
                if (book_it != l2_books_.end() && book_it->second.initialized) {
                    bid_50bp += book_it->second.bid_depth(mid_price, 0.005);
                    ask_50bp += book_it->second.ask_depth(mid_price, 0.005);
                }
            }
        }

        for (int n : CONCENTRATION_LEVELS) {
            double bid_top = 0.0;
            double ask_top = 0.0;
            for (const auto& key : group.symbol_keys) {
                auto book_it = l2_books_.find(key);
                if (book_it != l2_books_.end() && book_it->second.initialized) {
                    bid_top += book_it->second.bid_top_n_qty(n);
                    ask_top += book_it->second.ask_top_n_qty(n);
                }
            }
            out[idx++] = (bid_50bp == 0.0)
                             ? 0.0f
                             : static_cast<float>(bid_top / bid_50bp);
            out[idx++] = (ask_50bp == 0.0)
                             ? 0.0f
                             : static_cast<float>(ask_top / ask_50bp);
        }

        double bid_10bp = 0.0;
        double ask_10bp = 0.0;
        if (mid_price > 0.0) {
            for (const auto& key : group.symbol_keys) {
                auto book_it = l2_books_.find(key);
                if (book_it != l2_books_.end() && book_it->second.initialized) {
                    bid_10bp += book_it->second.bid_depth(mid_price, 0.001);
                    ask_10bp += book_it->second.ask_depth(mid_price, 0.001);
                }
            }
        }
        out[idx++] =
            (bid_50bp == 0.0) ? 0.0f : static_cast<float>(bid_10bp / bid_50bp);
        out[idx++] =
            (ask_50bp == 0.0) ? 0.0f : static_cast<float>(ask_10bp / ask_50bp);

        for (int n : MICROPRICE_LEVELS) {
            double offset = 0.0;
            if (mid_price > 0.0) {
                for (const auto& key : group.symbol_keys) {
                    auto book_it = l2_books_.find(key);
                    if (book_it != l2_books_.end() &&
                        book_it->second.initialized) {
                        offset +=
                            book_it->second.microprice_offset(n, mid_price);
                    }
                }
            }
            out[idx++] = static_cast<float>(offset);
        }
    }

    return {true, idx};
}

// --- Seqlock cache update ---

void GlobalBBASM::recompute_features() {
    if (!is_live_mode()) return;

    float features[MAX_FEATURES]{};
    auto [valid, count] = compute_features(features);

    uint32_t s = seq_.load(std::memory_order_relaxed);
    seq_.store(s + 1, std::memory_order_release);
    for (size_t i = 0; i < count; ++i) {
        cached_features_[i].store(features[i], std::memory_order_relaxed);
    }
    cached_valid_.store(valid, std::memory_order_relaxed);
    seq_.store(s + 2, std::memory_order_release);
}

// --- Emit ---

std::optional<std::vector<float>> GlobalBBASM::emit_state_impl() const {
    if (is_live_mode()) {
        uint32_t s0 = seq_.load(std::memory_order_acquire);
        if (s0 & 1) return std::nullopt;  // writer active, skip
        std::vector<float> state(num_features_);
        for (size_t i = 0; i < num_features_; ++i) {
            state[i] = cached_features_[i].load(std::memory_order_relaxed);
        }
        bool valid = cached_valid_.load(std::memory_order_relaxed);
        uint32_t s1 = seq_.load(std::memory_order_acquire);
        if (s0 != s1) return std::nullopt;  // torn read, skip
        if (!valid) return std::nullopt;
        return state;
    }

    float features[MAX_FEATURES]{};
    auto [valid, count] = compute_features(features);
    if (!valid) return std::nullopt;
    return std::vector<float>(features, features + count);
}

// --- Feature names ---

std::vector<std::string> GlobalBBASM::feature_names() const {
    std::vector<std::string> names;

    auto fmt_band = [](double band) -> std::string {
        int bp = static_cast<int>(std::round(band * 10000));
        return std::to_string(bp) + "bp";
    };

    for (const auto& group : asset_groups_) {
        for (const auto& key : group.symbol_keys) {
            names.push_back(key + ":bba_imbalance");
        }

        for (int window : BBA_WINDOWS) {
            names.push_back(group.name + ":bba_momentum_" +
                            std::to_string(window) + "s");
        }

        for (double band : DEPTH_BANDS) {
            names.push_back(group.name + ":depth_imbalance_" + fmt_band(band));
        }

        for (int n : CONCENTRATION_LEVELS) {
            names.push_back(group.name + ":bid_concentration_top" +
                            std::to_string(n));
            names.push_back(group.name + ":ask_concentration_top" +
                            std::to_string(n));
        }

        names.push_back(group.name + ":bid_gradient_10bp_50bp");
        names.push_back(group.name + ":ask_gradient_10bp_50bp");

        for (int n : MICROPRICE_LEVELS) {
            names.push_back(group.name + ":microprice_offset_top" +
                            std::to_string(n));
        }
    }

    return names;
}

// --- Dependencies ---

std::vector<MarketData::SMMarketDataDependency>
GlobalBBASM::get_market_data_dependencies() const {
    std::vector<MarketData::SMMarketDataDependency> deps;
    for (const auto& [key, _] : symbol_states_) {
        auto c1 = key.find(':');
        auto c2 = key.find(':', c1 + 1);

        std::string symbol = key.substr(0, c1);
        std::string exchange = key.substr(c1 + 1, c2 - c1 - 1);
        std::string market_type = key.substr(c2 + 1);

        MarketData::SMMarketDataDependency bba_dep;
        bba_dep.symbol = symbol;
        bba_dep.exchange_name = exchange;
        bba_dep.market_type = market_type;
        bba_dep.event_type = MarketData::MarketEvent::BBA;
        deps.push_back(std::move(bba_dep));

        MarketData::SMMarketDataDependency l2_dep;
        l2_dep.symbol = symbol;
        l2_dep.exchange_name = exchange;
        l2_dep.market_type = market_type;
        l2_dep.event_type = MarketData::MarketEvent::L2_UPDATE;
        deps.push_back(std::move(l2_dep));
    }
    return deps;
}

// --- Mid-price accessor ---

std::optional<double> GlobalBBASM::get_mid_price(
    const std::string& symbol_key) const {
    for (size_t gi = 0; gi < asset_groups_.size(); ++gi) {
        for (const auto& gk : asset_groups_[gi].symbol_keys) {
            if (gk == symbol_key) {
                double p =
                    atomic_mid_prices_[gi].load(std::memory_order_relaxed);
                if (p <= 0.0) return std::nullopt;
                return p;
            }
        }
    }
    return std::nullopt;
}

}  // namespace HistoricalFeatureGen

namespace HistoricalFeatureGen {
KERONIS_REGISTER_STATE_MACHINE("global_bba", GlobalBBASM);
}  // namespace HistoricalFeatureGen
