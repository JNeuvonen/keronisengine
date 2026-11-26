"""US-equity metadata routes: ibkr and polygon are the same catalog."""

from unittest.mock import MagicMock

import pytest
from fastapi import FastAPI
from fastapi.testclient import TestClient

from cpp_live_proxy.api.v1.ccxt import metadata as metadata_mod

QUERY_PAIRS = (
    ("ibkr", "spot"),
    ("ibkr", "stock"),
    ("polygon", "spot"),
    ("polygon", "stock"),
)


@pytest.fixture
def client():
    app = FastAPI()
    app.include_router(metadata_mod.router)
    return TestClient(app)


def _strip_type(rows):
    return [
        {k: v for k, v in row.items() if k != "type"}
        for row in sorted(rows, key=lambda r: r["symbol"])
    ]


def test_four_pairs_same_exchange_info_universe(client):
    payloads = []
    for exchange_name, market_type in QUERY_PAIRS:
        res = client.get(
            "/exchange-info",
            params={"exchange_name": exchange_name, "market_type": market_type},
        )
        assert res.status_code == 200, res.text
        rows = res.json()
        symbols = [row["symbol"] for row in rows]
        assert "AAPL" in symbols
        assert "AAPLUSD" not in symbols
        for row in rows:
            assert row["type"] == market_type
            assert row["quote"] == "USD"
            assert row["precision"]["amount"] == 1.0
            assert row["limits"]["amount"]["min"] == 1.0
            assert row["allows_margin"] is False
        payloads.append(rows)

    stripped = [_strip_type(p) for p in payloads]
    for other in stripped[1:]:
        assert other == stripped[0]


def test_four_pairs_same_get_symbols(client):
    sets = []
    for exchange_name, market_type in QUERY_PAIRS:
        res = client.get(
            "/get-symbols",
            params={"exchange_name": exchange_name, "market_type": market_type},
        )
        assert res.status_code == 200, res.text
        names = res.json()["data"]
        assert "AAPL" in names
        assert "AAPLUSD" not in names
        sets.append(set(names))
    for other in sets[1:]:
        assert other == sets[0]


def test_get_active_symbols_ibkr_equals_polygon(client):
    ibkr = client.get(
        "/get-active-symbols",
        params={"exchange_name": "ibkr", "market_type": "spot"},
    )
    polygon = client.get(
        "/get-active-symbols",
        params={"exchange_name": "polygon", "market_type": "stock"},
    )
    assert ibkr.status_code == 200, ibkr.text
    assert polygon.status_code == 200, polygon.text
    assert ibkr.json()["data"] == polygon.json()["data"]
    assert "AAPL" in ibkr.json()["data"]


@pytest.mark.parametrize("exchange_name", ["ibkr", "polygon"])
def test_swap_rejected(client, exchange_name):
    res = client.get(
        "/exchange-info",
        params={"exchange_name": exchange_name, "market_type": "swap"},
    )
    assert res.status_code == 400
    res_symbols = client.get(
        "/get-symbols",
        params={"exchange_name": exchange_name, "market_type": "swap"},
    )
    assert res_symbols.status_code == 400
    res_active = client.get(
        "/get-active-symbols",
        params={"exchange_name": exchange_name, "market_type": "swap"},
    )
    assert res_active.status_code == 400


def test_backend_does_not_intercept_us_equity(client, monkeypatch):
    backend = MagicMock()
    monkeypatch.setattr(metadata_mod, "BACKEND", backend)
    ei = client.get(
        "/exchange-info",
        params={"exchange_name": "ibkr", "market_type": "spot"},
    )
    gs = client.get(
        "/get-symbols",
        params={"exchange_name": "polygon", "market_type": "stock"},
    )
    ga = client.get(
        "/get-active-symbols",
        params={"exchange_name": "ibkr", "market_type": "spot"},
    )
    assert ei.status_code == 200
    assert gs.status_code == 200
    assert ga.status_code == 200
    backend.get_exchange_info.assert_not_called()
    backend.get_symbols.assert_not_called()
    backend.get_active_symbols.assert_not_called()
