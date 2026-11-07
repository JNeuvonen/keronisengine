#include "market_data/sm_syncer.h"

#include <thread>

#include "ccxt_proxy.h"
#include "common_types/log_event_types.h"
#include "disk_logging/log.h"
#include "market_data/normalized_trade.h"
#include "market_data/state_machine_dispatcher.h"

namespace MarketData {

MarketEvent StateMachineSyncer::trade_to_market_event(
    const NormalizedParsedTrade& trade, const std::string& exchange_name,
    const std::string& market_type) {
    MarketEvent event;
    event.type = MarketEvent::TRADE;
    event.trade = TradeEvent{
        trade.symbol,   exchange_name,        market_type,    trade.price,
        trade.quantity, trade.is_buyer_maker, trade.timestamp};
    return event;
}

void StateMachineSyncer::sync_single_dependency(
    const std::shared_ptr<StrategyStateMachine>& sm,
    SMMarketDataDependency& dep) {
    if (dep.is_synced) return;
    if (dep.is_optional) return;  // gate doesn't wait on optional deps
    if (dep.first_live_event_ts_ms == 0) return;

    if (dep.event_type != MarketEvent::TRADE) {
        return;
    }

    int iters = 0;
    int zero_count = 0;
    long long total_trades = 0;
    std::string last_trade_id = dep.last_synced_trade_id;

    // Detect stale trade_id: if saved ID maps to trades behind the watermark,
    // discard it and re-bootstrap from timestamp
    if (!last_trade_id.empty() && dep.last_synced_ts_ms > 0) {
        auto probe = CcxtProxy::instance().get_historical_trades_by_id(
            dep.exchange_name, dep.market_type, dep.symbol, last_trade_id,
            RateLimiter::RequestClass::SM_SYNC);
        if (!probe.empty() && probe.back().timestamp < dep.last_synced_ts_ms) {
            LOG_WARNING(
                Core::TRADING_LOG_EVENT_TYPE_SM_SYNCER_STALE_TRADE_ID,
                "SM sync: trade_id " + last_trade_id + " is stale for " +
                    dep.get_key() +
                    " (ts=" + std::to_string(probe.back().timestamp) +
                    " < synced_ts=" + std::to_string(dep.last_synced_ts_ms) +
                    ") — re-bootstrapping from timestamp",
                (nlohmann::json{{"dep_key", dep.get_key()},
                                {"stale_trade_id", last_trade_id},
                                {"probe_ts_ms", probe.back().timestamp},
                                {"synced_ts_ms", dep.last_synced_ts_ms}}));
            last_trade_id.clear();
            dep.last_synced_trade_id.clear();
        }
    }

    if (last_trade_id.empty()) {
        if (dep.last_synced_ts_ms == 0) {
            return;
        }
        long long from_ts = dep.last_synced_ts_ms;

        auto initial_trades = CcxtProxy::instance().get_historical_trades(
            dep.exchange_name, dep.market_type, dep.symbol, from_ts);
        if (!initial_trades.empty()) {
            last_trade_id = initial_trades.back().trade_id;
            for (const auto& trade : initial_trades) {
                auto event = trade_to_market_event(trade, dep.exchange_name,
                                                   dep.market_type);
                sm->react(event, true);
            }
            total_trades += initial_trades.size();
        }
    }

    if (last_trade_id.empty()) {
        return;
    }

    if (!dep.last_synced_trade_id.empty()) {
        LOG_INFO(
            Core::TRADING_LOG_EVENT_TYPE_SM_SYNCER_PROGRESS,
            "SM sync: resuming " + dep.get_key() +
                " from trade_id=" + dep.last_synced_trade_id +
                " ts=" + std::to_string(dep.last_synced_ts_ms) +
                " to live_ts=" + std::to_string(dep.first_live_event_ts_ms),
            (nlohmann::json{{"dep_key", dep.get_key()},
                            {"phase", "resumed"},
                            {"from_trade_id", dep.last_synced_trade_id},
                            {"from_ts_ms", dep.last_synced_ts_ms},
                            {"live_ts_ms", dep.first_live_event_ts_ms}}));
    }

    long long ts_before_loop = dep.last_synced_ts_ms;

    while (dep.last_synced_ts_ms < dep.first_live_event_ts_ms) {
        auto trades = CcxtProxy::instance().get_historical_trades_by_id(
            dep.exchange_name, dep.market_type, dep.symbol, last_trade_id,
            RateLimiter::RequestClass::SM_SYNC);

        std::this_thread::sleep_for(std::chrono::milliseconds(1000));

        if (trades.empty()) {
            zero_count++;
            if (zero_count > 2) break;
            continue;
        }

        long long batch_first_ts = trades.front().timestamp;
        long long batch_last_ts = trades.back().timestamp;
        last_trade_id = trades.back().trade_id;

        for (const auto& trade : trades) {
            auto event = trade_to_market_event(trade, dep.exchange_name,
                                               dep.market_type);
            sm->react(event, true);
        }
        total_trades += trades.size();

        // SM may have gone live during react() if this was the last dep
        if (sm->is_fully_synced()) {
            LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_SM_WENT_LIVE,
                     "SM sync: went live during sync of " + dep.get_key() +
                         " total_trades=" + std::to_string(total_trades),
                     (nlohmann::json{{"sm_name", sm->get_name()},
                                     {"dep_key", dep.get_key()},
                                     {"total_trades", total_trades},
                                     {"from_state", "syncing"},
                                     {"to_state", "live"},
                                     {"is_real_transition", true},
                                     {"trigger", "during_dep_sync"}}));
            return;
        }

        // Pacing now lives in CcxtProxy via RateLimiter::should_throttle.
        ++iters;
        if (iters >= 100) {
            long long lag_ms =
                dep.first_live_event_ts_ms - dep.last_synced_ts_ms;
            double lag_minutes = static_cast<double>(lag_ms) / 60000.0;
            LOG_INFO(
                Core::TRADING_LOG_EVENT_TYPE_SM_SYNCER_PROGRESS,
                "SM sync: pausing " + dep.get_key() + " after " +
                    std::to_string(iters) + " iters, " +
                    std::to_string(total_trades) + " trades, lag: " +
                    std::to_string(lag_minutes) + " min" + " ts_advanced=" +
                    std::to_string(dep.last_synced_ts_ms - ts_before_loop) +
                    "ms (will continue next cycle)",
                (nlohmann::json{{"dep_key", dep.get_key()},
                                {"phase", "iter_cap_paused"},
                                {"iters", iters},
                                {"total_trades", total_trades},
                                {"lag_ms", lag_ms},
                                {"ts_advanced_ms",
                                 dep.last_synced_ts_ms - ts_before_loop}}));
            break;
        }
    }

    dep.last_synced_trade_id = last_trade_id;
    long long remaining_lag_ms =
        dep.first_live_event_ts_ms - dep.last_synced_ts_ms;
    double remaining_lag_min = static_cast<double>(remaining_lag_ms) / 60000.0;
    LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_SM_SYNCER_PROGRESS,
             "SM sync: " + dep.get_key() +
                 " total_trades=" + std::to_string(total_trades) +
                 " remaining_lag: " + std::to_string(remaining_lag_min) +
                 " min",
             (nlohmann::json{{"dep_key", dep.get_key()},
                             {"phase", "dep_complete"},
                             {"total_trades", total_trades},
                             {"remaining_lag_ms", remaining_lag_ms}}));
}

void StateMachineSyncer::sync_state_machines() {
    auto machines = StateMachineDispatcher::get_instance().get_machines();

    // Warmup-timeout thresholds — 5 minutes to cross the gate, 5 minutes
    // between repeat emits per stuck SM so a persistent stall shows a
    // steady drumbeat rather than flooding the log.
    constexpr auto WARMUP_TIMEOUT_THRESHOLD = std::chrono::minutes(5);
    constexpr auto WARMUP_TIMEOUT_EMIT_COOLDOWN = std::chrono::minutes(5);
    const auto now_steady = std::chrono::steady_clock::now();

    for (auto& sm : machines) {
        void* key = sm.get();
        if (sm->is_fully_synced()) {
            warmup_start_time_.erase(key);
            last_warmup_timeout_emit_.erase(key);
            continue;
        }

        // First time we see this SM unsynced, stamp the start. Subsequent
        // iterations compare against it to detect stuck warmup.
        auto start_it = warmup_start_time_.find(key);
        if (start_it == warmup_start_time_.end()) {
            warmup_start_time_[key] = now_steady;
        } else {
            const auto age = now_steady - start_it->second;
            if (age >= WARMUP_TIMEOUT_THRESHOLD) {
                auto last_emit_it = last_warmup_timeout_emit_.find(key);
                const bool emit_allowed =
                    last_emit_it == last_warmup_timeout_emit_.end() ||
                    (now_steady - last_emit_it->second) >=
                        WARMUP_TIMEOUT_EMIT_COOLDOWN;
                if (emit_allowed) {
                    last_warmup_timeout_emit_[key] = now_steady;
                    auto [synced, total] = sm->get_sync_progress();
                    const long long age_s =
                        std::chrono::duration_cast<std::chrono::seconds>(age)
                            .count();
                }
            }
        }

        sm->init_dependencies();

        if (!loaded_sms_.count(sm.get())) {
            loaded_sms_.insert(sm.get());
            if (sm->deserialize_state()) {
                LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_SM_STATE_BIN_LOADED,
                         "SM sync: loaded persisted state from disk",
                         (nlohmann::json{{"sm_name", sm->get_name()}}));
            }
            if (sm->is_fully_synced()) {
                LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_SM_WENT_LIVE,
                         "SM sync: fully synced from persisted state",
                         (nlohmann::json{{"sm_name", sm->get_name()},
                                         {"from_state", "loading"},
                                         {"to_state", "live"},
                                         {"is_real_transition", true},
                                         {"trigger", "persisted_state"}}));
                continue;
            }
        }

        auto [synced, total] = sm->get_sync_progress();
        LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_SM_SYNCER_PROGRESS,
                 "SM sync: " + std::to_string(synced) + "/" +
                     std::to_string(total) + " deps synced",
                 (nlohmann::json{{"sm_name", sm->get_name()},
                                 {"phase", "deps_progress"},
                                 {"deps_synced", synced},
                                 {"deps_total", total}}));

        auto* dep = sm->get_next_unsynced_dependency();
        if (!dep) continue;

        sync_single_dependency(sm, *dep);
    }
}

void StateMachineSyncer::persist_state() {
    auto machines = StateMachineDispatcher::get_instance().get_machines();
    const auto now = std::chrono::steady_clock::now();
    for (auto& sm : machines) {
        const auto interval = std::chrono::milliseconds(sm->persist_interval_ms());
        auto it = last_persist_.find(sm.get());
        if (it != last_persist_.end() && now - it->second < interval) continue;
        sm->serialize_state();
        last_persist_[sm.get()] = now;
    }
}

}  // namespace MarketData
