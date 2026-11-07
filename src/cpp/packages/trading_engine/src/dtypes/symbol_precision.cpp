#include "market_making/dtypes/symbol_precision.h"

#include "market_making/engine.h"

mpfr::mpreal Core::SymbolPrecisionAccess::round_quantity(
    const std::string& symbol, const std::string& exchange_name,
    const std::string& market_type, const mpfr::mpreal& quantity) {
    if (engine_ == nullptr) {
        return quantity;
    }
    return engine_->round_quantity(symbol, exchange_name, market_type,
                                   quantity);
}

Core::SymbolPrecisionAccess& Core::SymbolPrecisionAccess::getInstance() {
    static SymbolPrecisionAccess instance;
    return instance;
}

mpfr::mpreal Core::SymbolPrecisionAccess::round_price(
    const std::string& symbol, const std::string& exchange_name,
    const std::string& market_type, const mpfr::mpreal& price) {
    if (engine_ == nullptr) {
        return price;
    }

    return engine_->round_price(symbol, exchange_name, market_type, price);
}

int Core::SymbolPrecisionAccess::get_qty_precision(
    const std::string& symbol, const std::string& exchange_name,
    const std::string& market_type) {
    if (engine_ == nullptr) {
        return 0;
    }

    return engine_->get_qty_precision(symbol, exchange_name, market_type);
}

int Core::SymbolPrecisionAccess::get_price_precision(
    const std::string& symbol, const std::string& exchange_name,
    const std::string& market_type) {
    if (engine_ == nullptr) {
        return 0;
    }
    return engine_->get_price_precision(symbol, exchange_name, market_type);
}