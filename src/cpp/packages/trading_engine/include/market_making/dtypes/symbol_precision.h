#pragma once

#include <memory>

#include "mpreal.h"

namespace Core {

class Engine;

class SymbolPrecisionAccess {
   public:
    static SymbolPrecisionAccess& getInstance();

    void initialize(Engine* engine) { engine_ = engine; }

    mpfr::mpreal round_quantity(const std::string& symbol,
                                const std::string& exchange_name,
                                const std::string& market_type,
                                const mpfr::mpreal& quantity);

    mpfr::mpreal round_price(const std::string& symbol,
                             const std::string& exchange_name,
                             const std::string& market_type,
                             const mpfr::mpreal& price);

    int get_qty_precision(const std::string& symbol,
                          const std::string& exchange_name,
                          const std::string& market_type);
    int get_price_precision(const std::string& symbol,
                            const std::string& exchange_name,
                            const std::string& market_type);

   private:
    SymbolPrecisionAccess() = default;
    SymbolPrecisionAccess(const SymbolPrecisionAccess&) =
        delete;  // Prevent copying
    SymbolPrecisionAccess& operator=(const SymbolPrecisionAccess&) =
        delete;  // Prevent assignment

    // Explicitly null: every accessor treats null as "precision unknown"
    // and returns 0, so an uninitialised pointer here would be read as
    // "zero decimal places" rather than crashing -- silent truncation.
    Engine* engine_ = nullptr;
};

}  // namespace Core