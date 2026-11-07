#pragma once

#include <atomic>
#include <memory>

#include <memory>
#include <optional>
#include <string>
#include <vector>
class WsStream;

namespace MarketData {

struct StreamInfo {
    std::string exchange_name;
    std::string market_type;
    std::string symbol;
    std::optional<std::string> interval;
    std::optional<std::string> api_key;
};

struct StreamsConfig {
    std::vector<StreamInfo> streams;
};

struct StreamMeta {
    std::vector<StreamInfo> stream_infos;
    std::unique_ptr<WsStream> stream;
    std::string stream_type;
    // Set when coverage retires the connection; its handler drops anything a
    // reconnect already in flight still delivers.
    std::shared_ptr<std::atomic<bool>> retired;
};
};  // namespace MarketData