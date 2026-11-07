#include "historical_feature_gen/register_live_sms.h"

#include <cstdlib>
#include <memory>
#include <sstream>
#include <string>

#include "common_types/log_event_types.h"
#include "disk_logging/log.h"
#include "market_data/config.h"
#include "market_data/state_machine_dispatcher.h"
#include "market_data/state_machine_registry.h"

namespace HistoricalFeatureGen {

// MD_STATE_MACHINES lists registered machines by name, comma-separated. Each
// one warms up from venue REST history, so a host registers only what its
// strategies watch, and must cover every dependency of what it registers. A
// strategy that watches an unregistered machine gets null and never predicts.
void register_live_state_machines() {
    if (!MarketData::Config::getInstance().get_is_state_machines_enabled()) {
        LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_SM_REGISTERED,
                 "MD_ENABLE_STATE_MACHINES=0 — skipping SM registration",
                 nlohmann::json{});
        return;
    }
    const char* env = std::getenv("MD_STATE_MACHINES");
    if (!env) return;

    std::stringstream ss(env);
    for (std::string name; std::getline(ss, name, ',');) {
        if (name.empty()) continue;
        std::shared_ptr<MarketData::StrategyStateMachine> sm =
            MarketData::StateMachineRegistry::create(name);
        sm->prepare_live();
        MarketData::StateMachineDispatcher::get_instance().register_machine(sm);
        LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_SM_REGISTERED,
                 "Registered state machine",
                 (nlohmann::json{{"sm_name", sm->get_name()}}));
    }
}

}  // namespace HistoricalFeatureGen
