import asyncio

import pytest

from cpp_live_proxy.ccxt_wrapper import CcxtWrapper
from cpp_live_proxy.type_models import Order


class RecordingClient:
    def __init__(self):
        self.calls = []

    async def create_order(self, **kwargs):
        self.calls.append(kwargs)
        return {"id": "1", "status": "expired", "filled": 0.0, "info": {}}


def make_order(order_type, exchange="binance"):
    return Order(
        type=order_type, side="buy", status="open", symbol="BTC/USDT:USDT",
        exchange_name=exchange, market_type="swap", quote_asset="USDT",
        base_asset="BTC", create_timestamp=0, expires_on_ms=0, account_id=50000,
        price="50000", quantity="0.002", filled_quantity="0",
        sizing_coeff_delta="1", client_order_id="c1", strategy_id=9,
        position_side="LONG",
    )


@pytest.mark.parametrize(
    "order_type, exchange, ccxt_type, time_in_force",
    [
        ("immediate_or_cancel", "binance", "limit", "IOC"),
        ("fill_or_kill", "binance", "limit", "FOK"),
        ("immediate_or_cancel", "bybit", "limit", "IOC"),
        ("limit", "binance", "limit", None),
    ],
)
def test_time_in_force_rides_on_a_limit_order(
    monkeypatch, order_type, exchange, ccxt_type, time_in_force
):
    client = RecordingClient()
    wrapper = CcxtWrapper()
    monkeypatch.setattr(wrapper, "_get_target_client_from_order", lambda order: client)

    result = asyncio.new_event_loop().run_until_complete(
        wrapper.create_order(make_order(order_type, exchange))
    )

    assert result.success
    (call,) = client.calls
    assert call["type"] == ccxt_type
    assert call["params"].get("timeInForce") == time_in_force
