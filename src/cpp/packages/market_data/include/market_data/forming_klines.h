#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "market_data/polygon_second_bars.h"

namespace MarketData {

struct FormingKline {
    std::string symbol;
    bool closed = false;
    long long received_ms = 0;
    SecondBar bar;
};

struct FormingKlines {
    std::uint64_t latest_sequence = 0;
    std::vector<FormingKline> klines;
};

// The latest Binance kline message per (exchange, market, interval, symbol),
// closed or not. Written by the ohlcv consumer thread, so put() only copies
// the raw message; parsing happens when a reader asks.
class FormingKlineStore {
   public:
    static FormingKlineStore& get_instance();

    void put(const std::string& exchange, const std::string& market_type,
             std::string_view msg, long long received_ms);
    // Klines updated after `since`; every kline when `since` is ahead of the
    // store (the recorder restarted). Empty `symbols` means all of them.
    [[nodiscard]] FormingKlines read(const std::string& exchange,
                                     const std::string& market_type,
                                     const std::string& interval,
                                     const std::vector<std::string>& symbols,
                                     std::uint64_t since) const;
    [[nodiscard]] std::size_t size() const;
    void clear();

    // [u64 latest sequence][u32 klines] then per kline
    // [u16 length][name][u8 closed][i64 received ms][64-byte bar].
    static std::string encode(const FormingKlines& klines);

   private:
    struct Entry {
        std::string symbol;
        std::string msg;
        long long received_ms = 0;
        std::uint64_t sequence = 0;
    };

    mutable std::mutex mutex_;
    std::unordered_map<std::string, Entry> entries_;
    std::uint64_t latest_sequence_ = 0;
};

}  // namespace MarketData
