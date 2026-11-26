#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "market_making/dtypes/def_only_structs.h"

namespace LighterDirect {

// One Lighter perp market. The engine names it `venue_symbol` + "USDC"
// (BTC -> BTCUSDC); Lighter addresses it by `market_id`.
struct LighterMarket {
    std::string symbol;
    std::string venue_symbol;
    long long market_id = -1;
    int size_decimals = 0;
    int price_decimals = 0;
    double min_base_amount = 0;
    double min_quote_amount = 0;
    bool is_active = false;
};

// Public market metadata from Lighter's REST `orderBooks`. Market ids are
// always resolved from the venue, never assumed from a ticker.
class LighterMarkets {
   public:
    static LighterMarkets& instance();

    // Perp markets in an `orderBooks` response; spot markets are skipped.
    static std::vector<LighterMarket> parse_order_books(const std::string& body);
    static Core::SymbolExchangeInfo to_symbol_info(const LighterMarket& market);

    static std::string rest_base_url();
    static std::string public_ws_url();

    // Fetches the listing; keeps the previous one when the fetch fails.
    bool refresh();
    // Fetches only while nothing is loaded yet.
    bool ensure_loaded();
    void set_markets(std::vector<LighterMarket> markets);

    std::optional<LighterMarket> by_symbol(const std::string& symbol);
    std::optional<LighterMarket> by_market_id(long long market_id);
    std::vector<LighterMarket> all();

   private:
    LighterMarkets() = default;
    std::mutex mutex_;
    std::vector<LighterMarket> markets_;
};

}  // namespace LighterDirect
