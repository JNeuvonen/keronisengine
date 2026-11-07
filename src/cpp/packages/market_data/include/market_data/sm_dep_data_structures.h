#pragma once

#include <array>
#include <map>
#include <string>
#include <vector>

namespace MarketData {

// ============================================================================
// BBA State — tracks latest best bid/ask per symbol
// ============================================================================

struct BBAState {
    double bid_price = 0;
    double bid_qty = 0;
    double ask_price = 0;
    double ask_qty = 0;
    long long last_update_ms = 0;
    double prev_mid = 0;

    double mid_price() const { return (bid_price + ask_price) / 2.0; }

    double imbalance() const {
        double sum = bid_qty + ask_qty;
        return sum == 0.0 ? 0.5 : bid_qty / sum;
    }

    double mid_change() const {
        double mid = mid_price();
        if (prev_mid == 0.0 || mid == 0.0) return 0.0;
        return (mid - prev_mid) / prev_mid;
    }

    void update(double bp, double bq, double ap, double aq, long long ts) {
        prev_mid = mid_price();
        bid_price = bp;
        bid_qty = bq;
        ask_price = ap;
        ask_qty = aq;
        last_update_ms = ts;
    }
};

// ============================================================================
// Flow State — rolling 1-second buckets for trade flow tracking
// ============================================================================

struct FlowBucket {
    double buy_volume = 0;
    double sell_volume = 0;

    void clear() {
        buy_volume = 0;
        sell_volume = 0;
    }

    double ofi() const {
        double sum = buy_volume + sell_volume;
        return sum == 0.0 ? 0.0 : (buy_volume - sell_volume) / sum;
    }
};

template <int NUM_BUCKETS>
struct FlowState {
    std::array<FlowBucket, NUM_BUCKETS> buckets{};
    long long last_sec = -1;

    void clear_stale(long long current_sec) {
        if (last_sec < 0) {
            last_sec = current_sec;
            return;
        }
        long long gap = current_sec - last_sec;
        if (gap <= 0) return;
        if (gap >= NUM_BUCKETS) {
            for (auto& b : buckets) b.clear();
        } else {
            for (long long i = last_sec + 1; i <= current_sec; ++i) {
                buckets[i % NUM_BUCKETS].clear();
            }
        }
        last_sec = current_sec;
    }

    void add_trade(double qty, bool is_buyer_maker, long long ts_ms) {
        long long sec = ts_ms / 1000;
        clear_stale(sec);
        auto& bucket = buckets[sec % NUM_BUCKETS];
        double notional = qty;
        if (is_buyer_maker) {
            bucket.sell_volume += notional;
        } else {
            bucket.buy_volume += notional;
        }
    }

    double ofi_window(int window_sec) const {
        if (last_sec < 0) return 0.0;
        double buy = 0, sell = 0;
        int w = std::min(window_sec, NUM_BUCKETS);
        for (int i = 0; i < w; ++i) {
            long long sec = last_sec - i;
            if (sec < 0) break;
            const auto& b = buckets[sec % NUM_BUCKETS];
            buy += b.buy_volume;
            sell += b.sell_volume;
        }
        double sum = buy + sell;
        return sum == 0.0 ? 0.0 : (buy - sell) / sum;
    }
};

// ============================================================================
// L2 Book State — maintains order book + tracks additions/cancels near touch
// ============================================================================

struct L2Bucket {
    double bid_additions = 0;
    double ask_additions = 0;
    double bid_cancels = 0;
    double ask_cancels = 0;

    void clear() {
        bid_additions = 0;
        ask_additions = 0;
        bid_cancels = 0;
        ask_cancels = 0;
    }
};

template <int NUM_BUCKETS>
struct L2BookState {
    std::map<double, double> bids;
    std::map<double, double> asks;
    std::array<L2Bucket, NUM_BUCKETS> buckets{};
    long long last_sec = -1;

    void clear_stale(long long current_sec) {
        if (last_sec < 0) {
            last_sec = current_sec;
            return;
        }
        long long gap = current_sec - last_sec;
        if (gap <= 0) return;
        if (gap >= NUM_BUCKETS) {
            for (auto& b : buckets) b.clear();
        } else {
            for (long long i = last_sec + 1; i <= current_sec; ++i) {
                buckets[i % NUM_BUCKETS].clear();
            }
        }
        last_sec = current_sec;
    }

    void apply_update(
        const std::vector<std::pair<double, double>>& bid_updates,
        const std::vector<std::pair<double, double>>& ask_updates,
        double mid_price, double pct_threshold, long long ts_ms) {
        long long sec = ts_ms / 1000;
        clear_stale(sec);
        auto& bucket = buckets[sec % NUM_BUCKETS];

        double threshold = mid_price * pct_threshold;
        double lower_bound = mid_price - threshold;
        double upper_bound = mid_price + threshold;

        for (const auto& [price, qty] : bid_updates) {
            if (price < lower_bound || price > upper_bound) {
                // Update book but don't track in buckets
                if (qty == 0.0)
                    bids.erase(price);
                else
                    bids[price] = qty;
                continue;
            }
            double prev_qty = 0;
            auto it = bids.find(price);
            if (it != bids.end()) prev_qty = it->second;

            if (qty == 0.0) {
                bucket.bid_cancels += prev_qty;
                bids.erase(price);
            } else if (qty > prev_qty) {
                bucket.bid_additions += (qty - prev_qty);
                bids[price] = qty;
            } else if (qty < prev_qty) {
                bucket.bid_cancels += (prev_qty - qty);
                bids[price] = qty;
            } else {
                bids[price] = qty;
            }
        }

        for (const auto& [price, qty] : ask_updates) {
            if (price < lower_bound || price > upper_bound) {
                if (qty == 0.0)
                    asks.erase(price);
                else
                    asks[price] = qty;
                continue;
            }
            double prev_qty = 0;
            auto it = asks.find(price);
            if (it != asks.end()) prev_qty = it->second;

            if (qty == 0.0) {
                bucket.ask_cancels += prev_qty;
                asks.erase(price);
            } else if (qty > prev_qty) {
                bucket.ask_additions += (qty - prev_qty);
                asks[price] = qty;
            } else if (qty < prev_qty) {
                bucket.ask_cancels += (prev_qty - qty);
                asks[price] = qty;
            } else {
                asks[price] = qty;
            }
        }
    }

    double near_touch_imbalance(double mid_price,
                                double pct_threshold) const {
        double threshold = mid_price * pct_threshold;
        double lower = mid_price - threshold;
        double upper = mid_price + threshold;

        double bid_qty = 0, ask_qty = 0;
        for (auto it = bids.rbegin(); it != bids.rend(); ++it) {
            if (it->first < lower) break;
            if (it->first <= upper) bid_qty += it->second;
        }
        for (auto it = asks.begin(); it != asks.end(); ++it) {
            if (it->first > upper) break;
            if (it->first >= lower) ask_qty += it->second;
        }
        double sum = bid_qty + ask_qty;
        return sum == 0.0 ? 0.0 : (bid_qty - ask_qty) / sum;
    }

    double additions_window(int window_sec, bool is_ask) const {
        if (last_sec < 0) return 0.0;
        double total = 0;
        int w = std::min(window_sec, NUM_BUCKETS);
        for (int i = 0; i < w; ++i) {
            long long sec = last_sec - i;
            if (sec < 0) break;
            const auto& b = buckets[sec % NUM_BUCKETS];
            total += is_ask ? b.ask_additions : b.bid_additions;
        }
        return total;
    }

    double cancels_window(int window_sec, bool is_ask) const {
        if (last_sec < 0) return 0.0;
        double total = 0;
        int w = std::min(window_sec, NUM_BUCKETS);
        for (int i = 0; i < w; ++i) {
            long long sec = last_sec - i;
            if (sec < 0) break;
            const auto& b = buckets[sec % NUM_BUCKETS];
            total += is_ask ? b.ask_cancels : b.bid_cancels;
        }
        return total;
    }

    double net_additions_window(int window_sec, bool is_ask) const {
        return additions_window(window_sec, is_ask) -
               cancels_window(window_sec, is_ask);
    }
};

}  // namespace MarketData
