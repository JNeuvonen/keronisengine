"""Background task that turns simulator ``EngineEvent``s into CCXT user-data
WS messages.

Polls ``GET /control/events?since=<cursor>`` on the simulator, filters for
order-lifecycle events, looks up the current ``Order`` state by
``exchange_order_id``, translates to the CCXT ``order_update`` shape the
engine expects on ``/api/v1/user-data/stream``, and broadcasts via
``CcxtWrapper.broadcast``.

The engine's user-data WS parser
(``OrderResNormalized::from_ccxt_user_data_stream_msg``) drives order
state transitions from these messages — the streamer is the other half
of the impersonator after Phase B's order lifecycle routes.
"""

from __future__ import annotations

import asyncio
from typing import Awaitable, Callable, Optional

import httpx

from cpp_live_proxy.backends import _order_translation


# Order-lifecycle event kinds. BookUpdate is excluded — it doesn't map to
# a user-data message.
_LIFECYCLE_KINDS = frozenset({
    "OrderAck", "OrderFill", "OrderCancel", "OrderReject",
})


class SimulatorEventStreamer:
    """Poll → translate → broadcast loop. One instance per proxy process."""

    def __init__(
        self,
        control_url: str,
        broadcast: Callable[[dict], Awaitable[None]],
        *,
        poll_interval_s: float = 0.05,
    ):
        self._control_url = control_url.rstrip("/")
        self._broadcast = broadcast
        self._poll_interval_s = poll_interval_s
        self._cursor = 0
        self._running = False
        self._http: Optional[httpx.AsyncClient] = None

    async def run(self) -> None:
        """Main loop. Cancel the task to stop."""
        self._running = True
        self._http = httpx.AsyncClient(
            timeout=httpx.Timeout(5.0, connect=2.0)
        )
        try:
            while self._running:
                try:
                    await self._poll_once()
                except asyncio.CancelledError:
                    raise
                except Exception as exc:
                    # Never let a poll failure kill the loop — transient
                    # network/simulator-restart should be recoverable.
                    print(f"[simulator_event_streamer] poll failed: {exc!r}")
                await asyncio.sleep(self._poll_interval_s)
        finally:
            if self._http is not None:
                await self._http.aclose()
                self._http = None

    async def aclose(self) -> None:
        self._running = False

    # ── Internals ────────────────────────────────────────────────────────

    async def _poll_once(self) -> None:
        assert self._http is not None
        r = await self._http.get(
            f"{self._control_url}/control/events",
            params={"since": self._cursor},
        )
        r.raise_for_status()
        body = r.json()
        events = body.get("events", [])
        next_cursor = body.get("next_cursor", self._cursor)

        for event in events:
            if event.get("kind") not in _LIFECYCLE_KINDS:
                continue
            await self._emit_for_event(event)

        self._cursor = next_cursor

    async def _emit_for_event(self, event: dict) -> None:
        payload = event.get("payload") or {}
        eid = payload.get("exchange_order_id")
        if not eid:
            return

        assert self._http is not None
        try:
            order_r = await self._http.get(
                f"{self._control_url}/control/order",
                params={"id": eid},
            )
        except Exception as exc:
            print(f"[simulator_event_streamer] get order {eid} failed: {exc!r}")
            return
        if order_r.status_code != 200:
            # Order may have been reset/dropped. Skip silently.
            return
        sim_order = order_r.json()

        # Derive CCXT context from the order's venue + venue_specific tag
        # that our SimulatorBackend set at submit time.
        exchange_name = sim_order.get("venue", "binance") or "binance"
        venue_specific = sim_order.get("venue_specific") or {}
        market_type = venue_specific.get("market_type", "swap") or "swap"

        ccxt_order = _order_translation.sim_order_to_ccxt_dict(
            sim_order, exchange_name, market_type
        )

        msg = {
            "event_type": "order_update",
            "orders": [ccxt_order],
        }
        try:
            await self._broadcast(msg)
        except Exception as exc:
            print(f"[simulator_event_streamer] broadcast failed: {exc!r}")
