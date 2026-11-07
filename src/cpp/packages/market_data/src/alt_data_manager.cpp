#include "market_data/alt_data_manager.h"

#include <iostream>

namespace MarketData {

AltDataManager::AltDataManager() {
    module_by_source_[AltDataSource::UsEquityStockSplits] =
        std::make_unique<UsEquityStockSplitsModule>();
    module_by_source_[AltDataSource::UsEquitySecFacts] =
        std::make_unique<UsEquitySecFactsModule>();
    module_by_source_[AltDataSource::BinanceSwapFunding] =
        std::make_unique<BinanceSwapFundingModule>();
    module_by_source_[AltDataSource::BinanceSwapTradingStatus] =
        std::make_unique<BinanceSwapTradingStatusModule>();
    module_by_source_[AltDataSource::BinanceSwapAccountLongShortRatio] =
        std::make_unique<BinanceSwapAccountLongShortRatioModule>();
}

AltDataManager& AltDataManager::instance() {
    static AltDataManager manager;
    return manager;
}

UsEquityStockSplitsModule& AltDataManager::us_equity_stock_splits() {
    return static_cast<UsEquityStockSplitsModule&>(
        *module_by_source_.at(AltDataSource::UsEquityStockSplits));
}

const UsEquityStockSplitsModule& AltDataManager::us_equity_stock_splits()
    const {
    return static_cast<const UsEquityStockSplitsModule&>(
        *module_by_source_.at(AltDataSource::UsEquityStockSplits));
}

UsEquitySecFactsModule& AltDataManager::us_equity_sec_facts() {
    return static_cast<UsEquitySecFactsModule&>(
        *module_by_source_.at(AltDataSource::UsEquitySecFacts));
}

const UsEquitySecFactsModule& AltDataManager::us_equity_sec_facts() const {
    return static_cast<const UsEquitySecFactsModule&>(
        *module_by_source_.at(AltDataSource::UsEquitySecFacts));
}

BinanceSwapFundingModule& AltDataManager::binance_swap_funding() {
    return static_cast<BinanceSwapFundingModule&>(
        *module_by_source_.at(AltDataSource::BinanceSwapFunding));
}

BinanceSwapTradingStatusModule& AltDataManager::binance_swap_trading_status() {
    return static_cast<BinanceSwapTradingStatusModule&>(
        *module_by_source_.at(AltDataSource::BinanceSwapTradingStatus));
}

const BinanceSwapTradingStatusModule& AltDataManager::binance_swap_trading_status() const {
    return static_cast<const BinanceSwapTradingStatusModule&>(
        *module_by_source_.at(AltDataSource::BinanceSwapTradingStatus));
}

const BinanceSwapFundingModule& AltDataManager::binance_swap_funding() const {
    return static_cast<const BinanceSwapFundingModule&>(
        *module_by_source_.at(AltDataSource::BinanceSwapFunding));
}

BinanceSwapAccountLongShortRatioModule&
AltDataManager::binance_swap_account_long_short_ratio() {
    return static_cast<BinanceSwapAccountLongShortRatioModule&>(
        *module_by_source_.at(AltDataSource::BinanceSwapAccountLongShortRatio));
}

const BinanceSwapAccountLongShortRatioModule&
AltDataManager::binance_swap_account_long_short_ratio() const {
    return static_cast<const BinanceSwapAccountLongShortRatioModule&>(
        *module_by_source_.at(AltDataSource::BinanceSwapAccountLongShortRatio));
}

SymbolCikResolver& AltDataManager::symbol_cik() { return symbol_cik_resolver_; }

const SymbolCikResolver& AltDataManager::symbol_cik() const {
    return symbol_cik_resolver_;
}

std::vector<std::string>
AltDataManager::preload_us_equity_sec_facts_for_symbols(
    const std::vector<std::string>& symbols, long long as_of_ms) {
    if (!symbol_cik_resolver_.loaded()) {
        symbol_cik_resolver_.load(alt_data_root() + "/filings/sec");
    }
    std::vector<std::string> ciks;
    std::vector<std::string> unresolved;
    ciks.reserve(symbols.size());
    for (const auto& symbol : symbols) {
        const auto cik = symbol_cik_resolver_.cik_for(symbol, as_of_ms);
        if (!cik.has_value()) {
            unresolved.push_back(symbol);
            continue;
        }
        ciks.push_back(cik.value());
    }
    preload(AltDataSource::UsEquitySecFacts, ciks);
    std::cout << "[alt-data] symbol -> cik: " << ciks.size() << " resolved, "
              << unresolved.size() << " unresolved of " << symbols.size()
              << std::endl;
    return unresolved;
}

AltDataModule* AltDataManager::module(AltDataSource source) {
    auto it = module_by_source_.find(source);
    return it == module_by_source_.end() ? nullptr : it->second.get();
}

const AltDataModule* AltDataManager::module(AltDataSource source) const {
    auto it = module_by_source_.find(source);
    return it == module_by_source_.end() ? nullptr : it->second.get();
}

std::size_t AltDataManager::preload(AltDataSource source,
                                    const std::vector<std::string>& keys) {
    auto* mod = module(source);
    if (mod == nullptr) {
        std::cerr << "[alt-data] no module registered for " << to_string(source)
                  << "\n";
        return 0;
    }
    const std::size_t resolved = mod->preload(keys);
    std::cout << "[alt-data] " << to_string(source) << ": provisioned "
              << resolved << " of " << keys.size() << " keys, "
              << mod->keys_without_data().size() << " without data"
              << std::endl;
    return resolved;
}

std::size_t AltDataManager::refresh_changed(AltDataSource source) {
    auto* mod = module(source);
    return mod == nullptr ? 0 : mod->refresh_changed();
}

std::size_t AltDataManager::refresh_all_changed() {
    std::size_t total = 0;
    for (auto& [source, mod] : module_by_source_) {
        (void)source;
        total += mod->refresh_changed();
    }
    return total;
}

std::size_t AltDataManager::refresh_keys(AltDataSource source,
                                         const std::vector<std::string>& keys) {
    auto* mod = module(source);
    return mod == nullptr ? 0 : mod->refresh_keys(keys);
}

void AltDataManager::clear() {
    for (auto& [source, mod] : module_by_source_) {
        (void)source;
        mod->clear();
    }
    symbol_cik_resolver_.clear();
}

}  // namespace MarketData
