#include "market_data/state_machine_registry.h"
#include "historical_feature_gen/transformations/polymarket_trade_flow_sm.h"

namespace HistoricalFeatureGen {

void PolymarketTradeFlowSM::clear_stale_buckets(OutcomeState& state,
                                                  long long current_sec) const {
    if (state.last_sec < 0) {
        for (auto& b : state.buckets) b.clear();
        state.last_sec = current_sec;
        return;
    }

    long long gap = current_sec - state.last_sec;
    if (gap <= 0) return;

    if (gap >= NUM_BUCKETS) {
        for (auto& b : state.buckets) b.clear();
    } else {
        for (long long s = state.last_sec + 1; s <= current_sec; ++s) {
            state.buckets[static_cast<size_t>(s % NUM_BUCKETS)].clear();
        }
    }
    state.last_sec = current_sec;
}

void PolymarketTradeFlowSM::close_event(const std::string& condition_id,
                                          long long close_ts) {
    if (already_closed_.count(condition_id)) return;

    auto it = event_states_.find(condition_id);
    if (it == event_states_.end()) {
        already_closed_.insert(condition_id);
        return;
    }

    auto& es = it->second;
    ClosedEventProfile profile;
    profile.condition_id = condition_id;
    profile.close_time_ms = close_ts;

    double duration_ms =
        static_cast<double>(es.last_trade_ts_ms - es.first_trade_ts_ms);
    profile.duration_sec = duration_ms > 0 ? duration_ms / 1000.0 : 0.0;

    for (const auto& outcome : es.outcomes) {
        profile.total_buy_volume += outcome.cumulative_buy;
        profile.total_sell_volume += outcome.cumulative_sell;
        profile.total_trade_count += outcome.cumulative_count;
    }

    profile.total_volume = profile.total_buy_volume + profile.total_sell_volume;
    profile.volume_per_sec =
        profile.duration_sec > 0
            ? profile.total_volume / profile.duration_sec
            : 0.0;
    profile.buy_ratio =
        profile.total_volume > 0
            ? profile.total_buy_volume / profile.total_volume
            : 0.5;
    profile.trades_per_sec =
        profile.duration_sec > 0
            ? profile.total_trade_count / profile.duration_sec
            : 0.0;

    closed_history_.push_back(std::move(profile));
    if (static_cast<int>(closed_history_.size()) > MAX_CLOSED_HISTORY) {
        closed_history_.pop_front();
    }

    for (const auto& [asset_id, _] : es.asset_to_outcome) {
        asset_to_condition_.erase(asset_id);
    }
    event_states_.erase(it);
    already_closed_.insert(condition_id);
}

void PolymarketTradeFlowSM::react_impl(
    const MarketData::MarketEvent& event) {
    if (event.type == MarketData::MarketEvent::POLYMARKET_LAST_TRADE) {
        const auto& trade = event.polymarket_last_trade.value();
        long long ts_sec = trade.timestamp_ms / 1000;

        std::unique_lock<std::shared_mutex> lock(map_mutex_);

        auto& es = event_states_[trade.condition_id];
        if (es.condition_id.empty()) {
            es.condition_id = trade.condition_id;
        }
        if (es.first_trade_ts_ms == 0) {
            es.first_trade_ts_ms = trade.timestamp_ms;
        }
        es.last_trade_ts_ms = trade.timestamp_ms;

        auto outcome_it = es.asset_to_outcome.find(trade.asset_id);
        int outcome_idx;
        if (outcome_it == es.asset_to_outcome.end()) {
            outcome_idx = static_cast<int>(es.outcomes.size());
            es.asset_to_outcome[trade.asset_id] = outcome_idx;
            es.outcomes.emplace_back();
            asset_to_condition_[trade.asset_id] = trade.condition_id;
        } else {
            outcome_idx = outcome_it->second;
        }

        auto& outcome = es.outcomes[outcome_idx];
        clear_stale_buckets(outcome, ts_sec);

        auto& bucket =
            outcome.buckets[static_cast<size_t>(ts_sec % NUM_BUCKETS)];
        if (trade.is_buy) {
            bucket.buy_volume += trade.size;
            outcome.cumulative_buy += trade.size;
        } else {
            bucket.sell_volume += trade.size;
            outcome.cumulative_sell += trade.size;
        }
        bucket.trade_count++;
        outcome.cumulative_count++;

    } else if (event.type ==
               MarketData::MarketEvent::POLYMARKET_MARKET_RESOLVED) {
        const auto& resolved = event.polymarket_market_resolved.value();

        std::unique_lock<std::shared_mutex> lock(map_mutex_);
        close_event(resolved.condition_id, resolved.timestamp_ms);
    }
}

std::optional<PolymarketTradeFlowSM::FlowSnapshot>
PolymarketTradeFlowSM::get_flow(const std::string& asset_id) const {
    std::shared_lock<std::shared_mutex> lock(map_mutex_);

    auto cond_it = asset_to_condition_.find(asset_id);
    if (cond_it == asset_to_condition_.end()) return std::nullopt;

    auto event_it = event_states_.find(cond_it->second);
    if (event_it == event_states_.end()) return std::nullopt;

    const auto& es = event_it->second;
    auto outcome_it = es.asset_to_outcome.find(asset_id);
    if (outcome_it == es.asset_to_outcome.end()) return std::nullopt;

    int idx = outcome_it->second;
    if (idx < 0 || idx >= static_cast<int>(es.outcomes.size()))
        return std::nullopt;

    const auto& state = es.outcomes[idx];
    if (state.last_sec < 0) return std::nullopt;

    FlowSnapshot snap{};
    for (int wi = 0; wi < NUM_WINDOWS; ++wi) {
        int window = WINDOWS_SEC[wi];
        double buy = 0.0;
        double sell = 0.0;
        int count = 0;

        long long start_sec = state.last_sec - window + 1;
        for (long long sec = start_sec; sec <= state.last_sec; ++sec) {
            if (sec < 0) continue;
            const auto& b =
                state.buckets[static_cast<size_t>(sec % NUM_BUCKETS)];
            buy += b.buy_volume;
            sell += b.sell_volume;
            count += b.trade_count;
        }

        snap.buy_volume[wi] = buy;
        snap.sell_volume[wi] = sell;
        snap.ofi[wi] = buy - sell;
        snap.trade_count[wi] = count;
    }

    return snap;
}

std::optional<PolymarketTradeFlowSM::ClosedEventProfile>
PolymarketTradeFlowSM::get_closed_avg(int n) const {
    std::shared_lock<std::shared_mutex> lock(map_mutex_);

    if (closed_history_.empty()) return std::nullopt;

    int count = (n <= 0 || n > static_cast<int>(closed_history_.size()))
                    ? static_cast<int>(closed_history_.size())
                    : n;

    ClosedEventProfile avg{};
    auto it = closed_history_.end();
    for (int i = 0; i < count; ++i) {
        --it;
        avg.total_buy_volume += it->total_buy_volume;
        avg.total_sell_volume += it->total_sell_volume;
        avg.total_volume += it->total_volume;
        avg.volume_per_sec += it->volume_per_sec;
        avg.buy_ratio += it->buy_ratio;
        avg.total_trade_count += it->total_trade_count;
        avg.trades_per_sec += it->trades_per_sec;
        avg.duration_sec += it->duration_sec;
    }

    double d = static_cast<double>(count);
    avg.total_buy_volume /= d;
    avg.total_sell_volume /= d;
    avg.total_volume /= d;
    avg.volume_per_sec /= d;
    avg.buy_ratio /= d;
    avg.total_trade_count = static_cast<int>(avg.total_trade_count / d);
    avg.trades_per_sec /= d;
    avg.duration_sec /= d;

    return avg;
}

// --- Persistence ---

void PolymarketTradeFlowSM::serialize_state_impl(std::ostream& out) const {
    std::shared_lock<std::shared_mutex> lock(map_mutex_);

    uint32_t version = 1;
    out.write(reinterpret_cast<const char*>(&version), sizeof(version));

    uint32_t count = static_cast<uint32_t>(closed_history_.size());
    out.write(reinterpret_cast<const char*>(&count), sizeof(count));

    for (const auto& p : closed_history_) {
        uint32_t kl = static_cast<uint32_t>(p.condition_id.size());
        out.write(reinterpret_cast<const char*>(&kl), sizeof(kl));
        out.write(p.condition_id.data(), kl);
        out.write(reinterpret_cast<const char*>(&p.total_buy_volume),
                  sizeof(double));
        out.write(reinterpret_cast<const char*>(&p.total_sell_volume),
                  sizeof(double));
        out.write(reinterpret_cast<const char*>(&p.total_volume),
                  sizeof(double));
        out.write(reinterpret_cast<const char*>(&p.volume_per_sec),
                  sizeof(double));
        out.write(reinterpret_cast<const char*>(&p.buy_ratio), sizeof(double));
        out.write(reinterpret_cast<const char*>(&p.total_trade_count),
                  sizeof(int));
        out.write(reinterpret_cast<const char*>(&p.trades_per_sec),
                  sizeof(double));
        out.write(reinterpret_cast<const char*>(&p.duration_sec),
                  sizeof(double));
        out.write(reinterpret_cast<const char*>(&p.close_time_ms),
                  sizeof(long long));
    }
}

bool PolymarketTradeFlowSM::deserialize_state_impl(std::istream& in) {
    std::unique_lock<std::shared_mutex> lock(map_mutex_);

    uint32_t version = 0;
    in.read(reinterpret_cast<char*>(&version), sizeof(version));
    if (version != 1) return false;

    uint32_t count = 0;
    in.read(reinterpret_cast<char*>(&count), sizeof(count));
    if (count > 10000) return false;

    closed_history_.clear();
    already_closed_.clear();

    for (uint32_t i = 0; i < count; ++i) {
        ClosedEventProfile p;
        uint32_t kl = 0;
        in.read(reinterpret_cast<char*>(&kl), sizeof(kl));
        if (kl > 1000) return false;
        p.condition_id.resize(kl);
        in.read(p.condition_id.data(), kl);
        in.read(reinterpret_cast<char*>(&p.total_buy_volume), sizeof(double));
        in.read(reinterpret_cast<char*>(&p.total_sell_volume), sizeof(double));
        in.read(reinterpret_cast<char*>(&p.total_volume), sizeof(double));
        in.read(reinterpret_cast<char*>(&p.volume_per_sec), sizeof(double));
        in.read(reinterpret_cast<char*>(&p.buy_ratio), sizeof(double));
        in.read(reinterpret_cast<char*>(&p.total_trade_count), sizeof(int));
        in.read(reinterpret_cast<char*>(&p.trades_per_sec), sizeof(double));
        in.read(reinterpret_cast<char*>(&p.duration_sec), sizeof(double));
        in.read(reinterpret_cast<char*>(&p.close_time_ms), sizeof(long long));

        if (!in.good()) return false;

        already_closed_.insert(p.condition_id);
        closed_history_.push_back(std::move(p));
    }

    return true;
}

}  // namespace HistoricalFeatureGen

namespace HistoricalFeatureGen {
KERONIS_REGISTER_STATE_MACHINE("polymarket_trade_flow", PolymarketTradeFlowSM);
}  // namespace HistoricalFeatureGen
