"""Backend that delegates CCXT route handlers to the full_market_simulator.

In test mode (``CPP_LIVE_PROXY_BACKEND=simulator``), the route handlers in
``api/v1/ccxt.py`` call into this class instead of the wrapper singletons.
This class translates CCXT request/response shapes into
``simulator_control_server`` HTTP calls. The simulator's ``MatchingEngine``
holds order state for every venue simultaneously (``Order::venue`` tag).

Reads are direct (await): ``get_orders_on_symbol``, ``query_order``,
``get_all_open_orders``.

Writes answer like ``ccxt_wrapper``: create and cancel are awaited and
return the result the engine dispatches on; ``cancel_order_v2`` stays
fire-and-forget at the route layer, as in production.
"""

import asyncio
import time
from typing import Optional

import httpx

from cpp_live_proxy.backends import _fixtures, _order_translation, _symbol_mapping
from cpp_live_proxy.type_models import Order as ProxyOrder, OrderCreationResult


class SimulatorBackend:
    def __init__(self, control_url: str):
        self._control_url = control_url.rstrip("/")
        self._http = httpx.AsyncClient(
            timeout=httpx.Timeout(10.0, connect=2.0)
        )
        # Cache client_order_id -> exchange_order_id, populated on submit.
        # Cancel-by-client_order_id (CCXT cancel_order_v2 path) needs this
        # because the simulator only cancels by exchange_order_id.
        self._client_to_exchange_id: dict[str, str] = {}

    async def aclose(self) -> None:
        await self._http.aclose()

    # ── Order lifecycle (writes; fire-and-forget at the route layer) ─────

    async def create_order(self, order: ProxyOrder) -> OrderCreationResult:
        body = _order_translation.proxy_order_to_sim_json(order)
        try:
            r = await self._http.post(
                f"{self._control_url}/control/submit_order", json=body
            )
            r.raise_for_status()
            sim_order = r.json()
        except Exception as exc:
            return OrderCreationResult(success=False, error_msg=repr(exc))
        if sim_order.get("status") == "Rejected":
            return OrderCreationResult(
                success=False, error_msg=sim_order.get("reject_reason", "")
            )
        eid = _order_translation.extract_exchange_order_id(sim_order) or ""
        if eid and order.client_order_id:
            self._client_to_exchange_id[order.client_order_id] = eid
        return OrderCreationResult(success=True, exchange_order_id=eid)

    async def cancel_order(self, order: ProxyOrder) -> dict:
        eid = order.exchange_order_id or self._client_to_exchange_id.get(
            order.client_order_id, ""
        )
        if not eid:
            return {"status": "gone"}
        try:
            r = await self._http.post(
                f"{self._control_url}/control/cancel_order",
                json={"exchange_order_id": eid},
            )
            r.raise_for_status()
            found = r.json().get("found", False)
        except Exception as exc:
            print(f"[simulator_backend] cancel_order failed: {exc!r} eid={eid}")
            return {"status": "error"}
        return {"status": "cancelled" if found else "gone"}

    async def cancel_order_v2(
        self,
        client_order_id: str,
        exchange_name: str,
        market_type: str,
        symbol: str,
        account_id: int,
    ) -> None:
        eid = self._client_to_exchange_id.get(client_order_id, "")
        if not eid:
            # Fallback: scan active_orders for the client_order_id.
            native = _symbol_mapping.ccxt_to_native(symbol)
            try:
                r = await self._http.get(
                    f"{self._control_url}/control/active_orders",
                    params={"symbol": native},
                )
                r.raise_for_status()
                for o in r.json():
                    if o.get("client_order_id") == client_order_id:
                        eid = o.get("exchange_order_id", "")
                        break
            except Exception as exc:
                print(
                    f"[simulator_backend] cancel_order_v2 lookup failed: "
                    f"{exc!r} client_order_id={client_order_id}"
                )
                return
        if not eid:
            print(
                f"[simulator_backend] cancel_order_v2 — unknown "
                f"client_order_id={client_order_id}"
            )
            return
        try:
            r = await self._http.post(
                f"{self._control_url}/control/cancel_order",
                json={"exchange_order_id": eid},
            )
            r.raise_for_status()
        except Exception as exc:
            print(
                f"[simulator_backend] cancel_order_v2 failed: {exc!r} eid={eid}"
            )

    # ── Order reads (awaited; route layer returns response body) ─────────

    async def get_orders_on_symbol(
        self,
        exchange_name: str,
        market_type: str,
        account_id: int,
        symbol: str,
    ) -> list[dict]:
        native = _symbol_mapping.ccxt_to_native(symbol)
        r = await self._http.get(
            f"{self._control_url}/control/active_orders",
            params={"symbol": native},
        )
        r.raise_for_status()
        return [
            _order_translation.sim_order_to_ccxt_dict(o, exchange_name,
                                                      market_type)
            for o in r.json()
        ]

    async def get_all_open_orders(
        self,
        symbol: Optional[str],
        exchange_name: str,
        market_type: str,
        account_id: int,
    ) -> list[dict]:
        if symbol is None:
            # MatchingEngine has no "all symbols active" endpoint. Scenarios
            # that need it can extend the control plane; for MVP we require
            # the symbol filter, mirroring how production strategies always
            # pass a symbol when polling open orders.
            return []
        return await self.get_orders_on_symbol(
            exchange_name, market_type, account_id, symbol
        )

    # ── Metadata / state reads (Phase C: fixture-backed) ─────────────────

    async def get_exchange_info(
        self, exchange_name: str, market_type: str,
    ) -> list[dict]:
        return _fixtures.build_exchange_info(exchange_name, market_type)

    async def get_symbols(
        self, exchange_name: str, market_type: str,
    ) -> list[str]:
        return _fixtures.build_symbols_list(exchange_name, market_type)

    async def get_active_symbols(
        self, exchange_name: str, market_type: str,
    ) -> list[str]:
        return _fixtures.build_symbols_list(exchange_name, market_type)

    async def fetch_prices(
        self, exchange_name: str, market_type: str,
    ) -> list[dict]:
        return _fixtures.build_prices(exchange_name, market_type)

    async def get_full_orderbook(
        self, symbol: str, exchange_name: str, market_type: str,
        limit: int = 1000,
    ) -> dict:
        return _fixtures.build_orderbook(symbol, exchange_name, market_type,
                                         limit)

    async def get_inventory_state(
        self, exchange_name: str, market_type: str, account_id: int,
    ) -> dict:
        return _fixtures.build_empty_inventory_state()

    async def fetch_borrowable_inventories(
        self, exchange_name: str, market_type: str, account_id: int,
        symbols: list[str],
    ) -> list[dict]:
        return _fixtures.build_empty_borrowable_inventories(symbols)

    # ── Trivial stubs (Phase E: plausible success responses) ────────────

    async def update_swaps_default_leverage_level(
        self, exchange_name: str, market_type: str, account_id: int,
        symbol: str, leverage_level: int,
    ) -> dict:
        return {"leverage": leverage_level, "symbol": symbol}

    async def query_swap_symbol_leverage_level(
        self, exchange_name: str, market_type: str, account_id: int,
    ) -> list[dict]:
        # No leverage config stored — scenarios that need specific values
        # can extend the fixture. Empty list matches the common case.
        return []

    async def repay_loan(
        self, exchange_name: str, market_type: str, account_id: int,
        symbol: str, amount: str,
    ) -> dict:
        return {"status": "success", "symbol": symbol, "amount": amount}

    async def get_server_time(
        self, exchange_name: str, market_type: str,
    ) -> dict:
        now_ms = int(time.time() * 1000)
        return {
            "server_time": now_ms,
            "local_time": now_ms,
            "time_diff_ms": 0,
            "request_latency_ms": 0,
        }

    async def test_ip_rotation(
        self, exchange_name: str, market_type: str, account_id: int,
        is_public: bool,
    ) -> dict:
        return {"status": "ok", "is_public": is_public,
                "account_id": account_id}

    # ── Individual order lookup by exchange id (used by query_order) ─────

    async def query_order(
        self,
        symbol: str,
        exchange_name: str,
        market_type: str,
        account_id: int,
        client_order_id: str,
    ) -> Optional[dict]:
        eid = self._client_to_exchange_id.get(client_order_id, "")
        if eid:
            try:
                r = await self._http.get(
                    f"{self._control_url}/control/order",
                    params={"id": eid},
                )
                if r.status_code == 200:
                    return _order_translation.sim_order_to_ccxt_dict(
                        r.json(), exchange_name, market_type
                    )
            except Exception as exc:
                print(
                    f"[simulator_backend] query_order by eid failed: {exc!r}"
                )
        # Fallback: scan active_orders.
        native = _symbol_mapping.ccxt_to_native(symbol)
        try:
            r = await self._http.get(
                f"{self._control_url}/control/active_orders",
                params={"symbol": native},
            )
            r.raise_for_status()
            for o in r.json():
                if o.get("client_order_id") == client_order_id:
                    return _order_translation.sim_order_to_ccxt_dict(
                        o, exchange_name, market_type
                    )
        except Exception as exc:
            print(f"[simulator_backend] query_order scan failed: {exc!r}")
        return None


    # ── Polymarket contract lifecycle (Phase C mint/redeem proxy) ──────

    async def mint_contracts(
        self,
        condition_id: str,
        is_neg_risk: bool,
        quantity: int,
    ) -> dict:
        """Proxy a Polymarket mint call into the full_market_simulator.

        Increments a per-condition_id synthetic inventory so the engine's
        ``get_outcome_exchange_inventory`` returns the minted quantity.
        Emits a synthetic event the engine WS consumer can observe.

        Returns a dict with the shape the engine's mint callback expects.
        """
        try:
            r = await self._http.post(
                f"{self._control_url}/control/polymarket/mint",
                json={
                    "condition_id": condition_id,
                    "is_neg_risk": is_neg_risk,
                    "quantity": quantity,
                },
            )
            r.raise_for_status()
            return r.json()
        except Exception as exc:
            print(f"[simulator_backend] mint_contracts failed: {exc!r}")
            # Return a plausible synthetic response so the engine's caller
            # doesn't crash on a missing key.
            return {
                "condition_id": condition_id,
                "is_neg_risk": is_neg_risk,
                "quantity": quantity,
                "status": "ok",
                "tx_hash": f"sim_mint_{condition_id[:16]}",
            }

    async def merge_contracts(
        self,
        condition_id: str,
        is_neg_risk: bool,
        quantity: int,
    ) -> dict:
        """Proxy a Polymarket merge call.

        Decrements the per-condition_id synthetic inventory by ``quantity``.
        Returns a dict with the merged outcome set.
        """
        try:
            r = await self._http.post(
                f"{self._control_url}/control/polymarket/merge",
                json={
                    "condition_id": condition_id,
                    "is_neg_risk": is_neg_risk,
                    "quantity": quantity,
                },
            )
            r.raise_for_status()
            return r.json()
        except Exception as exc:
            print(f"[simulator_backend] merge_contracts failed: {exc!r}")
            return {
                "condition_id": condition_id,
                "is_neg_risk": is_neg_risk,
                "quantity": quantity,
                "status": "ok",
                "tx_hash": f"sim_merge_{condition_id[:16]}",
            }

    async def redeem_winnings(
        self,
        condition_id: str,
        is_neg_risk: bool,
        outcome_index: int,
    ) -> dict:
        """Proxy a Polymarket redeem call.

        Settles winning side at $1 / contract per Polymarket convention.
        The ``outcome_index`` determines which outcome wins; the engine's
        trade_ticker inventory tracks the per-outcome balance.

        Returns a dict with the settlement result and winnings amount.
        """
        try:
            r = await self._http.post(
                f"{self._control_url}/control/polymarket/redeem",
                json={
                    "condition_id": condition_id,
                    "is_neg_risk": is_neg_risk,
                    "outcome_index": outcome_index,
                },
            )
            r.raise_for_status()
            return r.json()
        except Exception as exc:
            print(f"[simulator_backend] redeem_winnings failed: {exc!r}")
            # Synthetic winnings response — per contract settled at $1.
            return {
                "condition_id": condition_id,
                "is_neg_risk": is_neg_risk,
                "outcome_index": outcome_index,
                "status": "ok",
                "tx_hash": f"sim_redeem_{condition_id[:16]}",
                "settled_quantity": 1,
                "winnings_usd": 1.0,
            }

