#include <iostream>

#include "historical_feature_gen/historical_feature_gen.h"

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: historical_feature_gen <config.json>" << std::endl;
        return 1;
    }

    try {
        auto config = HistoricalFeatureGen::parse_config(argv[1]);
        auto sm =
            HistoricalFeatureGen::create_state_machine(config.state_machine);
        sm->configure(config.sm_params);
        sm->prepare_historical();

        // Incremental resume if state was loaded from disk
        if (sm->has_loaded_state()) {
            long long min_ts = sm->get_min_synced_ts();
            if (min_ts > 0 && min_ts > config.start_ts_ms) {
                long long buffer_ms = config.incremental_buffer_ms;
                config.start_ts_ms =
                    std::max(config.start_ts_ms, min_ts - buffer_ms);
                std::cout << "Incremental resume from ts="
                          << config.start_ts_ms << " (watermark=" << min_ts
                          << ", buffer=" << buffer_ms << "ms)" << std::endl;
            }
        }

        int result = HistoricalFeatureGen::run(config, *sm);
        if (result == 0) {
            sm->serialize_state();
        }
        return result;
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
}
