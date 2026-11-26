"""Test-suite fixtures.

Auto-mocks the US equity catalog's Polygon fetch for all tests in this
directory so IBKRAccountClient construction doesn't try to hit Polygon
or read DATA_DIR_ROOT cache. Any test that wants to override the stub
universe can patch `_fetch_from_polygon` locally.
"""

import os
import tempfile

import pytest


_DEFAULT_STUB_ROWS = [
    {"ticker": "AAPL", "name": "Apple Inc.", "type": "CS",
     "primary_exchange": "XNAS", "currency": "usd"},
    {"ticker": "MSFT", "name": "Microsoft Corp.", "type": "CS",
     "primary_exchange": "XNAS", "currency": "usd"},
    {"ticker": "NVDA", "name": "NVIDIA Corp.", "type": "CS",
     "primary_exchange": "XNAS", "currency": "usd"},
    {"ticker": "SPY", "name": "SPDR S&P 500 ETF", "type": "ETF",
     "primary_exchange": "ARCX", "currency": "usd"},
    {"ticker": "QQQ", "name": "Invesco QQQ", "type": "ETF",
     "primary_exchange": "ARCX", "currency": "usd"},
    {"ticker": "ASML", "name": "ASML Holding NV", "type": "CS",
     "primary_exchange": "XNAS", "currency": "usd"},
]


@pytest.fixture(autouse=True)
def _stub_us_equity_catalog(monkeypatch):
    """Replace Polygon fetch with a fixed stub list and isolate the
    cache under a tmp DATA_DIR_ROOT for every test in this directory.
    Tests that exercise the catalog's own logic (fetch failures, stale
    cache, etc.) override this via a more specific patch."""
    from cpp_live_proxy import us_equity_catalog as cat

    cat.USEquityCatalog.reset_for_tests()
    monkeypatch.setattr(
        cat, "_fetch_from_polygon", lambda: list(_DEFAULT_STUB_ROWS)
    )
    with tempfile.TemporaryDirectory() as td:
        monkeypatch.setenv("DATA_DIR_ROOT", td)
        yield
    cat.USEquityCatalog.reset_for_tests()
