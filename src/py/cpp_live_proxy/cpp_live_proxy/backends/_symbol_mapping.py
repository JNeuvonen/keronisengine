"""CCXT symbol format <-> exchange-native format.

CCXT canonical: ``BTC/USDT`` (spot), ``BTC/USDT:USDT`` (swap/perp).
Exchange-native: ``BTCUSDT`` (no separators) for every venue we route through
the proxy today (Binance, Gate, Bybit, OKX, Hyperliquid, Aster).

Heuristic conversion is sufficient for the impersonator: scenarios deal in
known symbol sets (BTCUSDT, ETHUSDT, etc.). If a scenario needs richer
mapping (futures with different settle assets, etc.), extend this module.
"""

_KNOWN_QUOTES = ("USDT", "USDC", "USD", "BUSD", "BTC", "ETH", "EUR")


def ccxt_to_native(ccxt_symbol: str) -> str:
    """``BTC/USDT:USDT`` -> ``BTCUSDT``; ``BTC/USDT`` -> ``BTCUSDT``."""
    s = ccxt_symbol.split(":")[0]
    return s.replace("/", "")


def native_to_ccxt(native_symbol: str, market_type: str) -> str:
    """``BTCUSDT`` -> ``BTC/USDT`` (spot) or ``BTC/USDT:USDT`` (swap).

    Falls back to the input string unchanged if the quote can't be inferred.
    """
    for quote in _KNOWN_QUOTES:
        if native_symbol.endswith(quote) and len(native_symbol) > len(quote):
            base = native_symbol[: -len(quote)]
            ccxt = f"{base}/{quote}"
            if market_type == "swap":
                ccxt = f"{ccxt}:{quote}"
            return ccxt
    return native_symbol
