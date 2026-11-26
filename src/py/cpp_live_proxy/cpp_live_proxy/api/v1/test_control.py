"""Test-only control endpoints exposed by cpp_live_proxy.

These routes exist solely to give infra_e2e_testing scenarios precise
levers to simulate failure modes that production would normally hit
during incidents. They're gated by ``CPP_LIVE_PROXY_TEST_MODE=1`` —
the router is not registered when the proxy boots in production mode.

Endpoints are namespaced under ``/api/v1/test`` to make them obviously
non-prod in any log trace.
"""

from __future__ import annotations

from fastapi import APIRouter

from cpp_live_proxy.ccxt_wrapper import CcxtWrapper


router = APIRouter()


@router.post("/disconnect_user_data_ws")
async def disconnect_user_data_ws() -> dict:
    """Drop every active connection on the user-data WebSocket stream.

    Pushes a close sentinel onto each listener queue; the user-data
    handler at api/v1/user_data.py recognizes the sentinel, breaks the
    receive loop, and the finally block unregisters + closes the
    socket. Idempotent — safe to call when no clients are connected.

    Used by ``test_ws_drop_rest_resync`` to simulate a WS gap and
    verify the engine's REST-fallback path (verify_order_states)
    catches missed fills.

    Returns ``{"disconnected": <int>}`` — count of WS connections that
    received the close signal.
    """
    wrapper = CcxtWrapper()
    count = await wrapper.disconnect_all_user_data_listeners()
    return {"disconnected": count}
