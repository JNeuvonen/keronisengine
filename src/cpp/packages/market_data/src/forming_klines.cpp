#include "market_data/forming_klines.h"

#include <simdjson.h>

#include "ohlcv_dataset_tick.h"

namespace MarketData {

namespace {

std::string_view string_field(std::string_view msg, std::string_view tag) {
    const auto start = msg.find(tag);
    if (start == std::string_view::npos) return {};
    const auto value = start + tag.size();
    const auto end = msg.find('"', value);
    if (end == std::string_view::npos) return {};
    return msg.substr(value, end - value);
}

void build_key(std::string& key, std::string_view exchange,
               std::string_view market_type, std::string_view interval,
               std::string_view symbol) {
    key.clear();
    key.append(exchange).append(1, '|').append(market_type).append(1, '|');
    key.append(interval).append(1, '|').append(symbol);
}

template <typename T>
void put_value(std::string& out, T value) {
    out.append(reinterpret_cast<const char*>(&value), sizeof(value));
}

}  // namespace

FormingKlineStore& FormingKlineStore::get_instance() {
    static FormingKlineStore instance;
    return instance;
}

void FormingKlineStore::put(const std::string& exchange,
                            const std::string& market_type, std::string_view msg,
                            long long received_ms) {
    const auto interval = string_field(msg, "\"i\":\"");
    const auto symbol = string_field(msg, "\"s\":\"");
    if (interval.empty() || symbol.empty()) return;
    // Reused so a steady stream of updates allocates nothing.
    thread_local std::string key;
    build_key(key, exchange, market_type, interval, symbol);

    std::lock_guard lock(mutex_);
    auto it = entries_.find(key);
    if (it == entries_.end())
        it = entries_.emplace(key, Entry{std::string(symbol), {}, 0, 0}).first;
    it->second.msg.assign(msg);
    it->second.received_ms = received_ms;
    it->second.sequence = ++latest_sequence_;
}

FormingKlines FormingKlineStore::read(const std::string& exchange,
                                      const std::string& market_type,
                                      const std::string& interval,
                                      const std::vector<std::string>& symbols,
                                      std::uint64_t since) const {
    std::vector<Entry> matched;
    FormingKlines out;
    {
        std::lock_guard lock(mutex_);
        out.latest_sequence = latest_sequence_;
        const bool restarted = since > latest_sequence_;
        auto take = [&](const Entry& entry) {
            if (restarted || entry.sequence > since) matched.push_back(entry);
        };
        std::string key;
        if (symbols.empty()) {
            build_key(key, exchange, market_type, interval, "");
            for (const auto& [entry_key, entry] : entries_)
                if (entry_key.compare(0, key.size(), key) == 0) take(entry);
        } else {
            for (const auto& symbol : symbols) {
                build_key(key, exchange, market_type, interval, symbol);
                if (auto it = entries_.find(key); it != entries_.end())
                    take(it->second);
            }
        }
    }

    simdjson::dom::parser parser;
    out.klines.reserve(matched.size());
    for (const auto& entry : matched) {
        simdjson::padded_string padded(entry.msg);
        auto doc = parser.parse(padded);
        if (doc.error() != simdjson::SUCCESS) continue;
        auto data = doc["data"];
        try {
            const auto tick = OhlcvDatasetTick::from_simdjson(
                data.error() == simdjson::SUCCESS ? data.value() : doc.value());
            SecondBar bar{tick.kline_open_time, tick.open_price,
                          tick.high_price,      tick.low_price,
                          tick.close_price,     tick.volume,
                          tick.volume > 0 ? tick.quote_asset_volume / tick.volume
                                          : tick.close_price,
                          static_cast<std::int64_t>(tick.number_of_trades)};
            out.klines.push_back(
                {entry.symbol, tick.is_closed, entry.received_ms, bar});
        } catch (const std::exception&) {
            continue;
        }
    }
    return out;
}

std::size_t FormingKlineStore::size() const {
    std::lock_guard lock(mutex_);
    return entries_.size();
}

void FormingKlineStore::clear() {
    std::lock_guard lock(mutex_);
    entries_.clear();
    latest_sequence_ = 0;
}

std::string FormingKlineStore::encode(const FormingKlines& klines) {
    std::string out;
    put_value(out, klines.latest_sequence);
    put_value(out, static_cast<std::uint32_t>(klines.klines.size()));
    for (const auto& kline : klines.klines) {
        put_value(out, static_cast<std::uint16_t>(kline.symbol.size()));
        out += kline.symbol;
        put_value(out, static_cast<std::uint8_t>(kline.closed));
        put_value(out, static_cast<std::int64_t>(kline.received_ms));
        put_value(out, kline.bar);
    }
    return out;
}

}  // namespace MarketData
