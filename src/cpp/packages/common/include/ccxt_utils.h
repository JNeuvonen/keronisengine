#pragma once

#include <string>

namespace CCXTUtils {

std::string convert_ccxt_symbol(const std::string& ccxt_symbol_format);
std::string convert_gateio_symbol(const std::string& gateio_symbol);
std::string symbol_to_ccxt_format(const std::string& symbol,
                                  const std::string& swap_currency);
std::pair<std::string, std::string> infer_base_quote_assets(
    const std::string& symbol, const std::string& default_quote_asset = "");

// Per-venue allow-list of quote assets that engine bookkeeping (auto-
// enumerated trade tickers, symbols-info filters, etc.) accepts.
// Crypto venues are stablecoin-quoted (USDT for existing strategies);
// IBKR is USD-quoted for US equities. The set is expected to grow
// (USDC for new crypto strats; EUR for IBKR European listings) so the
// predicate lives in one place rather than being inlined per-call-site.
bool is_supported_quote_for_venue(const std::string& exchange_name,
                                  const std::string& quote_asset);

// Hyperliquid perps are uniformly USDC-quoted, while the HL API/WS speaks the
// bare coin ("BTC"). The rest of the system speaks the canonical base+quote
// form ("BTCUSDC"), so all HL symbol juggling is a fixed USDC append/strip at
// the venue boundary (direct client, WS parse, market-data).
std::string hyperliquid_coin_to_symbol(const std::string& coin);
std::string hyperliquid_symbol_to_coin(const std::string& symbol);

}  // namespace CCXTUtils
