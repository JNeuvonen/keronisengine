#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "market_data/depth_history.h"
#include "simdjson.h"

namespace MarketData {

// One Lighter `order_book/{id}` channel message. Sizes are absolute; a zero
// size deletes the level. Updates chain: begin_nonce == previous nonce.
struct LighterBookMessage {
    enum class Kind { SNAPSHOT, UPDATE };
    Kind kind = Kind::UPDATE;
    long long market_id = -1;
    long long nonce = 0;
    long long begin_nonce = 0;
    long long exchange_ms = 0;
    std::vector<DepthLevel> bids;
    std::vector<DepthLevel> asks;

    // nullopt for anything that is not an order_book snapshot or update.
    static std::optional<LighterBookMessage> parse(
        std::string_view msg, simdjson::dom::parser &parser);
};

class LighterOrderBook {
   public:
    enum class ApplyResult {
        SNAPSHOT_APPLIED,
        APPLIED,
        NOT_READY,  // update before any snapshot; dropped
        GAP,        // begin_nonce does not chain; book reset, needs snapshot
        CROSSED,    // applied, but best bid >= best ask
    };

    ApplyResult apply(const LighterBookMessage &msg);
    void reset();

    [[nodiscard]] bool is_ready() const { return ready_; }
    [[nodiscard]] long long nonce() const { return nonce_; }
    [[nodiscard]] size_t bid_count() const { return bids_.size(); }
    [[nodiscard]] size_t ask_count() const { return asks_.size(); }

    [[nodiscard]] DepthSnapshot top(size_t levels, long long received_ms,
                                    long long exchange_ms) const;

   private:
    static void set_levels(const std::vector<DepthLevel> &levels,
                           std::map<double, double, std::greater<>> &side);
    static void set_levels(const std::vector<DepthLevel> &levels,
                           std::map<double, double> &side);
    [[nodiscard]] bool is_crossed() const;

    std::map<double, double, std::greater<>> bids_;
    std::map<double, double> asks_;
    long long nonce_ = 0;
    bool ready_ = false;
};

}  // namespace MarketData
