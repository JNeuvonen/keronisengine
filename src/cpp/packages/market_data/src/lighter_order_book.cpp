#include "market_data/lighter_order_book.h"

#include <algorithm>
#include <charconv>

namespace MarketData {

namespace {

double to_double(std::string_view sv) {
    double value = 0;
    std::from_chars(sv.data(), sv.data() + sv.size(), value);
    return value;
}

bool parse_levels(simdjson::dom::element book, const char *key,
                  std::vector<DepthLevel> &out) {
    simdjson::dom::array levels;
    if (book[key].get(levels) != simdjson::SUCCESS) return false;
    out.reserve(levels.size());
    for (auto level : levels) {
        std::string_view price, size;
        if (level["price"].get(price) != simdjson::SUCCESS ||
            level["size"].get(size) != simdjson::SUCCESS)
            return false;
        out.push_back({to_double(price), to_double(size)});
    }
    return true;
}

}  // namespace

std::optional<LighterBookMessage> LighterBookMessage::parse(
    std::string_view msg, simdjson::dom::parser &parser) {
    simdjson::padded_string padded(msg);
    simdjson::dom::element doc;
    if (parser.parse(padded).get(doc) != simdjson::SUCCESS) return std::nullopt;

    std::string_view type, channel;
    if (doc["type"].get(type) != simdjson::SUCCESS ||
        doc["channel"].get(channel) != simdjson::SUCCESS)
        return std::nullopt;

    LighterBookMessage out;
    if (type == "subscribed/order_book") {
        out.kind = Kind::SNAPSHOT;
    } else if (type == "update/order_book") {
        out.kind = Kind::UPDATE;
    } else {
        return std::nullopt;
    }

    const auto colon = channel.find(':');
    if (colon == std::string_view::npos) return std::nullopt;
    const auto id = channel.substr(colon + 1);
    if (std::from_chars(id.data(), id.data() + id.size(), out.market_id).ec !=
        std::errc{})
        return std::nullopt;

    simdjson::dom::element book;
    int64_t nonce = 0, begin_nonce = 0, timestamp = 0;
    if (doc["order_book"].get(book) != simdjson::SUCCESS ||
        book["nonce"].get(nonce) != simdjson::SUCCESS)
        return std::nullopt;
    if (book["begin_nonce"].get(begin_nonce) != simdjson::SUCCESS)
        begin_nonce = 0;
    if (doc["timestamp"].get(timestamp) != simdjson::SUCCESS) timestamp = 0;
    out.nonce = nonce;
    out.begin_nonce = begin_nonce;
    out.exchange_ms = timestamp;

    if (!parse_levels(book, "bids", out.bids) ||
        !parse_levels(book, "asks", out.asks))
        return std::nullopt;
    return out;
}

void LighterOrderBook::set_levels(
    const std::vector<DepthLevel> &levels,
    std::map<double, double, std::greater<>> &side) {
    for (const auto &level : levels) {
        if (level.quantity <= 0)
            side.erase(level.price);
        else
            side[level.price] = level.quantity;
    }
}

void LighterOrderBook::set_levels(const std::vector<DepthLevel> &levels,
                                  std::map<double, double> &side) {
    for (const auto &level : levels) {
        if (level.quantity <= 0)
            side.erase(level.price);
        else
            side[level.price] = level.quantity;
    }
}

bool LighterOrderBook::is_crossed() const {
    return !bids_.empty() && !asks_.empty() &&
           bids_.begin()->first >= asks_.begin()->first;
}

void LighterOrderBook::reset() {
    bids_.clear();
    asks_.clear();
    nonce_ = 0;
    ready_ = false;
}

LighterOrderBook::ApplyResult LighterOrderBook::apply(
    const LighterBookMessage &msg) {
    if (msg.kind == LighterBookMessage::Kind::SNAPSHOT) {
        reset();
        set_levels(msg.bids, bids_);
        set_levels(msg.asks, asks_);
        nonce_ = msg.nonce;
        ready_ = true;
        return is_crossed() ? ApplyResult::CROSSED
                            : ApplyResult::SNAPSHOT_APPLIED;
    }
    if (!ready_) return ApplyResult::NOT_READY;
    if (msg.begin_nonce != nonce_) {
        reset();
        return ApplyResult::GAP;
    }
    set_levels(msg.bids, bids_);
    set_levels(msg.asks, asks_);
    nonce_ = msg.nonce;
    return is_crossed() ? ApplyResult::CROSSED : ApplyResult::APPLIED;
}

DepthSnapshot LighterOrderBook::top(size_t levels, long long received_ms,
                                    long long exchange_ms) const {
    DepthSnapshot snapshot;
    snapshot.received_ms = received_ms;
    snapshot.exchange_ms = exchange_ms;
    snapshot.sequence = nonce_;
    snapshot.is_valid = ready_ && !is_crossed();
    snapshot.bids.reserve(std::min(levels, bids_.size()));
    snapshot.asks.reserve(std::min(levels, asks_.size()));
    for (auto it = bids_.begin();
         it != bids_.end() && snapshot.bids.size() < levels; ++it)
        snapshot.bids.push_back({it->first, it->second});
    for (auto it = asks_.begin();
         it != asks_.end() && snapshot.asks.size() < levels; ++it)
        snapshot.asks.push_back({it->first, it->second});
    return snapshot;
}

}  // namespace MarketData
