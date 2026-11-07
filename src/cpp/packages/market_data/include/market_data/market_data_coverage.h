#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace MarketData {

// Which symbols a stream covers. Either every listed symbol ("*", narrowed by
// the filters) or an explicit list. Filters apply to "*" only.
struct CoverageSymbolSelector {
    bool all_listed = false;
    std::vector<std::string> symbols;
    std::vector<std::string> contains_any;
    std::vector<std::string> exclude;
    std::vector<std::string> exclude_suffixes;
    // Intersect an explicit list with the venue's listed symbols.
    bool only_listed = false;
};

// Symbols in `solo` get a connection each; the rest are packed `group_size`
// per connection, or all onto one when group_size is 0.
struct CoverageConnectionLayout {
    std::vector<std::string> solo;
    int group_size = 0;
};

struct CoverageExchangeBars {
    std::string interval;
    long long interval_ms = 0;
    bool stream = true;
    std::optional<CoverageSymbolSelector> symbols;
};

struct CoverageTradeBars {
    long long interval_ms = 0;
    CoverageSymbolSelector symbols;
};

struct CoverageBookStream {
    CoverageSymbolSelector symbols;
    CoverageConnectionLayout connections;
};

struct CoverageMarket {
    std::string exchange_name;
    std::string market_type;
    std::optional<CoverageSymbolSelector> symbols;
    std::vector<CoverageExchangeBars> exchange_bars;
    std::optional<CoverageSymbolSelector> trades;
    std::vector<CoverageTradeBars> trade_bars;
    std::optional<CoverageBookStream> book_ticker;
    std::optional<CoverageBookStream> l2;
    bool mini_ticker = false;
    std::vector<std::string> record;
};

struct CoverageConfig {
    std::vector<CoverageMarket> markets;
    bool polymarket = false;
    bool record_polymarket = false;
};

struct ResolvedOhlcvDataset {
    std::string exchange_name;
    std::string market_type;
    std::string symbol;
    long long interval_ms = 0;
    bool stream = false;
    bool operator==(const ResolvedOhlcvDataset&) const = default;
    auto operator<=>(const ResolvedOhlcvDataset&) const = default;
};

struct ResolvedTradeBarDataset {
    std::string exchange_name;
    std::string market_type;
    std::string symbol;
    long long interval_ms = 0;
    bool operator==(const ResolvedTradeBarDataset&) const = default;
    auto operator<=>(const ResolvedTradeBarDataset&) const = default;
};

// One websocket connection. `interval` is set for exchange-bar streams.
struct ResolvedConnection {
    std::string exchange_name;
    std::string market_type;
    std::optional<std::string> interval;
    std::vector<std::string> symbols;
    bool operator==(const ResolvedConnection&) const = default;
    auto operator<=>(const ResolvedConnection&) const = default;
};

struct ResolvedMarketSymbol {
    std::string exchange_name;
    std::string market_type;
    std::string symbol;
    bool operator==(const ResolvedMarketSymbol&) const = default;
    auto operator<=>(const ResolvedMarketSymbol&) const = default;
};

// DataRecorder key; no symbol means every symbol of the market. Per-symbol
// keys stop a removed symbol from being recorded while its connection, shared
// with symbols still covered, stays open.
struct ResolvedRecordingKey {
    std::string stream_type;
    std::string exchange_name;
    std::string market_type;
    std::optional<std::string> symbol;
    bool operator==(const ResolvedRecordingKey&) const = default;
    auto operator<=>(const ResolvedRecordingKey&) const = default;
};

// Everything a process subscribes to, in a deterministic order.
struct ResolvedCoverage {
    std::vector<ResolvedOhlcvDataset> ohlcv_datasets;
    std::vector<ResolvedTradeBarDataset> trade_bar_datasets;
    std::vector<ResolvedConnection> exchange_bar_connections;
    std::vector<ResolvedConnection> trade_connections;
    std::vector<ResolvedConnection> book_ticker_connections;
    std::vector<ResolvedConnection> lob_connections;
    std::vector<ResolvedConnection> mini_ticker_connections;
    // Engine books for every book-ticker and L2 symbol.
    std::vector<ResolvedMarketSymbol> orderbooks;
    // REST snapshots seed every L2 book.
    std::vector<ResolvedMarketSymbol> rest_orderbook_snapshots;
    std::vector<ResolvedRecordingKey> recording_keys;
    bool polymarket = false;
    bool polygon_second_bars = false;
    bool operator==(const ResolvedCoverage&) const = default;
};

// Difference between two resolutions, as a running process applies it.
// Connections are opened or retired whole, never edited in place; one is
// retired only when none of its symbols is still covered, so a reload never
// gaps or duplicates a symbol that stays.
struct CoverageChange {
    std::vector<ResolvedOhlcvDataset> added_ohlcv_datasets;
    std::vector<ResolvedOhlcvDataset> removed_ohlcv_datasets;
    std::vector<ResolvedTradeBarDataset> added_trade_bar_datasets;
    std::vector<ResolvedTradeBarDataset> removed_trade_bar_datasets;
    std::vector<ResolvedConnection> opened_exchange_bar_connections;
    std::vector<ResolvedConnection> retired_exchange_bar_connections;
    std::vector<ResolvedConnection> opened_trade_connections;
    std::vector<ResolvedConnection> retired_trade_connections;
    std::vector<ResolvedConnection> opened_book_ticker_connections;
    std::vector<ResolvedConnection> retired_book_ticker_connections;
    std::vector<ResolvedConnection> opened_lob_connections;
    std::vector<ResolvedConnection> retired_lob_connections;
    std::vector<ResolvedConnection> opened_mini_ticker_connections;
    std::vector<ResolvedConnection> retired_mini_ticker_connections;
    std::vector<ResolvedMarketSymbol> added_orderbooks;
    std::vector<ResolvedMarketSymbol> added_rest_orderbook_snapshots;
    bool recording_keys_changed = false;
    bool polymarket_changed = false;
    [[nodiscard]] bool empty() const;
};

class MarketDataCoverage {
   public:
    using ListedSymbols = std::function<std::vector<std::string>(
        const std::string& exchange_name, const std::string& market_type)>;

    // Throws std::invalid_argument naming the offending field. Unknown keys
    // are errors: a misspelled field would otherwise silently drop coverage.
    [[nodiscard]] static CoverageConfig load(const std::string& path);
    [[nodiscard]] static CoverageConfig parse(const std::string& json_text);

    [[nodiscard]] static ResolvedCoverage resolve(
        const CoverageConfig& config, const ListedSymbols& listed_symbols);

    // Keeps every current connection that still carries a covered symbol and
    // packs only the uncovered remainder onto new connections.
    [[nodiscard]] static ResolvedCoverage resolve_against(
        const CoverageConfig& config, const ListedSymbols& listed_symbols,
        const ResolvedCoverage& current);

    [[nodiscard]] static CoverageChange diff(const ResolvedCoverage& current,
                                             const ResolvedCoverage& next);

    [[nodiscard]] static long long parse_interval_ms(const std::string& text);
};

}  // namespace MarketData
