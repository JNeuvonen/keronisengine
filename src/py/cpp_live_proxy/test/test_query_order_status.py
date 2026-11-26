import asyncio

import ccxt
import pytest
from fastapi import HTTPException

import cpp_live_proxy.api.v1.ccxt.orders as orders_api


def _query(monkeypatch, outcome):
    async def query_order(*args):
        if isinstance(outcome, Exception):
            raise outcome
        return outcome

    monkeypatch.setattr(orders_api, "BACKEND", None)
    monkeypatch.setattr(orders_api.ccxt_wrapper, "query_order", query_order)
    return asyncio.new_event_loop().run_until_complete(
        orders_api.query_order("PHAROS/USDT:USDT", "binance", "swap", 50000, "00101")
    )


@pytest.mark.parametrize(
    "outcome, status",
    [
        (ccxt.OrderNotFound("binance {\"code\":-2013}"), 404),
        (None, 404),
        (ccxt.NetworkError("timeout"), 502),
        (ccxt.DDoSProtection("429"), 502),
        (RuntimeError("boom"), 502),
    ],
)
def test_only_a_venue_not_found_is_404(monkeypatch, outcome, status):
    with pytest.raises(HTTPException) as err:
        _query(monkeypatch, outcome)
    assert err.value.status_code == status


def test_found_order_is_returned(monkeypatch):
    assert _query(monkeypatch, {"id": "1"}) == {"data": {"id": "1"}}
