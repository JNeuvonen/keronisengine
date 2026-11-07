#pragma once

#include <optional>
#include <string>
#include <vector>

namespace MarketData {

// Point-in-time view of one websocket stream for display. Carries no URL path
// or query and no StreamInfo::api_key: private-stream URLs embed listen keys.
struct StreamStatus {
    std::string key;
    bool is_private = false;
    std::string stream_type;
    std::string exchange_name;
    std::string market_type;
    std::optional<long long> account_id;
    std::vector<std::string> symbols;
    std::string host;

    bool is_connected = false;
    // Venue is silent by schedule (US equities outside extended hours).
    bool is_held = false;
    bool is_stale = false;
    long long last_msg_age_ms = 0;
    long long msg_count = 0;
    long long sampled_at_ms = 0;

    int disconnects = 0;
    int reconnects = 0;
    int forced_disconnects = 0;
    std::string disconnect_reason;
    long long disconnected_for_ms = 0;
};

struct StreamStatusUtils {
    // "wss://host:9443/ws/<listen-key>?x=y" -> "wss://host:9443".
    static std::string url_host(const std::string& url) {
        const auto scheme_end = url.find("://");
        const size_t host_start =
            scheme_end == std::string::npos ? 0 : scheme_end + 3;
        const auto host_end = url.find_first_of("/?#", host_start);
        auto authority = url.substr(0, host_end);
        const auto at = authority.find('@', host_start);
        if (at != std::string::npos) {
            authority.erase(host_start, at + 1 - host_start);
        }
        return authority;
    }
};

}  // namespace MarketData
