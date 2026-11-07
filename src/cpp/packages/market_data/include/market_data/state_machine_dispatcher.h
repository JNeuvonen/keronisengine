#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "concurrent_queue_include_helper.h"
#include "market_data/market_event.h"
#include "market_data/strategy_state_machine.h"

namespace MarketData {

class StateMachineDispatcher {
   public:
    static StateMachineDispatcher& get_instance();

    // Enqueue events (called from trade/BBA consumer threads)
    void enqueue(MarketEvent event);

    // Register/unregister a strategy's state machine (no symbol key)
    void register_machine(std::shared_ptr<StrategyStateMachine> machine);
    void unregister_machine(const std::shared_ptr<StrategyStateMachine>& machine);

    // Start/stop the consumer thread
    void start();
    void stop();

    bool is_running() const;

    // Get snapshot of registered machines (for SM syncer)
    std::vector<std::shared_ptr<StrategyStateMachine>> get_machines();

    // O(1) lookup by name — lockless, safe after startup
    std::shared_ptr<StrategyStateMachine> get_machine(const std::string& name) const;

   private:
    StateMachineDispatcher() = default;
    ~StateMachineDispatcher();

    StateMachineDispatcher(const StateMachineDispatcher&) = delete;
    StateMachineDispatcher& operator=(const StateMachineDispatcher&) = delete;

    void consumer_loop();

    moodycamel::BlockingConcurrentQueue<MarketEvent> queue_;
    std::vector<std::shared_ptr<StrategyStateMachine>> machines_;
    std::unordered_map<std::string, std::shared_ptr<StrategyStateMachine>> machines_by_name_;
    std::mutex machines_mutex_;
    std::atomic<uint64_t> machines_version_{0};
    std::atomic<bool> running_{false};
    std::thread consumer_thread_;
};

}  // namespace MarketData
