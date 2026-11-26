"""Unit tests for cpp_live_proxy.us_equity_catalog.

Polygon is mocked end-to-end. No network calls; cache file is written
under a tmp DATA_DIR_ROOT.
"""

import json
import os
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest.mock import patch

from cpp_live_proxy import us_equity_catalog as cat


def _polygon_rows():
    return [
        {"ticker": "AAPL", "name": "Apple Inc.", "type": "CS",
         "primary_exchange": "XNAS", "currency": "usd"},
        {"ticker": "SPY", "name": "SPDR S&P 500 ETF", "type": "ETF",
         "primary_exchange": "ARCX", "currency": "usd"},
        {"ticker": "BRK.B", "name": "Berkshire Hathaway B", "type": "CS",
         "primary_exchange": "XNYS", "currency": "usd"},
    ]


class TestUSEquityCatalog(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self._prev_data_dir = os.environ.get("DATA_DIR_ROOT")
        os.environ["DATA_DIR_ROOT"] = self._tmp.name
        cat.USEquityCatalog.reset_for_tests()

    def tearDown(self):
        cat.USEquityCatalog.reset_for_tests()
        if self._prev_data_dir is None:
            os.environ.pop("DATA_DIR_ROOT", None)
        else:
            os.environ["DATA_DIR_ROOT"] = self._prev_data_dir
        self._tmp.cleanup()

    def test_cold_boot_fetches_from_polygon_and_writes_cache(self):
        with patch.object(cat, "_fetch_from_polygon", return_value=_polygon_rows()):
            inst = cat.USEquityCatalog.get_instance()
        markets = inst.markets()
        self.assertEqual(set(markets.keys()), {"AAPL", "SPY", "BRK.B"})
        # Cache file exists and is well-formed.
        p = Path(self._tmp.name) / cat.CACHE_REL_PATH
        self.assertTrue(p.is_file())
        payload = json.loads(p.read_text())
        self.assertEqual(len(payload["rows"]), 3)
        self.assertIn("fetched_at_unix", payload)

    def test_warm_boot_reads_cache_without_polygon_call(self):
        # Pre-populate cache.
        p = Path(self._tmp.name) / cat.CACHE_REL_PATH
        p.parent.mkdir(parents=True, exist_ok=True)
        payload = {
            "fetched_at_unix": time.time(),  # fresh
            "rows": _polygon_rows(),
        }
        p.write_text(json.dumps(payload))

        with patch.object(cat, "_fetch_from_polygon") as mock_fetch:
            inst = cat.USEquityCatalog.get_instance()
            mock_fetch.assert_not_called()

        markets = inst.markets()
        self.assertEqual(set(markets.keys()), {"AAPL", "SPY", "BRK.B"})

    def test_stale_cache_triggers_refresh(self):
        # Cache older than refresh interval.
        p = Path(self._tmp.name) / cat.CACHE_REL_PATH
        p.parent.mkdir(parents=True, exist_ok=True)
        old_payload = {
            "fetched_at_unix": time.time() - (48 * 3600),
            "rows": [{"ticker": "OLD", "name": "Old", "type": "CS",
                      "primary_exchange": "X", "currency": "usd"}],
        }
        p.write_text(json.dumps(old_payload))

        with patch.object(cat, "_fetch_from_polygon", return_value=_polygon_rows()) as mock_fetch:
            inst = cat.USEquityCatalog.get_instance()
            mock_fetch.assert_called_once()

        markets = inst.markets()
        self.assertNotIn("OLD", markets)
        self.assertIn("AAPL", markets)

    def test_stale_cache_refresh_failure_keeps_old_data(self):
        old_payload = {
            "fetched_at_unix": time.time() - (48 * 3600),
            "rows": [{"ticker": "STALE", "name": "Stale", "type": "CS",
                      "primary_exchange": "X", "currency": "usd"}],
        }
        p = Path(self._tmp.name) / cat.CACHE_REL_PATH
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(json.dumps(old_payload))

        def boom():
            raise RuntimeError("Polygon outage")

        with patch.object(cat, "_fetch_from_polygon", side_effect=boom):
            inst = cat.USEquityCatalog.get_instance()

        # Stale data preserved — never zeroed out by the failed refresh.
        markets = inst.markets()
        self.assertIn("STALE", markets)

    def test_cold_boot_polygon_failure_raises(self):
        # No cache + Polygon down → must fail loudly per design.
        def boom():
            raise RuntimeError("Polygon outage")

        with patch.object(cat, "_fetch_from_polygon", side_effect=boom):
            with self.assertRaises(Exception):
                cat.USEquityCatalog.get_instance()

    def test_polygon_returns_zero_rows_is_treated_as_failure(self):
        # Defensive: a 0-row response is ambiguous (real outage vs zero
        # active equities) and we'd rather not overwrite cache with [].
        with patch.object(cat, "_fetch_from_polygon", return_value=[]):
            with self.assertRaises(Exception):
                cat.USEquityCatalog.get_instance()

    def test_markets_entries_are_ccxt_shape(self):
        with patch.object(cat, "_fetch_from_polygon", return_value=_polygon_rows()):
            inst = cat.USEquityCatalog.get_instance()
        m = inst.markets()["AAPL"]
        # Required fields the engine + downstream filters read.
        self.assertEqual(m["base"], "AAPL")
        self.assertEqual(m["quote"], "USD")
        self.assertEqual(m["type"], "stock")
        self.assertTrue(m["active"])
        self.assertTrue(m["spot"])
        self.assertFalse(m["swap"])
        self.assertFalse(m["contract"])
        self.assertEqual(m["precision"]["price"], 0.01)
        self.assertEqual(m["precision"]["amount"], 1.0)
        # info preserves Polygon-native fields for debug.
        self.assertEqual(m["info"]["polygon_type"], "CS")
        self.assertEqual(m["info"]["primary_exchange"], "XNAS")

    def test_markets_per_call_isolated_from_internal_state(self):
        with patch.object(cat, "_fetch_from_polygon", return_value=_polygon_rows()):
            inst = cat.USEquityCatalog.get_instance()
        m = inst.markets()
        m["AAPL"]["active"] = False
        # Caller mutation must NOT leak back into the singleton.
        self.assertTrue(inst.markets()["AAPL"]["active"])

    def test_brk_b_with_period_preserved(self):
        # The old fetch_symbols stripped "." tickers. Our new path keeps
        # them — BRK.B is a real, tradeable stock.
        with patch.object(cat, "_fetch_from_polygon", return_value=_polygon_rows()):
            inst = cat.USEquityCatalog.get_instance()
        self.assertIn("BRK.B", inst.markets())

    def test_exchange_info_rows_native_ticker_not_concat(self):
        with patch.object(cat, "_fetch_from_polygon", return_value=_polygon_rows()):
            inst = cat.USEquityCatalog.get_instance()
        rows = inst.exchange_info_rows("stock")
        by_symbol = {r["symbol"]: r for r in rows}
        self.assertEqual(set(by_symbol), {"AAPL", "SPY", "BRK.B"})
        self.assertNotIn("AAPLUSD", by_symbol)
        aapl = by_symbol["AAPL"]
        self.assertEqual(aapl["base"], "AAPL")
        self.assertEqual(aapl["quote"], "USD")
        self.assertEqual(aapl["precision"]["amount"], 1.0)
        self.assertEqual(aapl["precision"]["price"], 0.01)
        self.assertEqual(aapl["limits"]["amount"]["min"], 1.0)
        self.assertEqual(aapl["limits"]["cost"]["min"], 1.0)
        self.assertFalse(aapl["allows_margin"])
        self.assertEqual(aapl["type"], "stock")
        self.assertTrue(aapl["is_active"])

    def test_exchange_info_rows_spot_same_universe_only_type_differs(self):
        with patch.object(cat, "_fetch_from_polygon", return_value=_polygon_rows()):
            inst = cat.USEquityCatalog.get_instance()
        stock = sorted(inst.exchange_info_rows("stock"), key=lambda r: r["symbol"])
        spot = sorted(inst.exchange_info_rows("spot"), key=lambda r: r["symbol"])
        self.assertEqual([r["symbol"] for r in stock], [r["symbol"] for r in spot])
        for s, p in zip(stock, spot):
            self.assertEqual(s["precision"], p["precision"])
            self.assertEqual(s["limits"], p["limits"])
            self.assertEqual(s["is_active"], p["is_active"])
            self.assertEqual(s["base"], p["base"])
            self.assertEqual(s["quote"], p["quote"])
            self.assertEqual(s["type"], "stock")
            self.assertEqual(p["type"], "spot")

    def test_exchange_info_and_symbol_names_keep_inactive(self):
        rows = _polygon_rows() + [
            {"ticker": "DEAD", "name": "Dead Co", "type": "CS",
             "primary_exchange": "X", "currency": "usd", "active": False},
        ]
        with patch.object(cat, "_fetch_from_polygon", return_value=rows):
            inst = cat.USEquityCatalog.get_instance()
        info = {r["symbol"]: r for r in inst.exchange_info_rows("stock")}
        self.assertIn("DEAD", info)
        self.assertFalse(info["DEAD"]["is_active"])
        self.assertEqual(
            inst.symbol_names(active_only=False),
            ["AAPL", "BRK.B", "DEAD", "SPY"],
        )
        self.assertEqual(
            inst.symbol_names(active_only=True),
            ["AAPL", "BRK.B", "SPY"],
        )


class TestPolygonFilterAtFetchLayer(unittest.TestCase):
    """fetch_us_equity_tickers should filter to CS + ETF only — exclude
    WARRANT/RIGHT/UNIT/etc. Tests the polygon.py method directly with a
    mocked underlying client."""

    def _make_fake_polygon_api(self, rows):
        # PolygonAPI.__new__ reads Secrets().polygon_api_key, so we
        # short-circuit by setting _instance directly. The rows are
        # plain SimpleNamespace objects that emulate the polygon-api
        # client's Ticker objects (ticker / name / type / etc.).
        from keronis_utils.polygon import PolygonAPI
        from types import SimpleNamespace

        # Pre-set _instance so __new__ skips Secrets().
        PolygonAPI._instance = None  # ensure clean state
        api = object.__new__(PolygonAPI)
        api._client = SimpleNamespace(list_tickers=lambda **kw: iter(rows))
        return api

    def tearDown(self):
        from keronis_utils.polygon import PolygonAPI
        PolygonAPI._instance = None

    def test_filters_to_common_stock_and_etf(self):
        from types import SimpleNamespace
        rows = [
            SimpleNamespace(ticker="AAPL", name="Apple", type="CS",
                            primary_exchange="XNAS", currency_name="usd"),
            SimpleNamespace(ticker="SPY", name="SPDR", type="ETF",
                            primary_exchange="ARCX", currency_name="usd"),
            SimpleNamespace(ticker="ABCW", name="Some Warrant", type="WARRANT",
                            primary_exchange="XNYS", currency_name="usd"),
            SimpleNamespace(ticker="ABCR", name="Some Right", type="RIGHT",
                            primary_exchange="XNYS", currency_name="usd"),
            SimpleNamespace(ticker="ABCU", name="Some Unit", type="UNIT",
                            primary_exchange="XNYS", currency_name="usd"),
        ]
        api = self._make_fake_polygon_api(rows)
        out = api.fetch_us_equity_tickers(active=True)
        tickers = [r["ticker"] for r in out]
        self.assertEqual(tickers, ["AAPL", "SPY"])

    def test_returns_metadata_dicts_not_strings(self):
        from types import SimpleNamespace
        rows = [
            SimpleNamespace(ticker="AAPL", name="Apple", type="CS",
                            primary_exchange="XNAS", currency_name="usd"),
        ]
        api = self._make_fake_polygon_api(rows)
        out = api.fetch_us_equity_tickers(active=True)
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0]["ticker"], "AAPL")
        self.assertEqual(out[0]["name"], "Apple")
        self.assertEqual(out[0]["type"], "CS")
        self.assertEqual(out[0]["primary_exchange"], "XNAS")


if __name__ == "__main__":
    unittest.main()
