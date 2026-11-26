"""Static fixture data for metadata/read-only routes in test mode.

The real CCXT paths return per-venue data fetched from actual exchange
APIs. In test mode, the engine still needs plausible responses for
startup correctness (exchange_info for precision, symbols for
subscription) but scenarios typically don't care about exact prices.
Fixture values here cover the common case; scenarios that need dynamic
book state inject via the simulator control plane (``/control/set_book``)
rather than through these routes.

Every fixture is keyed by ``(exchange_name, market_type)``. Only CCXT-
routed venues are represented — Polymarket and Aster have their own
direct-signing impersonators and bypass this backend.
"""

from typing import NamedTuple


class _SymbolSpec(NamedTuple):
    base: str
    quote: str
    price: float
    tick: float   # price precision (min price increment)
    step: float   # amount precision (min quantity increment)
    min_qty: float
    min_notional: float


# Key symbols the live strategies touch. Extend as scenarios demand.
_SYMBOLS_BY_VENUE: dict[tuple[str, str], list[_SymbolSpec]] = {
    ("binance", "spot"): [
        _SymbolSpec("BTC", "USDT", 50000.0, 0.01, 1e-5, 1e-5, 5.0),
        _SymbolSpec("ETH", "USDT", 3000.0, 0.01, 1e-4, 1e-4, 5.0),
        _SymbolSpec("SOL", "USDT", 150.0, 0.01, 0.01, 0.01, 5.0),
    ],
    ("binance", "swap"): [
        _SymbolSpec("BTC", "USDT", 50000.0, 0.1, 0.001, 0.001, 5.0),
        _SymbolSpec("ETH", "USDT", 3000.0, 0.01, 0.001, 0.001, 5.0),
        _SymbolSpec("SOL", "USDT", 150.0, 0.001, 1.0, 1.0, 5.0),
    ],
    ("gateio", "spot"): [
        _SymbolSpec("BTC", "USDT", 50000.0, 0.01, 1e-5, 1e-5, 1.0),
        _SymbolSpec("ETH", "USDT", 3000.0, 0.01, 1e-4, 1e-4, 1.0),
    ],
    ("bybit", "spot"): [
        _SymbolSpec("BTC", "USDT", 50000.0, 0.01, 1e-5, 1e-5, 1.0),
    ],
    ("bybit", "swap"): [
        _SymbolSpec("BTC", "USDT", 50000.0, 0.1, 0.001, 0.001, 1.0),
    ],
    ("okx", "spot"): [
        _SymbolSpec("BTC", "USDT", 50000.0, 0.01, 1e-5, 1e-5, 1.0),
    ],
    ("okx", "swap"): [
        _SymbolSpec("BTC", "USDT", 50000.0, 0.1, 0.001, 0.001, 1.0),
    ],
    ("hyperliquid", "swap"): [
        _SymbolSpec("BTC", "USD", 50000.0, 0.5, 0.0001, 0.0001, 10.0),
        _SymbolSpec("ETH", "USD", 3000.0, 0.05, 0.001, 0.001, 10.0),
    ],
}


def _native_symbol(spec: _SymbolSpec) -> str:
    """Native concat format — matches production's
    ``ccxt_wrapper.get_exchange_symbols_info`` output which emits
    ``info["base"] + info["quote"]`` (e.g., ``"BTCUSDT"``). This is the
    format that ends up in ``Account::active_symbols`` and that
    ``symbol_exists`` is checked against during ``init_strategies`` —
    trade_ticker DB rows must match for the ticker's
    ``exchange_and_markets`` to be non-empty.
    """
    return f"{spec.base}{spec.quote}"


def _specs(exchange_name: str, market_type: str) -> list[_SymbolSpec]:
    return _SYMBOLS_BY_VENUE.get((exchange_name, market_type), [])


def build_exchange_info(exchange_name: str, market_type: str) -> list[dict]:
    """Native-symbol per-symbol metadata list.

    Shape + symbol format mirror production
    ``ccxt_wrapper.get_exchange_symbols_info`` (``info["base"] +
    info["quote"]``). The engine's ``Account::active_symbols`` is
    populated from this response; trade_ticker DB rows must use the
    same native format for ``symbol_exists`` to match.
    """
    return [
        {
            "symbol": _native_symbol(s),
            "base": s.base,
            "quote": s.quote,
            "is_active": True,
            "type": market_type,
            "limits": {
                "amount": {"min": s.min_qty, "max": 1e9},
                "price": {"min": s.tick, "max": 1e9},
                "cost": {"min": s.min_notional, "max": None},
            },
            "precision": {
                "price": s.tick,
                "amount": s.step,
                "base": None,
                "quote": None,
            },
        }
        for s in _specs(exchange_name, market_type)
    ]


def build_symbols_list(exchange_name: str, market_type: str) -> list[str]:
    """List of native-format symbols for (exchange, market_type)."""
    return [_native_symbol(s)
            for s in _specs(exchange_name, market_type)]


def build_prices(exchange_name: str, market_type: str) -> list[dict]:
    """Fixture last-trade prices per symbol."""
    return [
        {"symbol": _native_symbol(s), "price": s.price}
        for s in _specs(exchange_name, market_type)
    ]


def build_orderbook(symbol: str, exchange_name: str, market_type: str,
                    limit: int = 1000) -> dict:
    """One-level CCXT-shaped orderbook around the fixture price.

    Scenarios that need multi-level books with specific quantities should
    post to the simulator's ``/control/set_book`` and read back via the
    simulator, rather than calling this route.
    """
    import time
    # Try to find the symbol in the fixture; fall back to a BTC-ish price.
    price: float = 50000.0
    for s in _specs(exchange_name, market_type):
        if _native_symbol(s) == symbol:
            price = s.price
            break
    spread = price * 0.0001  # 1 bp
    now_ms = int(time.time() * 1000)
    return {
        "bids": [[price - spread, 1.0]],
        "asks": [[price + spread, 1.0]],
        "timestamp": now_ms,
        "nonce": now_ms,
    }


def build_empty_inventory_state() -> dict:
    """Empty balance + positions shape.

    Scenarios that need specific balances inject them directly via the
    engine control plane (``/control/engine/quote-asset``,
    ``/control/engine/inventory``) rather than through this route.
    """
    return {
        "inventory_state": {
            "quote_assets": [
                {
                    "asset": "USDT",
                    "quantity": "100000.0",
                    "free": "100000.0",
                    "used": "0",
                    "price": 1,
                },
            ],
            "inventory": [],
        }
    }


def build_empty_borrowable_inventories(symbols: list[str]) -> list[dict]:
    """One entry per requested symbol, all with zero borrow capacity."""
    return [
        {"symbol": s, "available_for_borrow": "0"} for s in symbols
    ]
