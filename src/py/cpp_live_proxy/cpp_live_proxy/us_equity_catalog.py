"""US equity symbol catalog.

Source of truth: Polygon `/v3/reference/tickers`. The free tier covers
the full ~12k active US equity universe. We cache to disk because:

1. Proxy boot must be fast and tolerate Polygon being briefly down.
2. The universe changes only on listings/delistings (end-of-day events),
   so a 24h refresh cadence is sufficient.

Cache file: ``$DATA_DIR_ROOT/caches/us_equity_symbols.json``.
"""

from __future__ import annotations

import json
import os
import threading
import time
from pathlib import Path
from typing import Any, Dict, List, Optional

from cpp_live_proxy.gcp_log import log


CACHE_REL_PATH = "caches/us_equity_symbols.json"
# The catalog always carries the FULL universe, delisted included. It is a
# description of what exists, not a permission list, so making completeness
# opt-in is how a survivor-only universe gets shipped by accident -- which is
# exactly what `active=True` did here for years.
#
# Selection belongs to the consumer, and the flag to do it already exists:
# entries carry their real `active` value, and `get_symbols(active=True)`
# filters them. Live trading asks for active names; a history backfill asks for
# everything and gets it without having to know a flag exists.
DEFAULT_REFRESH_INTERVAL_S = 24 * 3600
# Tick / lot rules are uniform across the US equity universe we care
# about. Sub-$1 stocks have $0.0001 tick but our strategies don't trade
# those; if that changes, override per-symbol or split the seed.
DEFAULT_PRICE_TICK = 0.01
DEFAULT_LOT_SIZE = 1.0

US_EQUITY_EXCHANGES = frozenset({"ibkr", "polygon"})
US_EQUITY_MARKET_TYPES = frozenset({"spot", "stock"})


def _cache_path() -> Path:
    root = os.environ.get("DATA_DIR_ROOT")
    if not root:
        raise RuntimeError(
            "DATA_DIR_ROOT not set; cannot resolve us_equity_catalog cache path"
        )
    return Path(root) / CACHE_REL_PATH


def _read_cache() -> Optional[Dict[str, Any]]:
    p = _cache_path()
    if not p.is_file():
        return None
    try:
        return json.loads(p.read_text())
    except Exception as e:
        log(
            f"us_equity_catalog: cache read failed ({p}): {type(e).__name__}: {e}",
            {},
            "ERROR",
        )
        return None


def _write_cache(payload: Dict[str, Any]) -> None:
    p = _cache_path()
    p.parent.mkdir(parents=True, exist_ok=True)
    tmp = p.with_suffix(p.suffix + ".tmp")
    tmp.write_text(json.dumps(payload, indent=2))
    tmp.replace(p)


def _fetch_from_polygon() -> List[Dict[str, Any]]:
    local_key = os.environ.get("POLYGON_API_KEY")
    if not local_key:
        raise RuntimeError(
            "POLYGON_API_KEY env var is required to refresh the US equity catalog"
        )
    # Both listing states: delisted names are where every company that went
    # to zero lives, and a short book that never sees them measures a
    # survivor sample.
    states = [True, False]
    from polygon import RESTClient as _RESTClient
    client = _RESTClient(local_key)

    out = []
    for active in states:
        for ticker in client.list_tickers(
            market="stocks", active=active, order="asc", limit=1000,
        ):
            t_type = getattr(ticker, "type", None)
            if t_type not in ("CS", "ETF"):
                continue
            out.append({
                "ticker": ticker.ticker,
                "name": getattr(ticker, "name", "") or "",
                "type": t_type,
                "primary_exchange": getattr(ticker, "primary_exchange", "")
                or "",
                "currency": getattr(ticker, "currency_name", "usd") or "usd",
                "active": bool(getattr(ticker, "active", active)),
                "list_date": getattr(ticker, "list_date", "") or "",
                "delisted_utc": getattr(ticker, "delisted_utc", "") or "",
            })
    return _dedupe(out)


def _dedupe(rows: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
    """One row per ticker, preferring the active listing.

    A symbol can appear in both the active and delisted responses when it was
    delisted and later reissued. The live entry has to win, or the engine would
    treat a currently-tradeable symbol as dead.
    """
    best: Dict[str, Dict[str, Any]] = {}
    for row in rows:
        key = row["ticker"]
        prev = best.get(key)
        if prev is None or (row.get("active") and not prev.get("active")):
            best[key] = row
    return sorted(best.values(), key=lambda r: r["ticker"])


def _to_ccxt_metadata(entry: Dict[str, Any]) -> Dict[str, Any]:
    """Build an IBKRAccountClient.markets entry from a Polygon row.

    Mirrors the shape ccxt's load_markets() emits for crypto venues so
    code paths that read these markets (CcxtWrapper.get_symbols,
    get_exchange_info) don't have to special-case IBKR.
    """
    return {
        "base": entry["ticker"],
        "quote": "USD",
        "active": bool(entry.get("active", True)),
        "type": "stock",
        # ccxt classification flags — direct-accessed by get_symbols.
        "spot": True,
        "swap": False,
        "future": False,
        "contract": False,
        "linear": False,
        "inverse": False,
        "expiry": None,
        "margin": False,
        "precision": {
            "price": DEFAULT_PRICE_TICK,
            "amount": DEFAULT_LOT_SIZE,
            "base": DEFAULT_LOT_SIZE,
            "quote": DEFAULT_PRICE_TICK,
        },
        "limits": {
            "amount": {"min": DEFAULT_LOT_SIZE, "max": 1_000_000.0},
            "price": {"min": DEFAULT_PRICE_TICK, "max": None},
            "cost": {"min": 1.0, "max": None},
        },
        # Carry venue-native fields under `info` for debug / future logic.
        "info": {
            "name": entry.get("name", ""),
            "polygon_type": entry.get("type", ""),
            "primary_exchange": entry.get("primary_exchange", ""),
            "list_date": entry.get("list_date", ""),
            "delisted_utc": entry.get("delisted_utc", ""),
        },
    }


class USEquityCatalog:
    """Process singleton holding the current US-equity symbol universe.

    Boot order:
      1. read cache file if present (fast)
      2. if missing OR older than refresh_interval_s: fetch from Polygon
      3. if Polygon fetch fails AND cache exists: keep stale cache, log
         WARNING, continue. Background daemon will retry.
      4. if Polygon fetch fails AND no cache: raise. Operator must fix
         the Polygon key / connectivity before the proxy can boot.

    Background daemon refreshes every refresh_interval_s and atomically
    swaps the markets dict on success. Failures are logged but never
    crash the daemon thread.
    """

    _instance: "Optional[USEquityCatalog]" = None
    _construction_lock: threading.Lock = threading.Lock()

    def __init__(self, refresh_interval_s: float = DEFAULT_REFRESH_INTERVAL_S):
        self._lock = threading.Lock()
        self._markets: Dict[str, Dict[str, Any]] = {}
        self._last_refresh_ts: float = 0.0
        self._refresh_interval_s = refresh_interval_s
        self._refresh_thread: Optional[threading.Thread] = None
        self._stop = threading.Event()

    @classmethod
    def get_instance(cls) -> "USEquityCatalog":
        with cls._construction_lock:
            if cls._instance is None:
                inst = cls()
                inst._initial_load()
                inst._start_refresh_daemon()
                cls._instance = inst
            return cls._instance

    @classmethod
    def reset_for_tests(cls) -> None:
        with cls._construction_lock:
            if cls._instance is not None:
                cls._instance._stop.set()
                t = cls._instance._refresh_thread
                if t is not None and t.is_alive():
                    t.join(timeout=2)
            cls._instance = None

    def _initial_load(self) -> None:
        cached = _read_cache()
        now = time.time()
        if cached is None:
            log(
                "us_equity_catalog: no cache; fetching from Polygon",
                {},
                "INFO",
            )
            self._refresh_from_polygon()
            return

        cached_ts = float(cached.get("fetched_at_unix", 0))
        age_s = now - cached_ts
        # Always populate from cache first so reads work immediately
        # while a slow refresh runs.
        self._populate_from_payload(cached)
        log(
            f"us_equity_catalog: loaded {len(self._markets)} tickers from cache "
            f"(age={age_s:.0f}s)",
            {},
            "INFO",
        )
        if age_s > self._refresh_interval_s:
            try:
                self._refresh_from_polygon()
            except Exception as e:
                log(
                    f"us_equity_catalog: stale-cache refresh failed; "
                    f"keeping {age_s:.0f}s-old cache: "
                    f"{type(e).__name__}: {e}",
                    {},
                    "WARNING",
                )

    def _refresh_from_polygon(self) -> None:
        rows = _fetch_from_polygon()
        if not rows:
            raise RuntimeError(
                "Polygon returned 0 US equity tickers; refusing to overwrite cache"
            )
        payload = {
            "fetched_at_unix": time.time(),
            "rows": rows,
        }
        _write_cache(payload)
        self._populate_from_payload(payload)
        log(
            f"us_equity_catalog: refreshed {len(rows)} tickers from Polygon",
            {},
            "INFO",
        )

    def _populate_from_payload(self, payload: Dict[str, Any]) -> None:
        with self._lock:
            self._markets = {
                row["ticker"]: _to_ccxt_metadata(row)
                for row in payload.get("rows", [])
            }
            self._last_refresh_ts = float(
                payload.get("fetched_at_unix", time.time())
            )

    def markets(self) -> Dict[str, Dict[str, Any]]:
        """Return a copy of the current markets dict. Copy so callers
        can mutate freely without races against the refresh daemon."""
        with self._lock:
            return {sym: dict(m) for sym, m in self._markets.items()}

    def exchange_info_rows(self, market_type: str) -> List[Dict[str, Any]]:
        rows = []
        for symbol, m in self.markets().items():
            rows.append(
                {
                    "symbol": symbol,
                    "base": m["base"],
                    "quote": m["quote"],
                    "is_active": bool(m["active"]),
                    "type": market_type,
                    "allows_margin": False,
                    "precision": {
                        "price": m["precision"]["price"],
                        "amount": m["precision"]["amount"],
                        "base": m["precision"]["base"],
                        "quote": m["precision"]["quote"],
                    },
                    "limits": {
                        "amount": {
                            "min": m["limits"]["amount"]["min"],
                            "max": m["limits"]["amount"]["max"],
                        },
                        "price": {
                            "min": m["limits"]["price"]["min"],
                            "max": m["limits"]["price"]["max"],
                        },
                        "cost": {
                            "min": m["limits"]["cost"]["min"],
                            "max": m["limits"]["cost"]["max"],
                        },
                    },
                }
            )
        return rows

    def symbol_names(self, *, active_only: bool) -> List[str]:
        names = [
            symbol
            for symbol, m in self.markets().items()
            if (not active_only) or m["active"]
        ]
        names.sort()
        return names

    def last_refresh_ts(self) -> float:
        with self._lock:
            return self._last_refresh_ts

    def _start_refresh_daemon(self) -> None:
        self._refresh_thread = threading.Thread(
            target=self._refresh_loop,
            name="us-equity-catalog-refresh",
            daemon=True,
        )
        self._refresh_thread.start()

    def _refresh_loop(self) -> None:
        while not self._stop.wait(timeout=self._refresh_interval_s):
            try:
                self._refresh_from_polygon()
            except Exception as e:
                log(
                    f"us_equity_catalog: background refresh failed: "
                    f"{type(e).__name__}: {e}",
                    {},
                    "ERROR",
                )
