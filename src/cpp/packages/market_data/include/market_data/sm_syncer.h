#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "market_data/normalized_trade.h"
#include "market_data/sm_market_data_dependency.h"
#include "market_data/strategy_state_machine.h"

namespace MarketData {

class StateMachineSyncer {
   public:
    // Called periodically from a sync thread
    void sync_state_machines();

    // Persist all SM states to disk
    void persist_state();

   private:
    void sync_single_dependency(
        const std::shared_ptr<StrategyStateMachine>& sm,
        SMMarketDataDependency& dep);

    MarketEvent trade_to_market_event(const NormalizedParsedTrade& trade,
                                      const std::string& exchange_name,
                                      const std::string& market_type);

    std::unordered_set<void*> loaded_sms_;
    // Per-SM (void*) first-seen-unsynced timestamp — cleared when SM goes
    // live. Used by sync_state_machines() to emit SM_WARMUP_TIMEOUT if an
    // SM stays unsynced for longer than the warmup threshold.
    std::unordered_map<void*, std::chrono::steady_clock::time_point>
        warmup_start_time_;
    // Per-SM last SM_WARMUP_TIMEOUT emit ts, so a persistently-stuck SM
    // doesn't flood the log — one row every 5 minutes per SM.
    std::unordered_map<void*, std::chrono::steady_clock::time_point>
        last_warmup_timeout_emit_;
    std::unordered_map<void*, std::chrono::steady_clock::time_point>
        last_persist_;
};

}  // namespace MarketData
