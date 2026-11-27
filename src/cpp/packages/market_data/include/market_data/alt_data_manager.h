#pragma once
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "market_data/alt_data_module.h"
#include "market_data/alt_data_registry.h"
#include "market_data/alt_data_types.h"
#include "market_data/binance_swap_series_modules.h"
#include "market_data/binance_swap_trading_status_module.h"
#include "market_data/custom_series_module.h"
#include "market_data/symbol_cik_resolver.h"
#include "market_data/us_equity_sec_facts_module.h"
#include "market_data/us_equity_stock_splits_module.h"

namespace MarketData {

// Process-wide registry of alt-data modules: corporate actions, filings-derived
// facts, short interest. Sibling of DatasetsManager rather than part of it --
// that one is a mutable, streamed, written-back store of OHLCV bars, and these
// are read-only histories with different keys and different failure semantics.
//
// Adding a source is one module class and one line in the constructor. The
// generic operations -- provisioning, refresh, coverage -- dispatch through
// AltDataModule; data is reached through the typed accessor for that source,
// because a split and a filing are not the same shape and pretending otherwise
// only moves the switch to every call site.
class AltDataManager {
   public:
    static AltDataManager& instance();

    UsEquityStockSplitsModule& us_equity_stock_splits();
    const UsEquityStockSplitsModule& us_equity_stock_splits() const;
    UsEquitySecFactsModule& us_equity_sec_facts();
    const UsEquitySecFactsModule& us_equity_sec_facts() const;
    BinanceSwapFundingModule& binance_swap_funding();
    const BinanceSwapFundingModule& binance_swap_funding() const;
    BinanceSwapTradingStatusModule& binance_swap_trading_status();
    const BinanceSwapTradingStatusModule& binance_swap_trading_status() const;
    BinanceSwapAccountLongShortRatioModule& binance_swap_account_long_short_ratio();
    const BinanceSwapAccountLongShortRatioModule&
    binance_swap_account_long_short_ratio() const;

    // Ticker -> CIK, point in time. Owned here because both the facts module
    // and the filings store resolve through it, and two copies would drift.
    SymbolCikResolver& symbol_cik();
    const SymbolCikResolver& symbol_cik() const;

    // Provision facts for `symbols` by resolving each to its CIK as of
    // `as_of_ms`. Returns the symbols that could not be resolved -- unmapped or
    // ambiguous -- which concentrate in delisted names, so the size of that set
    // is a bias measurement rather than a nuisance.
    std::vector<std::string> preload_us_equity_sec_facts_for_symbols(
        const std::vector<std::string>& symbols, long long as_of_ms);

    // Generic operations, for callers that treat sources uniformly.
    std::size_t preload(AltDataSource source,
                        const std::vector<std::string>& keys);
    std::size_t refresh_changed(AltDataSource source);
    std::size_t refresh_all_changed();
    std::size_t refresh_keys(AltDataSource source,
                             const std::vector<std::string>& keys);
    AltDataModule* module(AltDataSource source);
    const AltDataModule* module(AltDataSource source) const;

    // Tier 1: zero-C++ generic point-in-time series under <DATA_DIR_ROOT>/alt/custom/<dataset_name>/
    CustomSeriesModule& custom_series(const std::string& dataset_name,
                                      const std::string& root_override = "");

    // Tier 2: typed out-of-tree AltDataModule registry (KERONIS_REGISTER_ALT_DATA or programmatic)
    void register_custom(const std::string& name,
                         std::unique_ptr<AltDataModule> mod);
    AltDataModule* custom_module(const std::string& name);

    template <typename T>
    T& custom(const std::string& name) {
        auto* mod = custom_module(name);
        if (mod == nullptr) {
            throw std::runtime_error("AltDataManager: unknown custom module '" +
                                     name + "'");
        }
        auto* typed = dynamic_cast<T*>(mod);
        if (typed == nullptr) {
            throw std::runtime_error(
                "AltDataManager: custom module '" + name + "' has unexpected type");
        }
        return *typed;
    }

    void clear();

   private:
    AltDataManager();
    AltDataManager(const AltDataManager&) = delete;
    AltDataManager& operator=(const AltDataManager&) = delete;

    std::unordered_map<AltDataSource, std::unique_ptr<AltDataModule>>
        module_by_source_;
    mutable std::mutex custom_mutex_;
    std::unordered_map<std::string, std::unique_ptr<AltDataModule>>
        custom_modules_;
    SymbolCikResolver symbol_cik_resolver_;
};

}  // namespace MarketData
