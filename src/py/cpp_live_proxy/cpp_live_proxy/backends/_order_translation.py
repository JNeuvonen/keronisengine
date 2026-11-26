"""Translate cpp_live_proxy ``Order`` (Pydantic) <-> simulator ``Order`` (JSON).

The simulator's ``MatchingEngine`` accepts the FullMarketSimulator::Order
shape. The proxy receives ``cpp_live_proxy.type_models.Order``. This module
is the only place that knows both shapes.
"""

from typing import Optional

from cpp_live_proxy.backends import _status_mapping, _symbol_mapping
from cpp_live_proxy.type_models import Order as ProxyOrder


_TYPE_TO_SIM = {
    "limit": "Limit",
    "market": "Market",
    "limit_maker": "PostOnly",
    "post_only": "PostOnly",
}


def _normalize_side(side: str) -> str:
    s = side.lower()
    if s in ("buy", "bid"):
        return "Buy"
    if s in ("sell", "ask"):
        return "Sell"
    raise ValueError(f"unrecognized side: {side!r}")


def _normalize_type(order_type: str, post_only: bool) -> str:
    if post_only:
        return "PostOnly"
    sim_type = _TYPE_TO_SIM.get(order_type.lower())
    if sim_type is None:
        raise ValueError(f"unrecognized order type: {order_type!r}")
    return sim_type


def proxy_order_to_sim_json(order: ProxyOrder) -> dict:
    """Build the JSON body for ``POST /control/submit_order``."""
    native_symbol = _symbol_mapping.ccxt_to_native(order.symbol)
    return {
        "client_order_id": order.client_order_id,
        "venue": order.exchange_name,
        "symbol": native_symbol,
        "side": _normalize_side(order.side),
        "type": _normalize_type(order.type, order.post_only),
        "price": float(order.price) if order.price else 0.0,
        "qty": float(order.quantity),
        "submit_ts_ms": order.create_timestamp,
        "venue_specific": {
            "account_id": order.account_id,
            "market_type": order.market_type,
            "reduce_only": order.reduce_only,
            "strategy_id": order.strategy_id,
        },
    }


def sim_order_to_ccxt_dict(sim_order: dict, exchange_name: str,
                            market_type: str) -> dict:
    """Build a CCXT-shaped order response from a simulator order JSON.

    Shape mirrors ``OrderResNormalized::from_ccxt_user_data_stream_msg``
    parsing on the C++ side. Adapter for read endpoints (open_orders,
    query_order, orders_on_symbol).
    """
    native_symbol = sim_order.get("symbol", "")
    ccxt_symbol = _symbol_mapping.native_to_ccxt(native_symbol, market_type)
    sim_status = sim_order.get("status", "")
    filled = float(sim_order.get("filled_qty", 0.0))
    qty = float(sim_order.get("qty", 0.0))
    price = float(sim_order.get("price", 0.0))
    return {
        "symbol": ccxt_symbol,
        "clientOrderId": sim_order.get("client_order_id", ""),
        "id": sim_order.get("exchange_order_id", ""),
        "status": _status_mapping.sim_to_ccxt(sim_status),
        "price": price,
        "average": price if filled > 0 else None,
        "amount": qty,
        "filled": filled,
        "timestamp": sim_order.get("ack_ts_ms", 0),
        "lastUpdateTimestamp": sim_order.get("last_update_ts_ms", 0),
        "fee": {"currency": "USDT", "cost": 0.0},
    }


def extract_exchange_order_id(sim_order: dict) -> Optional[str]:
    eid = sim_order.get("exchange_order_id", "")
    return eid if eid else None
