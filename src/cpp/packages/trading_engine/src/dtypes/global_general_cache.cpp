#include "market_making/dtypes/global_general_cache.h"

namespace Core {

long long get_program_start_time_ms() {
    auto program_start_time_ms_it =
        Core::GlobalGeneralCache::get<long long>("program_start_time_ms");
    if (program_start_time_ms_it.has_value()) {
        return program_start_time_ms_it.value();
    }
    return 0;
}

void set_program_start_time_ms(long long program_start_time_ms) {
    Core::GlobalGeneralCache::insert<long long>("program_start_time_ms",
                                                program_start_time_ms);
}

}  // namespace Core