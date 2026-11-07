#pragma once

#include <vector>

#include "market_making/dtypes/simulation_input.h"

namespace BacktestRunner {

class OhlcvDatasetResolver {
   public:
    static void resolve(Core::SimulationInput& simulation_input);

   private:
    static std::vector<std::string> resolve_symbols(
        const Core::SimulationInput& simulation_input,
        const Core::OhlcvDatasetSelection& selection);
};

}  // namespace BacktestRunner
