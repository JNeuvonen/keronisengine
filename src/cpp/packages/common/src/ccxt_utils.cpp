#include "ccxt_utils.h"

#include <vector>

namespace CCXTUtils {
std::string convert_ccxt_symbol(const std::string& ccxt_symbol_format) {
    if (ccxt_symbol_format.find("/") != std::string::npos) {
        std::string symbol = ccxt_symbol_format;
        size_t colon_pos = symbol.find(":");
        if (colon_pos != std::string::npos) {
            symbol = symbol.substr(0, colon_pos);
        }
        size_t slash_pos = symbol.find("/");
        if (slash_pos != std::string::npos) {
            symbol = symbol.substr(0, slash_pos) + symbol.substr(slash_pos + 1);
        }
        return symbol;
    }
    return ccxt_symbol_format;
}

std::string convert_gateio_symbol(const std::string& gateio_symbol) {
    std::string symbol = gateio_symbol;
    size_t underscore_pos = symbol.find('_');
    if (underscore_pos != std::string::npos) {
        symbol.erase(underscore_pos, 1);
    }
    return symbol;
}

std::string symbol_to_ccxt_format(const std::string& symbol,
                                  const std::string& market_type) {
    // USD listed AFTER the longer USDT/USDC suffixes so a crypto symbol
    // ending in USDT (e.g. BTCUSDT) doesn't get mis-parsed as ending in
    // USD (BTCUSDT -> USDT match wins). USD covers IBKR-shaped symbols
    // like AAPLUSD that don't end in any other supported quote.
    static const std::vector<std::string> quote_assets = {
        "USDT", "USDC", "BTC", "ETH", "BNB", "SOL", "USD"};
    const auto& [base_asset, quote_asset] =
        CCXTUtils::infer_base_quote_assets(symbol);

    for (const auto& quote : quote_assets) {
        if (symbol.size() > quote.size() &&
            symbol.compare(symbol.size() - quote.size(), quote.size(), quote) ==
                0) {
            std::string base = symbol.substr(0, symbol.size() - quote.size());
            std::string result = base + "/" + quote;
            if (market_type == "swap") {
                result += ":" + quote_asset;
            }
            return result;
        }
    }

    std::string result = symbol;
    if (market_type == "swap") {
        result += ":" + quote_asset;
    }
    return result;
}

bool is_supported_quote_for_venue(const std::string& exchange_name,
                                  const std::string& quote_asset) {
    // IBKR is USD-quoted for US equities. Crypto venues are
    // stablecoin-quoted (USDT for existing strategies; USDC and others
    // can be added when a strategy needs them). Defaulting unknown
    // venues to USDT preserves prior behavior.
    if (exchange_name == "ibkr") {
        return quote_asset == "USD";
    }
    return quote_asset == "USDT";
}

std::pair<std::string, std::string> infer_base_quote_assets(
    const std::string& symbol, const std::string& default_quote_asset) {
    // See symbol_to_ccxt_format for rationale on USD ordering.
    static const std::vector<std::string> quote_assets = {
        "USDT", "USDC", "BTC", "ETH", "BNB", "SOL", "USD"};

    for (const auto& quote : quote_assets) {
        if (symbol.size() > quote.size() &&
            symbol.compare(symbol.size() - quote.size(), quote.size(), quote) ==
                0) {
            std::string base = symbol.substr(0, symbol.size() - quote.size());
            return {base, quote};
        }
    }

    return {symbol, default_quote_asset};
}

std::string hyperliquid_coin_to_symbol(const std::string& coin) {
    if (coin.empty()) return coin;
    return coin + "USDC";
}

std::string hyperliquid_symbol_to_coin(const std::string& symbol) {
    static const std::string kQuote = "USDC";
    if (symbol.size() > kQuote.size() &&
        symbol.compare(symbol.size() - kQuote.size(), kQuote.size(), kQuote) ==
            0) {
        return symbol.substr(0, symbol.size() - kQuote.size());
    }
    return symbol;
}

}  // namespace CCXTUtils