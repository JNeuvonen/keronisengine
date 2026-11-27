#include "market_data/alt_data_types.h"

#include <cstdlib>
#include <stdexcept>

namespace MarketData {

const char* to_string(AltDataSource source) {
    switch (source) {
        case AltDataSource::UsEquityStockSplits:
            return "us_equity_stock_splits";
        case AltDataSource::UsEquitySecFacts:
            return "us_equity_sec_facts";
        case AltDataSource::UsEquityShortInterest:
            return "us_equity_short_interest";
        case AltDataSource::BinanceSwapFunding:
            return "binance_swap_funding";
        case AltDataSource::BinanceSwapAccountLongShortRatio:
            return "binance_swap_account_long_short_ratio";
        case AltDataSource::BinanceSwapTradingStatus:
            return "binance_swap_trading_status";
        case AltDataSource::Custom:
            return "custom";
    }
    return "unknown";
}

std::optional<AltDataSource> alt_data_source_from_string(const std::string& name) {
    for (const auto source :
         {AltDataSource::UsEquityStockSplits, AltDataSource::UsEquitySecFacts,
          AltDataSource::UsEquityShortInterest, AltDataSource::BinanceSwapFunding,
          AltDataSource::BinanceSwapAccountLongShortRatio, AltDataSource::BinanceSwapTradingStatus,
          AltDataSource::Custom}) {
        if (name == to_string(source)) return source;
    }
    return std::nullopt;
}

const std::string& alt_data_root() {
    // Resolved once: path_for runs per key on every preload and every refresh,
    // and neither the env var nor the layout changes while the process lives.
    static const std::string root = [] {
        const char* env = std::getenv("DATA_DIR_ROOT");
        if (env == nullptr || *env == '\0') {
            throw std::invalid_argument("DATA_DIR_ROOT is not set");
        }
        return std::string(env) + "/alt";
    }();
    return root;
}

}  // namespace MarketData
