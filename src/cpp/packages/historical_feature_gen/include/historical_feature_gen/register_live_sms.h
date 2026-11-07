#pragma once

namespace HistoricalFeatureGen {

/// Register all live state machines with the StateMachineDispatcher.
/// Called at startup by both live_infra and market_data_recorder.
/// Add new SMs here — one place, both binaries pick them up.
void register_live_state_machines();

}  // namespace HistoricalFeatureGen
