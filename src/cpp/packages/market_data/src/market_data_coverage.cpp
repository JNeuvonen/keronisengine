#include "market_data/market_data_coverage.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

#include "nlohmann/json.hpp"

namespace MarketData {

namespace {

using json = nlohmann::json;

constexpr const char* RECORD_EXCHANGE_BARS = "exchange_bars";
constexpr const char* RECORD_TRADES = "trades";
constexpr const char* RECORD_BOOK_TICKER = "book_ticker";
constexpr const char* RECORD_L2 = "l2";

constexpr const char* MINI_TICKER_STREAM = "!miniTicker@arr";
constexpr const char* POLYGON_MINUTE_CHANNEL = "AM.*";
constexpr const char* POLYGON_SECOND_CHANNEL = "A.*";

[[noreturn]] void fail(const std::string& where, const std::string& what) {
    throw std::invalid_argument("market data coverage: " + where + ": " +
                                what);
}

void require_known_keys(const json& object, const std::set<std::string>& known,
                        const std::string& where) {
    if (!object.is_object()) fail(where, "expected an object");
    for (const auto& [key, _] : object.items()) {
        if (!known.contains(key)) fail(where, "unknown field \"" + key + "\"");
    }
}

std::vector<std::string> string_list(const json& value,
                                     const std::string& where) {
    if (!value.is_array()) fail(where, "expected a list of strings");
    std::vector<std::string> out;
    for (const auto& item : value) {
        if (!item.is_string()) fail(where, "expected a list of strings");
        out.push_back(item.get<std::string>());
    }
    return out;
}

CoverageSymbolSelector parse_selector(const json& value,
                                      const std::string& where) {
    CoverageSymbolSelector selector;
    const json* symbols = &value;
    if (value.is_object()) {
        require_known_keys(value,
                           {"symbols", "contains_any", "exclude",
                            "exclude_suffixes", "only_listed"},
                           where);
        if (!value.contains("symbols")) fail(where, "missing \"symbols\"");
        symbols = &value.at("symbols");
        if (value.contains("contains_any"))
            selector.contains_any =
                string_list(value.at("contains_any"), where + ".contains_any");
        if (value.contains("exclude"))
            selector.exclude = string_list(value.at("exclude"), where + ".exclude");
        if (value.contains("exclude_suffixes"))
            selector.exclude_suffixes = string_list(
                value.at("exclude_suffixes"), where + ".exclude_suffixes");
        if (value.contains("only_listed"))
            selector.only_listed = value.at("only_listed").get<bool>();
    }
    if (symbols->is_string()) {
        if (symbols->get<std::string>() != "*")
            fail(where, "symbols must be \"*\" or a list");
        selector.all_listed = true;
    } else {
        selector.symbols = string_list(*symbols, where);
        if (selector.symbols.empty()) fail(where, "empty symbol list");
    }
    const bool has_filters = !selector.contains_any.empty() ||
                             !selector.exclude.empty() ||
                             !selector.exclude_suffixes.empty();
    if (!selector.all_listed && has_filters)
        fail(where, "filters apply to \"*\" only");
    if (selector.all_listed && selector.only_listed)
        fail(where, "only_listed applies to an explicit list only");
    return selector;
}

CoverageBookStream parse_book_stream(const json& value,
                                     const std::string& where) {
    require_known_keys(value, {"symbols", "solo_connections", "group_size"},
                       where);
    CoverageBookStream stream;
    if (!value.contains("symbols")) fail(where, "missing \"symbols\"");
    stream.symbols = parse_selector(value.at("symbols"), where + ".symbols");
    if (value.contains("solo_connections"))
        stream.connections.solo = string_list(value.at("solo_connections"),
                                              where + ".solo_connections");
    if (value.contains("group_size")) {
        stream.connections.group_size = value.at("group_size").get<int>();
        if (stream.connections.group_size < 1)
            fail(where, "group_size must be positive");
    }
    return stream;
}

CoverageMarket parse_market(const json& value, const std::string& where) {
    require_known_keys(value,
                       {"exchange", "market_type", "symbols", "exchange_bars",
                        "trades", "trade_bars", "book_ticker", "l2",
                        "mini_ticker", "record"},
                       where);
    CoverageMarket market;
    if (!value.contains("exchange") || !value.contains("market_type"))
        fail(where, "missing \"exchange\" or \"market_type\"");
    market.exchange_name = value.at("exchange").get<std::string>();
    market.market_type = value.at("market_type").get<std::string>();
    const std::string name =
        where + "(" + market.exchange_name + ":" + market.market_type + ")";

    if (value.contains("symbols"))
        market.symbols = parse_selector(value.at("symbols"), name + ".symbols");
    if (value.contains("exchange_bars")) {
        for (const auto& bars : value.at("exchange_bars")) {
            const std::string bars_where = name + ".exchange_bars";
            require_known_keys(bars, {"interval", "stream", "symbols"},
                               bars_where);
            CoverageExchangeBars entry;
            entry.interval = bars.at("interval").get<std::string>();
            entry.interval_ms =
                MarketDataCoverage::parse_interval_ms(entry.interval);
            if (bars.contains("stream"))
                entry.stream = bars.at("stream").get<bool>();
            if (bars.contains("symbols"))
                entry.symbols =
                    parse_selector(bars.at("symbols"), bars_where + ".symbols");
            market.exchange_bars.push_back(std::move(entry));
        }
    }
    if (value.contains("trades")) {
        const json& trades = value.at("trades");
        require_known_keys(trades, {"symbols"}, name + ".trades");
        market.trades =
            parse_selector(trades.at("symbols"), name + ".trades.symbols");
    }
    if (value.contains("trade_bars")) {
        for (const auto& bars : value.at("trade_bars")) {
            const std::string bars_where = name + ".trade_bars";
            require_known_keys(bars, {"interval", "symbols"}, bars_where);
            CoverageTradeBars entry;
            entry.interval_ms = MarketDataCoverage::parse_interval_ms(
                bars.at("interval").get<std::string>());
            entry.symbols =
                parse_selector(bars.at("symbols"), bars_where + ".symbols");
            market.trade_bars.push_back(std::move(entry));
        }
    }
    if (value.contains("book_ticker"))
        market.book_ticker =
            parse_book_stream(value.at("book_ticker"), name + ".book_ticker");
    if (value.contains("l2"))
        market.l2 = parse_book_stream(value.at("l2"), name + ".l2");
    if (value.contains("mini_ticker"))
        market.mini_ticker = value.at("mini_ticker").get<bool>();
    if (value.contains("record"))
        market.record = string_list(value.at("record"), name + ".record");
    return market;
}

// Symbols per websocket connection for venues whose subscriptions are
// batched; 0 means the venue takes every symbol on one connection.
int max_symbols_per_connection(const std::string& exchange_name) {
    // Binance documents 200 streams per combined connection; going past 150
    // made the kline subscription go chronically stale.
    if (exchange_name == "binance") return 150;
    if (exchange_name == "bybit") return 100;
    if (exchange_name == "bitget") return 50;
    if (exchange_name == "gateio") return 150;
    return 0;
}

bool supports_exchange_bar_stream(const std::string& exchange_name) {
    return exchange_name == "binance" || exchange_name == "bybit" ||
           exchange_name == "bitget" || exchange_name == "gateio" ||
           exchange_name == "polygon";
}

bool contains(const std::vector<std::string>& values, const std::string& v) {
    return std::find(values.begin(), values.end(), v) != values.end();
}

bool ends_with(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(),
                         suffix) == 0;
}

class Resolver {
   public:
    explicit Resolver(const MarketDataCoverage::ListedSymbols& listed_symbols)
        : listed_symbols_(listed_symbols) {}

    std::vector<std::string> select(const CoverageMarket& market,
                                    const CoverageSymbolSelector& selector) {
        std::vector<std::string> out;
        if (!selector.all_listed) {
            if (!selector.only_listed) return selector.symbols;
            const auto& listed = listed_for(market);
            const std::set<std::string> listed_set(listed.begin(), listed.end());
            for (const auto& symbol : selector.symbols) {
                if (listed_set.contains(symbol)) out.push_back(symbol);
            }
            return out;
        }
        for (const auto& symbol : listed_for(market)) {
            if (!selector.contains_any.empty()) {
                bool matched = false;
                for (const auto& needle : selector.contains_any) {
                    if (symbol.find(needle) != std::string::npos) matched = true;
                }
                if (!matched) continue;
            }
            if (contains(selector.exclude, symbol)) continue;
            bool excluded_suffix = false;
            for (const auto& suffix : selector.exclude_suffixes) {
                if (ends_with(symbol, suffix)) excluded_suffix = true;
            }
            if (excluded_suffix) continue;
            out.push_back(symbol);
        }
        return out;
    }

   private:
    const std::vector<std::string>& listed_for(const CoverageMarket& market) {
        const std::string key = market.exchange_name + ":" + market.market_type;
        auto it = cache_.find(key);
        if (it == cache_.end()) {
            auto symbols =
                listed_symbols_(market.exchange_name, market.market_type);
            std::sort(symbols.begin(), symbols.end());
            symbols.erase(std::unique(symbols.begin(), symbols.end()),
                          symbols.end());
            it = cache_.emplace(key, std::move(symbols)).first;
        }
        return it->second;
    }

    const MarketDataCoverage::ListedSymbols& listed_symbols_;
    std::map<std::string, std::vector<std::string>> cache_;
};

const CoverageSymbolSelector& stream_selector(
    const CoverageMarket& market,
    const std::optional<CoverageSymbolSelector>& selector,
    const std::string& where) {
    if (selector.has_value()) return *selector;
    if (!market.symbols.has_value())
        fail(market.exchange_name + ":" + market.market_type + "." + where,
             "no \"symbols\" here or on the market");
    return *market.symbols;
}

// Packs `symbols` onto connections, keeping each of `kept` intact.
std::vector<ResolvedConnection> pack(
    const std::string& exchange_name, const std::string& market_type,
    const std::optional<std::string>& interval,
    const std::vector<std::string>& symbols,
    const CoverageConnectionLayout& layout,
    const std::vector<ResolvedConnection>& current) {
    std::vector<ResolvedConnection> out;
    std::set<std::string> placed;
    const std::set<std::string> wanted(symbols.begin(), symbols.end());
    for (const auto& connection : current) {
        if (connection.exchange_name != exchange_name ||
            connection.market_type != market_type ||
            connection.interval != interval)
            continue;
        bool carries_wanted = false;
        for (const auto& symbol : connection.symbols) {
            if (wanted.contains(symbol)) carries_wanted = true;
        }
        if (!carries_wanted) continue;
        out.push_back(connection);
        placed.insert(connection.symbols.begin(), connection.symbols.end());
    }

    const auto make = [&](std::vector<std::string> group) {
        return ResolvedConnection{exchange_name, market_type, interval,
                                  std::move(group)};
    };
    std::vector<std::string> grouped;
    for (const auto& symbol : symbols) {
        if (placed.contains(symbol)) continue;
        placed.insert(symbol);
        if (contains(layout.solo, symbol)) {
            out.push_back(make({symbol}));
            continue;
        }
        grouped.push_back(symbol);
        if (layout.group_size > 0 &&
            static_cast<int>(grouped.size()) >= layout.group_size) {
            out.push_back(make(std::move(grouped)));
            grouped.clear();
        }
    }
    if (!grouped.empty()) out.push_back(make(std::move(grouped)));
    return out;
}

template <typename T>
void sort_unique(std::vector<T>& values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

template <typename T>
void set_difference(const std::vector<T>& from, const std::vector<T>& minus,
                    std::vector<T>& out) {
    std::vector<T> a = from;
    std::vector<T> b = minus;
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    std::set_difference(a.begin(), a.end(), b.begin(), b.end(),
                        std::back_inserter(out));
}

ResolvedCoverage resolve_impl(const CoverageConfig& config,
                              const MarketDataCoverage::ListedSymbols& listed,
                              const ResolvedCoverage* current) {
    static const ResolvedCoverage NOTHING;
    const ResolvedCoverage& previous = current ? *current : NOTHING;
    Resolver resolver(listed);
    ResolvedCoverage out;
    std::set<std::string> seen_markets;

    for (const auto& market : config.markets) {
        const std::string& exchange = market.exchange_name;
        const std::string& market_type = market.market_type;
        const std::string name = exchange + ":" + market_type;
        if (!seen_markets.insert(name).second)
            fail(name, "market listed twice");

        const CoverageConnectionLayout batched{
            {}, max_symbols_per_connection(exchange)};

        // Polygon allows one connection per key, so every aggregate channel
        // rides one connection whose "symbols" are its subscription params.
        std::vector<std::string> polygon_channels;
        for (const auto& bars : market.exchange_bars) {
            if (exchange == "polygon" && bars.interval_ms == 1000) {
                // Second bars go to PolygonSecondBarStore, not to datasets.
                if (!bars.stream || bars.symbols.has_value())
                    fail(name, "polygon 1s bars are streamed for every symbol");
                polygon_channels.push_back(POLYGON_SECOND_CHANNEL);
                out.polygon_second_bars = true;
                continue;
            }
            const auto symbols = resolver.select(
                market, stream_selector(market, bars.symbols, "exchange_bars"));
            for (const auto& symbol : symbols) {
                out.ohlcv_datasets.push_back(
                    {exchange, market_type, symbol, bars.interval_ms,
                     bars.stream});
            }
            if (!bars.stream) continue;
            if (!supports_exchange_bar_stream(exchange))
                fail(name, "no exchange-bar websocket for this venue");
            if (exchange == "polygon") {
                // AM.* is a wildcard; the datasets decide which symbols are kept.
                if (bars.interval_ms != 60000)
                    fail(name, "polygon streams 1m and 1s bars only");
                polygon_channels.push_back(POLYGON_MINUTE_CHANNEL);
                continue;
            }
            auto connections =
                pack(exchange, market_type, bars.interval, symbols, batched,
                     previous.exchange_bar_connections);
            out.exchange_bar_connections.insert(
                out.exchange_bar_connections.end(), connections.begin(),
                connections.end());
        }
        if (!polygon_channels.empty()) {
            sort_unique(polygon_channels);
            out.exchange_bar_connections.push_back(
                {exchange, market_type, std::nullopt, polygon_channels});
        }

        std::vector<std::string> trade_symbols;
        if (market.trades.has_value()) {
            trade_symbols = resolver.select(market, *market.trades);
            auto connections = pack(exchange, market_type, std::nullopt,
                                    trade_symbols, batched,
                                    previous.trade_connections);
            out.trade_connections.insert(out.trade_connections.end(),
                                         connections.begin(),
                                         connections.end());
        }
        for (const auto& bars : market.trade_bars) {
            if (!market.trades.has_value())
                fail(name, "trade_bars need a trades stream");
            for (const auto& symbol : resolver.select(market, bars.symbols)) {
                // A trade bar exists only where its trades are streamed.
                if (!contains(trade_symbols, symbol)) continue;
                out.trade_bar_datasets.push_back(
                    {exchange, market_type, symbol, bars.interval_ms});
            }
        }

        std::vector<std::string> book_ticker_symbols;
        if (market.book_ticker.has_value()) {
            book_ticker_symbols =
                resolver.select(market, market.book_ticker->symbols);
            auto connections = pack(exchange, market_type, std::nullopt,
                                    book_ticker_symbols,
                                    market.book_ticker->connections,
                                    previous.book_ticker_connections);
            out.book_ticker_connections.insert(
                out.book_ticker_connections.end(), connections.begin(),
                connections.end());
            for (const auto& symbol : book_ticker_symbols)
                out.orderbooks.push_back({exchange, market_type, symbol});
        }

        std::vector<std::string> l2_symbols;
        if (market.l2.has_value()) {
            l2_symbols = resolver.select(market, market.l2->symbols);
            auto connections =
                pack(exchange, market_type, std::nullopt, l2_symbols,
                     market.l2->connections, previous.lob_connections);
            out.lob_connections.insert(out.lob_connections.end(),
                                       connections.begin(), connections.end());
            // Lighter's websocket opens every order_book subscription with
            // a full snapshot, so there is no REST seed to fetch.
            const bool seeded_by_stream = exchange == "lighter";
            for (const auto& symbol : l2_symbols) {
                out.orderbooks.push_back({exchange, market_type, symbol});
                if (!seeded_by_stream)
                    out.rest_orderbook_snapshots.push_back(
                        {exchange, market_type, symbol});
            }
        }

        if (market.mini_ticker)
            out.mini_ticker_connections.push_back(
                {exchange, market_type, std::nullopt, {MINI_TICKER_STREAM}});

        for (const auto& recorded : market.record) {
            std::vector<std::pair<std::string, std::vector<std::string>>> keys;
            if (recorded == RECORD_EXCHANGE_BARS) {
                std::vector<std::string> symbols;
                for (const auto& dataset : out.ohlcv_datasets) {
                    if (dataset.exchange_name == exchange &&
                        dataset.market_type == market_type && dataset.stream)
                        symbols.push_back(dataset.symbol);
                }
                if (symbols.empty())
                    fail(name, "records exchange_bars but streams none");
                keys.push_back({"klines", symbols});
            } else if (recorded == RECORD_TRADES) {
                if (!market.trades.has_value())
                    fail(name, "records trades but streams none");
                keys.push_back({"trade", trade_symbols});
            } else if (recorded == RECORD_BOOK_TICKER) {
                if (!market.book_ticker.has_value())
                    fail(name, "records book_ticker but streams none");
                keys.push_back({"book_ticker", book_ticker_symbols});
            } else if (recorded == RECORD_L2) {
                if (!market.l2.has_value())
                    fail(name, "records l2 but streams none");
                keys.push_back({"lob_l2_update", l2_symbols});
                keys.push_back({"lob_snapshot", l2_symbols});
            } else {
                fail(name, "unknown record stream \"" + recorded + "\"");
            }
            for (const auto& [stream_type, symbols] : keys) {
                for (const auto& symbol : symbols)
                    out.recording_keys.push_back(
                        {stream_type, exchange, market_type, symbol});
            }
        }
    }

    out.polymarket = config.polymarket;
    if (config.record_polymarket)
        out.recording_keys.push_back(
            {"polymarket", "polymarket", "market", std::nullopt});

    sort_unique(out.ohlcv_datasets);
    sort_unique(out.trade_bar_datasets);
    sort_unique(out.orderbooks);
    sort_unique(out.rest_orderbook_snapshots);
    sort_unique(out.recording_keys);
    return out;
}

}  // namespace

bool CoverageChange::empty() const {
    return added_ohlcv_datasets.empty() && removed_ohlcv_datasets.empty() &&
           added_trade_bar_datasets.empty() &&
           removed_trade_bar_datasets.empty() &&
           opened_exchange_bar_connections.empty() &&
           retired_exchange_bar_connections.empty() &&
           opened_trade_connections.empty() &&
           retired_trade_connections.empty() &&
           opened_book_ticker_connections.empty() &&
           retired_book_ticker_connections.empty() &&
           opened_lob_connections.empty() && retired_lob_connections.empty() &&
           opened_mini_ticker_connections.empty() &&
           retired_mini_ticker_connections.empty() && added_orderbooks.empty() &&
           added_rest_orderbook_snapshots.empty() && !recording_keys_changed &&
           !polymarket_changed;
}

long long MarketDataCoverage::parse_interval_ms(const std::string& text) {
    if (text.size() < 2) fail("interval", "\"" + text + "\" is not an interval");
    const char unit = text.back();
    long long count = 0;
    try {
        size_t consumed = 0;
        count = std::stoll(text.substr(0, text.size() - 1), &consumed);
        if (consumed != text.size() - 1) throw std::invalid_argument(text);
    } catch (const std::exception&) {
        fail("interval", "\"" + text + "\" is not an interval");
    }
    if (count <= 0) fail("interval", "\"" + text + "\" is not positive");
    switch (unit) {
        case 's':
            return count * 1000;
        case 'm':
            return count * 60'000;
        case 'h':
            return count * 3'600'000;
        case 'd':
            return count * 86'400'000;
        default:
            fail("interval", "\"" + text + "\" needs a unit of s, m, h or d");
    }
}

CoverageConfig MarketDataCoverage::parse(const std::string& json_text) {
    json document;
    try {
        document = json::parse(json_text);
    } catch (const json::parse_error& error) {
        fail("file", error.what());
    }
    require_known_keys(document, {"markets", "polymarket"}, "root");
    CoverageConfig config;
    try {
        if (document.contains("markets")) {
            for (size_t i = 0; i < document.at("markets").size(); ++i) {
                config.markets.push_back(parse_market(
                    document.at("markets").at(i),
                    "markets[" + std::to_string(i) + "]"));
            }
        }
        if (document.contains("polymarket")) {
            const json& polymarket = document.at("polymarket");
            require_known_keys(polymarket, {"stream", "record"}, "polymarket");
            config.polymarket = polymarket.value("stream", false);
            config.record_polymarket = polymarket.value("record", false);
            if (config.record_polymarket && !config.polymarket)
                fail("polymarket", "records but does not stream");
        }
    } catch (const json::exception& error) {
        fail("file", error.what());
    }
    return config;
}

CoverageConfig MarketDataCoverage::load(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) fail(path, "cannot open");
    std::stringstream buffer;
    buffer << file.rdbuf();
    return parse(buffer.str());
}

ResolvedCoverage MarketDataCoverage::resolve(
    const CoverageConfig& config, const ListedSymbols& listed_symbols) {
    return resolve_impl(config, listed_symbols, nullptr);
}

ResolvedCoverage MarketDataCoverage::resolve_against(
    const CoverageConfig& config, const ListedSymbols& listed_symbols,
    const ResolvedCoverage& current) {
    return resolve_impl(config, listed_symbols, &current);
}

CoverageChange MarketDataCoverage::diff(const ResolvedCoverage& current,
                                        const ResolvedCoverage& next) {
    CoverageChange change;
    set_difference(next.ohlcv_datasets, current.ohlcv_datasets,
                   change.added_ohlcv_datasets);
    set_difference(current.ohlcv_datasets, next.ohlcv_datasets,
                   change.removed_ohlcv_datasets);
    set_difference(next.trade_bar_datasets, current.trade_bar_datasets,
                   change.added_trade_bar_datasets);
    set_difference(current.trade_bar_datasets, next.trade_bar_datasets,
                   change.removed_trade_bar_datasets);
    set_difference(next.exchange_bar_connections,
                   current.exchange_bar_connections,
                   change.opened_exchange_bar_connections);
    set_difference(current.exchange_bar_connections,
                   next.exchange_bar_connections,
                   change.retired_exchange_bar_connections);
    set_difference(next.trade_connections, current.trade_connections,
                   change.opened_trade_connections);
    set_difference(current.trade_connections, next.trade_connections,
                   change.retired_trade_connections);
    set_difference(next.book_ticker_connections,
                   current.book_ticker_connections,
                   change.opened_book_ticker_connections);
    set_difference(current.book_ticker_connections,
                   next.book_ticker_connections,
                   change.retired_book_ticker_connections);
    set_difference(next.lob_connections, current.lob_connections,
                   change.opened_lob_connections);
    set_difference(current.lob_connections, next.lob_connections,
                   change.retired_lob_connections);
    set_difference(next.mini_ticker_connections,
                   current.mini_ticker_connections,
                   change.opened_mini_ticker_connections);
    set_difference(current.mini_ticker_connections,
                   next.mini_ticker_connections,
                   change.retired_mini_ticker_connections);
    set_difference(next.orderbooks, current.orderbooks,
                   change.added_orderbooks);
    set_difference(next.rest_orderbook_snapshots,
                   current.rest_orderbook_snapshots,
                   change.added_rest_orderbook_snapshots);
    change.recording_keys_changed = next.recording_keys != current.recording_keys;
    change.polymarket_changed = next.polymarket != current.polymarket;
    return change;
}

}  // namespace MarketData
