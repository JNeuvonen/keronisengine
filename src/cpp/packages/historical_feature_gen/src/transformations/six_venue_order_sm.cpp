#include "market_data/state_machine_registry.h"
#include "historical_feature_gen/transformations/six_venue_order_sm.h"

#include <algorithm>
#include <cmath>

namespace HistoricalFeatureGen {

void SixVenueOrderSM::initialize_symbol_states() {
    static constexpr const char* symbols[NUM_ASSETS][NUM_VENUES] = {
        {"XRPUSDT", "XRPUSDT", "XRP-USD", "XRP_USDT", "XRP", "XRP-USDT-SWAP"},
        {"SOLUSDT", "SOLUSDT", "SOL-USD", "SOL_USDT", "SOL", "SOL-USDT-SWAP"},
    };
    static constexpr const char* exchanges[NUM_VENUES] = {
        "binance", "bybit", "coinbase", "gateio", "hyperliquid", "okx"};
    static constexpr const char* markets[NUM_VENUES] = {
        "swap", "swap", "spot", "swap", "swap", "swap"};
    key_to_idx_.clear();
    feeds_ = {};
    latest_ts_ms_ = 0;
    for (int asset = 0; asset < NUM_ASSETS; ++asset) {
        for (int venue = 0; venue < NUM_VENUES; ++venue) {
            int idx = asset * NUM_VENUES + venue;
            auto& feed = feeds_[idx];
            feed.symbol = symbols[asset][venue];
            feed.exchange = exchanges[venue];
            feed.market_type = markets[venue];
            key_to_idx_[feed.symbol + ":" + feed.exchange + ":" + feed.market_type] = idx;
        }
    }
}

std::vector<MarketData::SMMarketDataDependency>
SixVenueOrderSM::get_market_data_dependencies() const {
    std::vector<MarketData::SMMarketDataDependency> deps;
    deps.reserve(NUM_FEEDS * 3);
    for (const auto& feed : feeds_) {
        for (auto type : {MarketData::MarketEvent::BBA,
                          MarketData::MarketEvent::L2_UPDATE,
                          MarketData::MarketEvent::TRADE}) {
            MarketData::SMMarketDataDependency dep;
            dep.symbol = feed.symbol;
            dep.exchange_name = feed.exchange;
            dep.market_type = feed.market_type;
            dep.event_type = type;
            deps.push_back(std::move(dep));
        }
    }
    return deps;
}

void SixVenueOrderSM::react_impl(const MarketData::MarketEvent& event) {
    std::string key;
    long long ts = 0;
    if (event.type == MarketData::MarketEvent::BBA && event.bba) {
        const auto& b = *event.bba;
        key = b.symbol + ":" + b.exchange_name + ":" + b.market_type;
        ts = b.timestamp_ms;
    } else if (event.type == MarketData::MarketEvent::TRADE && event.trade) {
        const auto& t = *event.trade;
        key = t.symbol + ":" + t.exchange_name + ":" + t.market_type;
        ts = t.timestamp_ms;
    } else if (event.type == MarketData::MarketEvent::L2_UPDATE && event.l2_update) {
        const auto& l = *event.l2_update;
        key = l.symbol + ":" + l.exchange_name + ":" + l.market_type;
        ts = l.update.timestamp;
    } else {
        return;
    }
    auto it = key_to_idx_.find(key);
    if (it == key_to_idx_.end()) return;
    auto& feed = feeds_[it->second];
    latest_ts_ms_ = std::max(latest_ts_ms_, ts);

    if (event.type == MarketData::MarketEvent::BBA) {
        const auto& b = *event.bba;
        if (b.bid_price <= 0.0 || b.ask_price <= b.bid_price ||
            b.bid_qty < 0.0 || b.ask_qty < 0.0) return;
        feed.bid = b.bid_price;
        feed.ask = b.ask_price;
        feed.bid_qty = b.bid_qty;
        feed.ask_qty = b.ask_qty;
        feed.bba_ts_ms = ts;
    } else if (event.type == MarketData::MarketEvent::TRADE) {
        const auto& t = *event.trade;
        if (t.price <= 0.0 || t.qty <= 0.0) return;
        auto& bucket = feed.flow[(ts / 1000) % FLOW_BUCKETS];
        if (bucket.second != ts / 1000) {
            bucket = {};
            bucket.second = ts / 1000;
        }
        double notional = t.price * t.qty;
        if (t.is_buyer_maker) bucket.sell += notional;
        else bucket.buy += notional;
        feed.trade_ts_ms = ts;
    } else {
        const auto& l = *event.l2_update;
        if (l.sub_type == MarketData::L2UpdateEvent::FULL_REFRESH &&
            feed.snapshot_ts_ms != ts) {
            feed.bids.clear();
            feed.asks.clear();
            feed.has_snapshot = true;
            feed.snapshot_ts_ms = ts;
        }
        if (!feed.has_snapshot) return;
        for (const auto& [price, qty] : l.update.bids) {
            if (qty <= 0.0) feed.bids.erase(price);
            else feed.bids[price] = qty;
        }
        for (const auto& [price, qty] : l.update.asks) {
            if (qty <= 0.0) feed.asks.erase(price);
            else feed.asks[price] = qty;
        }
        feed.l2_ts_ms = ts;
    }
}

std::optional<std::vector<float>> SixVenueOrderSM::emit_state_impl() const {
    return std::nullopt;
}

std::optional<std::vector<float>> SixVenueOrderSM::emit_state_for_symbol(int asset) const {
    auto guard = lock_state_exclusive();
    if (asset < 0 || asset >= NUM_ASSETS) return std::nullopt;
    const auto& primary = feeds_[asset * NUM_VENUES];
    if (primary.bba_ts_ms == 0 || latest_ts_ms_ - primary.bba_ts_ms > 1000)
        return std::nullopt;

    std::vector<float> result;
    result.reserve(NUM_VENUES * 13 + 4);
    for (int venue = 0; venue < NUM_VENUES; ++venue) {
        const auto& f = feeds_[asset * NUM_VENUES + venue];
        if (f.bba_ts_ms == 0 || f.l2_ts_ms == 0 || f.trade_ts_ms == 0 ||
            latest_ts_ms_ - f.bba_ts_ms > 5000 ||
            latest_ts_ms_ - f.l2_ts_ms > 5000 ||
            latest_ts_ms_ - f.trade_ts_ms > 30000 ||
            !f.has_snapshot) return std::nullopt;
        double mid = (f.bid + f.ask) / 2.0;
        double touch_total = f.bid_qty + f.ask_qty;
        double micro = touch_total > 0.0
            ? (f.ask * f.bid_qty + f.bid * f.ask_qty) / touch_total : mid;
        double bid_depth = 0.0, ask_depth = 0.0;
        for (auto it = f.bids.rbegin(); it != f.bids.rend(); ++it) {
            if (it->first < mid * 0.999) break;
            bid_depth += it->second * it->first;
        }
        for (auto it = f.asks.begin(); it != f.asks.end(); ++it) {
            if (it->first > mid * 1.001) break;
            ask_depth += it->second * it->first;
        }
        double depth_total = bid_depth + ask_depth;
        double buy[3] = {}, sell[3] = {};
        for (int age = 0; age < 30; ++age) {
            long long sec = latest_ts_ms_ / 1000 - age;
            const auto& bucket = f.flow[sec % FLOW_BUCKETS];
            if (bucket.second != sec) continue;
            for (int window = 0; window < 3; ++window) {
                if (age >= (window == 0 ? 1 : window == 1 ? 5 : 30)) continue;
                buy[window] += bucket.buy;
                sell[window] += bucket.sell;
            }
        }
        result.push_back(static_cast<float>((f.ask - f.bid) / mid * 10000));
        result.push_back(static_cast<float>(touch_total > 0.0 ?
            (f.bid_qty - f.ask_qty) / touch_total : 0.0));
        result.push_back(static_cast<float>((micro - mid) / mid * 10000));
        result.push_back(static_cast<float>(depth_total > 0.0 ?
            (bid_depth - ask_depth) / depth_total : 0.0));
        result.push_back(static_cast<float>(bid_depth));
        result.push_back(static_cast<float>(ask_depth));
        for (int window = 0; window < 3; ++window) {
            double total = buy[window] + sell[window];
            result.push_back(static_cast<float>(total > 0.0 ?
                (buy[window] - sell[window]) / total : 0.0));
        }
        result.push_back(static_cast<float>(std::log1p(buy[1] + sell[1])));
        result.push_back(static_cast<float>(latest_ts_ms_ - f.bba_ts_ms));
        result.push_back(static_cast<float>(latest_ts_ms_ - f.l2_ts_ms));
        result.push_back(static_cast<float>(latest_ts_ms_ - f.trade_ts_ms));
    }
    result.push_back(static_cast<float>(primary.bid));
    result.push_back(static_cast<float>(primary.ask));
    result.push_back(static_cast<float>(primary.bid_qty));
    result.push_back(static_cast<float>(primary.ask_qty));
    return result;
}

std::vector<std::string> SixVenueOrderSM::feature_names_for_symbol(int asset) const {
    (void)asset;
    static constexpr const char* venues[NUM_VENUES] = {
        "binance", "bybit", "coinbase", "gateio", "hyperliquid", "okx"};
    static constexpr const char* names[] = {
        "spread_bp", "bba_imbalance", "microprice_bp", "l2_imbalance_10bp",
        "bid_depth_usd_10bp", "ask_depth_usd_10bp",
        "ofi_1s", "ofi_5s", "ofi_30s", "log_trade_notional_5s",
        "bba_age_ms", "l2_age_ms", "trade_age_ms"};
    std::vector<std::string> out;
    out.reserve(NUM_VENUES * 13 + 4);
    for (const auto* venue : venues)
        for (const auto* name : names)
            out.push_back(std::string(venue) + ":" + name);
    for (const auto* name : {"bid", "ask", "bid_qty", "ask_qty"})
        out.push_back(std::string("binance:") + name);
    return out;
}

long long SixVenueOrderSM::get_symbol_latest_event_ts_ms(int asset) const {
    if (asset < 0 || asset >= NUM_ASSETS) return 0;
    return feeds_[asset * NUM_VENUES].bba_ts_ms;
}

}  // namespace HistoricalFeatureGen

namespace HistoricalFeatureGen {
KERONIS_REGISTER_STATE_MACHINE("six_venue_order", SixVenueOrderSM);
}  // namespace HistoricalFeatureGen
