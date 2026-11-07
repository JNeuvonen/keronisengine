#pragma once
#include <cstddef>
#include <string>

namespace MarketData {

// Read-only snapshot of one OhlcvDataset, cheap enough to rebuild for every
// dataset on a UI poll. Holds no pointer back to the dataset so a consumer
// can filter and sort it without ever taking dataset_mutex.
struct DatasetSummary {
    // Canonical key: the derived-bars base directory, which is also the key
    // DatasetsManager stores the dataset under.
    std::string identifier;
    // Search/display form of the same identity, colon-delimited:
    //   polygon:stock:AAPL:exchange:60000_ms
    std::string display_key;

    std::string exchange_name;
    std::string market_type;
    std::string symbol;
    std::string bar_kind;

    long long interval_ms = 0;
    std::size_t kline_count = 0;
    long long first_bar_open_time_ms = 0;
    long long last_bar_open_time_ms = 0;
    long long last_bar_close_time_ms = 0;
    bool init_fetch_done = false;
};

}  // namespace MarketData
