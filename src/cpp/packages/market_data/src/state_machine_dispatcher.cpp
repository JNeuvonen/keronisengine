#include "market_data/state_machine_dispatcher.h"

#include <algorithm>
#include <chrono>
#include <iostream>

#include "market_data/config.h"

namespace MarketData {

StateMachineDispatcher& StateMachineDispatcher::get_instance() {
    static StateMachineDispatcher instance;
    return instance;
}

StateMachineDispatcher::~StateMachineDispatcher() { stop(); }

void StateMachineDispatcher::enqueue(MarketEvent event) {
    // Drop on the floor when not running. With MD_ENABLE_STATE_MACHINES=0
    // start() is never called, so consumer_loop never reads — without this
    // guard the BBA pipeline (~500/s) would leak the queue unboundedly.
    if (!running_.load(std::memory_order_relaxed)) return;
    queue_.enqueue(std::move(event));
}

void StateMachineDispatcher::register_machine(
    std::shared_ptr<StrategyStateMachine> machine) {
    // Honor the global kill switch at the single chokepoint. Without this
    // gate, per-strategy SMs constructed inside strategy.cpp dispatch
    // binders leak past the register_live_sms.cpp early-return and end up
    // syncing/persisting
    // every second. Callers that look up by name via get_machine() must be
    // nullptr-tolerant when SMs are disabled — see watch_state_machine() for
    // the canonical pattern.
    if (!Config::getInstance().get_is_state_machines_enabled()) {
        std::cout << "[SM] register_machine no-op (SMs globally disabled): "
                  << machine->get_name() << std::endl;
        return;
    }
    std::lock_guard<std::mutex> lock(machines_mutex_);
    std::cout << "[SM] register_machine: " << machine->get_name() << std::endl;
    machines_by_name_[machine->get_name()] = machine;
    machines_.push_back(std::move(machine));
    machines_version_.fetch_add(1, std::memory_order_release);
}

void StateMachineDispatcher::unregister_machine(
    const std::shared_ptr<StrategyStateMachine>& machine) {
    std::lock_guard<std::mutex> lock(machines_mutex_);
    auto it = std::find(machines_.begin(), machines_.end(), machine);
    if (it != machines_.end()) {
        std::cout << "[SM] unregister_machine: " << machine->get_name()
                  << std::endl;
        machines_by_name_.erase(machine->get_name());
        machines_.erase(it);
        machines_version_.fetch_add(1, std::memory_order_release);
    }
}

void StateMachineDispatcher::start() {
    if (running_.exchange(true)) {
        return;  // already running
    }
    consumer_thread_ = std::thread([this]() { consumer_loop(); });
}

void StateMachineDispatcher::stop() {
    if (!running_.exchange(false)) {
        return;  // already stopped
    }
    if (consumer_thread_.joinable()) {
        consumer_thread_.join();
    }
}

bool StateMachineDispatcher::is_running() const { return running_.load(); }

void StateMachineDispatcher::consumer_loop() {
    MarketEvent event;
    std::vector<std::shared_ptr<StrategyStateMachine>> snapshot;
    uint64_t snapshot_version = 0;
    auto last_log = std::chrono::steady_clock::now();
    uint64_t events_processed = 0;

    while (running_.load(std::memory_order_relaxed)) {
        if (queue_.wait_dequeue_timed(event, std::chrono::milliseconds(100))) {
            ++events_processed;

            // // Re-snapshot only when machines vector changed
            uint64_t current_version =
                machines_version_.load(std::memory_order_acquire);
            if (current_version != snapshot_version) {
                std::lock_guard<std::mutex> lock(machines_mutex_);
                snapshot = machines_;
                snapshot_version = current_version;
            }

            for (auto& machine : snapshot) {
                machine->react(event);
            }
        }

        auto now = std::chrono::steady_clock::now();
        if (now - last_log >= std::chrono::seconds(1)) {
            std::cout << "[SM-Q] depth=" << queue_.size_approx()
                      << " processed=" << events_processed << std::endl;
            events_processed = 0;
            last_log = now;
        }
    }
}

std::vector<std::shared_ptr<StrategyStateMachine>>
StateMachineDispatcher::get_machines() {
    std::lock_guard<std::mutex> lock(machines_mutex_);
    return machines_;
}

std::shared_ptr<StrategyStateMachine> StateMachineDispatcher::get_machine(
    const std::string& name) const {
    auto it = machines_by_name_.find(name);
    if (it != machines_by_name_.end()) return it->second;
    return nullptr;
}

}  // namespace MarketData
