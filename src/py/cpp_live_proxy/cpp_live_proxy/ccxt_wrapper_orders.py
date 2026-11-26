import asyncio
import math
import time
from typing import Any, Dict, Optional

import ccxt

from cpp_live_proxy.ccxt_constants import (
    EXCHANGE_NAME_BINANCE,
    EXCHANGE_NAME_BITGET,
    EXCHANGE_NAME_BYBIT,
    EXCHANGE_NAME_GATEIO,
    EXCHANGE_NAME_IBKR,
    MARKET_TYPE_SPOT,
    MARKET_TYPE_SWAP,
    ORDER_TYPE_FILL_OR_KILL,
    ORDER_TYPE_IMMEDIATE_OR_CANCEL,
    ORDER_TYPE_LIMIT,
    ORDER_TYPE_LIMIT_MAKER,
    SPAWN_PRIVATE_STREAMS,
    STREAM_EVENT_TYPE_BALANCE_UPDATE,
    STREAM_EVENT_TYPE_ORDER_CANCEL_FAILED,
    STREAM_EVENT_TYPE_ORDER_CREATE_FAILED,
    STREAM_EVENT_TYPE_ORDER_UPDATE,
)
from cpp_live_proxy.ccxt_helpers import get_client_key as _get_client_key
from cpp_live_proxy.gcp_log import log
from cpp_live_proxy.normalization_utils import BitgetUtils
from cpp_live_proxy.type_models import Order, OrderCreationResult
from cpp_live_proxy._compat import (
    PY_PROXY_VENUE_ERROR,
    emit_trading_log,
    with_audit_context,
)


_TIME_IN_FORCE = {
    ORDER_TYPE_IMMEDIATE_OR_CANCEL: "IOC",
    ORDER_TYPE_FILL_OR_KILL: "FOK",
}


def _without_info(ccxt_order: Dict[str, Any]) -> Dict[str, Any]:
    """Drops the raw venue payload and turns non-finite floats into None: the
    response encoder rejects NaN, and the engine reads a failed create
    response as a failed order while the order may be live."""
    return {k: _finite(v) for k, v in ccxt_order.items() if k != "info"}


def _finite(value):
    if isinstance(value, float):
        return value if math.isfinite(value) else None
    if isinstance(value, dict):
        return {k: _finite(v) for k, v in value.items()}
    if isinstance(value, list):
        return [_finite(v) for v in value]
    return value


class CcxtWrapperOrdersMixin:
    def _get_target_client_from_order(self, order: Order):
        return self._round_robin_rotate_exchange_client(
            order.exchange_name, order.market_type, order.account_id, False
        )

    async def handle_orders(self, client, item, exchange_name, market_type, broadcast):
        status = self._private_stream_status.setdefault(
            _get_client_key(exchange_name, market_type, item, False), {}
        )
        if exchange_name == EXCHANGE_NAME_BITGET:
            return
        # IBKR doesn't expose ccxt-style watch_orders; its order updates
        # come via ib_insync's event subscription on the IBKRSession. No
        # work to do in this loop for IBKR.
        if exchange_name == EXCHANGE_NAME_IBKR:
            return

        while True:
            try:
                orders = await client.watch_orders()
                status["last_order_event_ms"] = int(time.time() * 1000)
                await broadcast(
                    {
                        "account_id": item,
                        "orders": orders,
                        "event_type": STREAM_EVENT_TYPE_ORDER_UPDATE,
                        "exchange_name": exchange_name,
                        "market_type": market_type,
                    }
                )
            except Exception as e:
                # Backoff on failure — a tight loop on persistent errors
                # (e.g. attribute missing, connection broken) starves the
                # event loop and degrades the entire process.
                print(f"Error in handle_orders: {e}", flush=True)
                await asyncio.sleep(1)

    async def handle_positions(
        self, client, item, exchange_name, market_type, broadcast
    ):
        if (
            exchange_name == EXCHANGE_NAME_GATEIO
            or exchange_name == EXCHANGE_NAME_BITGET
            or exchange_name == EXCHANGE_NAME_IBKR
        ):
            return

        while True:
            try:
                positions = await client.watch_positions()
                await broadcast(
                    {
                        "account_id": item,
                        "positions": positions,
                        "event_type": STREAM_EVENT_TYPE_BALANCE_UPDATE,
                        "exchange_name": exchange_name,
                        "market_type": market_type,
                    }
                )
            except Exception:
                # Same rationale as handle_orders — never tight-loop.
                await asyncio.sleep(1)

    def _restart_private_streams(self, client_key: str, client_wrapper) -> None:
        """Cancels the account's stream tasks and, given a client, starts new ones."""
        for task in self._private_stream_tasks.pop(client_key, []):
            task.cancel()
        if not SPAWN_PRIVATE_STREAMS or client_wrapper is None:
            return
        args = (
            client_wrapper.client,
            client_wrapper.account_id,
            client_wrapper.exchange_name,
            client_wrapper.market_type,
            self.broadcast,
        )
        self._private_stream_tasks[client_key] = [
            asyncio.create_task(self.handle_orders(*args)),
            asyncio.create_task(self.handle_positions(*args)),
        ]
        self._private_stream_status.setdefault(client_key, {})["started_ms"] = int(
            time.time() * 1000
        )
        log(f"[accounts] private streams started for {client_key}", {}, "INFO")

    async def restart_private_streams_for_account(
        self, exchange_name: str, market_type: str, account_id
    ) -> bool:
        """Rebuilds an account's user-data subscription from a new listen key.
        Binance can stop delivering on an open, ping-healthy socket; ccxt keeps
        that socket and its key, so restarting the tasks alone resubscribes to
        the same dead stream."""
        key = _get_client_key(exchange_name, market_type, account_id, False)
        wrappers = self.exchange_clients.get(key)
        if not wrappers or key not in self._private_stream_tasks:
            return False
        client = wrappers[0].client
        self._restart_private_streams(key, None)
        for option_key in ("future", "delivery", "spot", "margin"):
            options = client.options.get(option_key)
            if isinstance(options, dict):
                options["listenKey"] = None
                options["lastAuthenticatedTime"] = 0
        for url, ws_client in list(getattr(client, "clients", {}).items()):
            try:
                await ws_client.close()
            except Exception:
                pass
            client.clients.pop(url, None)
        self._restart_private_streams(key, wrappers[0])
        status = self._private_stream_status.setdefault(key, {})
        status["restarts"] = status.get("restarts", 0) + 1
        log(f"[accounts] private streams restarted for {key}", {}, "ERROR")
        return True

    def get_exchange_native_order_type(self, order: Order):
        exchange_name = order.exchange_name
        market_type = order.market_type

        if exchange_name == EXCHANGE_NAME_BINANCE:
            if market_type == MARKET_TYPE_SWAP:
                if order.type == ORDER_TYPE_LIMIT:
                    return ORDER_TYPE_LIMIT
                elif order.type == ORDER_TYPE_LIMIT_MAKER:
                    return ORDER_TYPE_LIMIT_MAKER
                elif order.type == ORDER_TYPE_FILL_OR_KILL:
                    return ORDER_TYPE_FILL_OR_KILL
                elif order.type == ORDER_TYPE_IMMEDIATE_OR_CANCEL:
                    return ORDER_TYPE_IMMEDIATE_OR_CANCEL
        elif exchange_name == EXCHANGE_NAME_BYBIT:
            if market_type == MARKET_TYPE_SWAP:
                if order.type == ORDER_TYPE_LIMIT:
                    return ORDER_TYPE_LIMIT
                elif order.type == ORDER_TYPE_LIMIT_MAKER:
                    return ORDER_TYPE_LIMIT_MAKER
                elif order.type == ORDER_TYPE_FILL_OR_KILL:
                    return ORDER_TYPE_FILL_OR_KILL
                elif order.type == ORDER_TYPE_IMMEDIATE_OR_CANCEL:
                    return ORDER_TYPE_IMMEDIATE_OR_CANCEL
        elif exchange_name == EXCHANGE_NAME_BITGET:
            if market_type == MARKET_TYPE_SWAP:
                if order.type == ORDER_TYPE_LIMIT:
                    return ORDER_TYPE_LIMIT
                elif order.type == ORDER_TYPE_LIMIT_MAKER:
                    return ORDER_TYPE_LIMIT_MAKER
                elif order.type == ORDER_TYPE_FILL_OR_KILL:
                    return ORDER_TYPE_FILL_OR_KILL
                elif order.type == ORDER_TYPE_IMMEDIATE_OR_CANCEL:
                    return ORDER_TYPE_IMMEDIATE_OR_CANCEL

        return ORDER_TYPE_LIMIT

    async def create_order(self, order: Order) -> OrderCreationResult:
        def _add_client_order_id(params: Dict, order: Order):
            if order.exchange_name == "bitget":
                params["clientOid"] = order.client_order_id
                params["clientOrderId"] = order.client_order_id
            else:
                params["clientOrderId"] = order.client_order_id

        def _add_position_side(params: Dict, order: Order):
            if order.exchange_name == "binance" and order.market_type == "swap":
                if order.position_side is None:
                    raise ValueError("Position side is not set for Binance swap market")
                params["positionSide"] = order.position_side

        try:
            client = self._get_target_client_from_order(order)
            params: Dict[str, Any] = {}
            _add_client_order_id(params, order)
            _add_position_side(params, order)

            symbol = order.symbol

            if order.is_alt_market_routed:
                if order.route_order_to_alt_symbol is None:
                    raise ValueError(
                        "is_alt_market_routed is true but route_order_to_alt_symbol is not set"
                    )
                symbol = order.route_order_to_alt_symbol

            if order.market_type == MARKET_TYPE_SWAP:
                if order.reduce_only:
                    params["reduceOnly"] = True

            if order.post_only:
                # ccxt-standard post-only param. Per-venue translation is
                # handled by ccxt internally (e.g. Binance USDT-M perp gets
                # timeInForce=GTX automatically).
                params["postOnly"] = True

            if order.market_type == MARKET_TYPE_SPOT:
                if order.exchange_name == "binance":
                    params["type"] = "margin"
                    if order.margin_auto_borrow and order.margin_auto_repay:
                        params["sideEffectType"] = "AUTO_BORROW_REPAY"
                    elif order.margin_auto_borrow:
                        params["sideEffectType"] = "AUTO_BORROW"
                    elif order.margin_auto_repay:
                        params["sideEffectType"] = "AUTO_REPAY"
                    params["autoRepayAtCancel"] = False

                if order.exchange_name == "bitget":
                    params["loanType"] = (
                        "autoLoanAndRepay"
                        if (order.margin_auto_borrow and order.margin_auto_repay)
                        else (
                            "autoLoan"
                            if order.margin_auto_borrow
                            else "autoRepay" if order.margin_auto_repay else "normal"
                        )
                    )
                    params["marginMode"] = "cross"

                if order.exchange_name == "bybit":
                    params["isLeverage"] = 1
                    params["category"] = "spot"
                    params["type"] = "margin"

                if order.exchange_name == "gateio":
                    params["unifiedAccount"] = True
                    params["marginMode"] = "cross"
                    params["auto_borrow"] = order.margin_auto_borrow
                    params["auto_repay"] = order.margin_auto_repay

            with with_audit_context(
                exchange=order.exchange_name,
                market_type=order.market_type,
                account_id=order.account_id,
                symbol=symbol,
            ):
                order_type = self.get_exchange_native_order_type(order)
                time_in_force = _TIME_IN_FORCE.get(order_type)
                if time_in_force is not None:
                    params["timeInForce"] = time_in_force
                    order_type = ORDER_TYPE_LIMIT
                ccxt_resp = await client.create_order(
                    symbol=symbol,
                    type=order_type,
                    side=order.side,
                    amount=order.quantity,
                    price=order.price,
                    params=params,
                )
            if not isinstance(ccxt_resp, dict):
                return OrderCreationResult(success=True)
            return OrderCreationResult(
                success=True,
                exchange_order_id=str(ccxt_resp.get("id", "")),
                order=_without_info(ccxt_resp),
            )

        except Exception as e:
            print(str(e))
            log(
                f"Error in create_order: {e}",
                {
                    "order": order.model_dump(),
                    "client_order_id": order.client_order_id,
                    "account_id": order.account_id,
                    "exchange_name": order.exchange_name,
                    "symbol": order.symbol,
                    "market_type": order.market_type,
                    "error": str(e),
                },
                "ERROR",
            )
            emit_trading_log(
                event_type=PY_PROXY_VENUE_ERROR,
                level="ERROR",
                message="create_order venue error",
                metadata={
                    "operation": "create_order",
                    "client_order_id": order.client_order_id,
                    "account_id": order.account_id,
                    "what": str(e),
                },
                symbol=order.symbol,
                strategy_id=order.strategy_id,
                exchange_name=order.exchange_name,
                market_type=order.market_type,
            )
            await self.broadcast(
                {
                    "event_type": STREAM_EVENT_TYPE_ORDER_CREATE_FAILED,
                    "order": order.model_dump(),
                    "client_order_id": order.client_order_id,
                    "account_id": order.account_id,
                    "exchange_name": order.exchange_name,
                    "symbol": order.symbol,
                    "market_type": order.market_type,
                    "error": str(e),
                }
            )
            return OrderCreationResult(success=False, error_msg=str(e))

    async def repay_loan(
        self,
        exchange_name: str,
        market_type: str,
        account_id: int,
        symbol: str,
        amount: str,
    ):
        client = self._round_robin_rotate_exchange_client(
            exchange_name, market_type, account_id, False
        )

        if exchange_name == "gateio" and market_type == "spot":
            params = {"unifiedAccount": True}
            res = await client.repay_cross_margin(symbol, amount, params=params)
        else:
            res = await client.repay_cross_margin(symbol, amount)
        print(res)
        return res

    def _get_fetch_order_params(
        self,
        exchange_name: str,
        market_type: str,
        client_order_id: str,
    ) -> Dict[str, Any]:
        if exchange_name == EXCHANGE_NAME_BITGET and market_type == MARKET_TYPE_SPOT:
            return {"clientOid": client_order_id}
        if exchange_name == EXCHANGE_NAME_BYBIT:
            return {"clientOrderId": client_order_id}
        if exchange_name == EXCHANGE_NAME_GATEIO:
            return {"clientOrderId": client_order_id}
        if exchange_name == EXCHANGE_NAME_IBKR:
            return {"clientOrderId": client_order_id}
        return {"origClientOrderId": client_order_id}

    def _get_cancel_order_identifiers(
        self,
        exchange_name: str,
        market_type: str,
        client_order_id: str,
    ) -> tuple[Optional[str], Dict[str, Any]]:
        if exchange_name == EXCHANGE_NAME_BITGET and market_type == MARKET_TYPE_SPOT:
            return (
                None,
                {
                    "clientOid": client_order_id,
                    "marginMode": "cross",
                    "type": "margin",
                    "productType": "cross",
                },
            )
        if exchange_name == EXCHANGE_NAME_BYBIT and market_type == MARKET_TYPE_SPOT:
            return (
                None,
                {
                    "orderLinkId": client_order_id,
                    "category": "spot",
                    "type": "margin",
                },
            )
        if exchange_name == EXCHANGE_NAME_BINANCE and market_type == MARKET_TYPE_SPOT:
            return (None, {"clientOrderId": client_order_id, "type": "margin"})
        if exchange_name == EXCHANGE_NAME_GATEIO and market_type == MARKET_TYPE_SPOT:
            gateio_client_order_id = f"t-{client_order_id}"
            return (
                gateio_client_order_id,
                {
                    "clientOrderId": gateio_client_order_id,
                    "unifiedAccount": True,
                },
            )
        if exchange_name == EXCHANGE_NAME_IBKR:
            return (None, {"clientOrderId": client_order_id})
        return (None, {"origClientOrderId": client_order_id})

    async def cancel_order(self, order: Order):
        try:
            client = self._get_target_client_from_order(order)

            symbol = order.symbol
            if order.is_alt_market_routed:
                if order.route_order_to_alt_symbol is None:
                    raise ValueError(
                        "is_alt_market_routed is true but route_order_to_alt_symbol is not set"
                    )
                symbol = order.route_order_to_alt_symbol

            order_id, params = self._get_cancel_order_identifiers(
                order.exchange_name,
                order.market_type,
                order.client_order_id,
            )
            with with_audit_context(
                exchange=order.exchange_name,
                market_type=order.market_type,
                account_id=order.account_id,
                symbol=symbol,
            ):
                ccxt_resp = await client.cancel_order(order_id, symbol, params=params)
            if isinstance(ccxt_resp, dict):
                return {"status": "cancelled", "order": _without_info(ccxt_resp)}
            return {"status": "cancelled"}
        except Exception as e:
            msg = str(e)
            # Known-safe outcome: the order is not at the venue (filled, never
            # rested, or already cancelled). The C++ caller drops it from state;
            # any other error leaves the order in state for reconcile/retry.
            gone = (
                isinstance(e, ccxt.OrderNotFound)
                or "-2011" in msg
                or "Unknown order" in msg
            )
            emit_trading_log(
                event_type=PY_PROXY_VENUE_ERROR,
                level="WARNING" if gone else "ERROR",
                message="cancel_order venue error",
                metadata={
                    "operation": "cancel_order",
                    "client_order_id": order.client_order_id,
                    "account_id": order.account_id,
                    "what": msg,
                    "gone": gone,
                },
                symbol=order.symbol,
                strategy_id=order.strategy_id,
                exchange_name=order.exchange_name,
                market_type=order.market_type,
            )
            if not gone:
                await self.broadcast(
                    {
                        "event_type": STREAM_EVENT_TYPE_ORDER_CANCEL_FAILED,
                        "order": order.model_dump(),
                        "client_order_id": order.client_order_id,
                        "account_id": order.account_id,
                        "error": msg,
                    }
                )
            return {"status": "gone" if gone else "error", "error_msg": msg}

    async def cancel_order_v2(
        self,
        client_order_id: str,
        exchange_name: str,
        market_type: str,
        symbol: str,
        account_id: int,
    ):
        try:
            client = self._round_robin_rotate_exchange_client(
                exchange_name, market_type, account_id, False
            )
            order_id, params = self._get_cancel_order_identifiers(
                exchange_name,
                market_type,
                client_order_id,
            )
            with with_audit_context(
                exchange=exchange_name,
                market_type=market_type,
                account_id=account_id,
                symbol=symbol,
            ):
                await client.cancel_order(order_id, symbol, params=params)
        except Exception as e:
            emit_trading_log(
                event_type=PY_PROXY_VENUE_ERROR,
                level="ERROR",
                message="cancel_order_v2 venue error",
                metadata={
                    "operation": "cancel_order_v2",
                    "client_order_id": client_order_id,
                    "account_id": account_id,
                    "what": str(e),
                },
                symbol=symbol,
                exchange_name=exchange_name,
                market_type=market_type,
            )
            raise e

    async def get_orders_on_symbol(
        self, exchange_name: str, market_type: str, account_id: int, symbol: str
    ):
        client = self._round_robin_rotate_exchange_client(
            exchange_name, market_type, account_id, False
        )

        if exchange_name == "bitget" and market_type == "spot":
            params: Dict[str, Any] = {
                "symbol": symbol.replace("/", ""),
            }
            params["startTime"] = int(time.time() * 1000 - 86_400_000 * 2)
            params["limit"] = 500
            res_history_orders = (
                await client.private_margin_get_v2_margin_crossed_history_orders(params)
            )

            res_open_orders = (
                await client.private_margin_get_v2_margin_crossed_open_orders(params)
            )

            parsed_history_orders = BitgetUtils.parse_raw_order_res(
                res_history_orders, market_type
            )

            parsed_open_orders = BitgetUtils.parse_raw_order_res(
                res_open_orders, market_type
            )

            res = parsed_history_orders + parsed_open_orders
            return res

        elif exchange_name == "bybit" and market_type == "spot":
            params = {
                "category": "spot",
                "type": "margin",
            }
            res_open = await client.fetch_open_orders(symbol=symbol, params=params)
            res_closed = await client.fetch_canceled_and_closed_orders(
                symbol=symbol, params=params
            )

            res = res_open + res_closed
            return res

        elif exchange_name == "gateio" and market_type == "spot":
            params = {
                "unifiedAccount": True,
            }
            res_closed = await client.fetch_closed_orders(symbol=symbol, params=params)
            res_open = await client.fetch_open_orders(symbol=symbol, params=params)
            res = res_closed + res_open
            return res

        res = await client.fetch_orders(symbol=symbol, limit=1000)
        return res

    async def get_all_open_orders(
        self, symbol: str, exchange_name: str, market_type: str, account_id: int
    ):
        client = self._round_robin_rotate_exchange_client(
            exchange_name, market_type, account_id, False
        )

        if exchange_name == "bitget" and market_type == "spot":
            params = {
                "symbol": symbol.replace("/", ""),
                "startTime": int(time.time() * 1000 - 86_400_000 * 2),
                "limit": 500,
            }
            res = await client.private_margin_get_v2_margin_crossed_open_orders(params)
            parsed_open_orders = BitgetUtils.parse_raw_order_res(res, market_type)
            return parsed_open_orders

        elif exchange_name == "bybit" and market_type == "spot":
            params = {
                "category": "spot",
                "type": "margin",
            }
            res = await client.fetch_open_orders(symbol=symbol, params=params)
            return res

        elif exchange_name == "gateio" and market_type == "spot":
            params = {
                "unifiedAccount": True,
            }
            res = await client.fetch_open_orders(symbol=symbol, params=params)
            return res
        elif exchange_name == "binance" and market_type == "spot":
            params = {
                "type": "margin",
            }
            res = await client.fetch_open_orders(symbol=symbol, params=params)
            return res

        res = await client.fetch_open_orders(symbol=symbol)
        return res

    async def query_order(
        self,
        symbol: str,
        exchange_name: str,
        market_type: str,
        account_id: int,
        client_order_id: str,
    ):
        client = self._round_robin_rotate_exchange_client(
            exchange_name, market_type, account_id, False
        )

        if exchange_name == "bitget" and market_type == "spot":
            params: Dict[str, Any] = {
                "symbol": symbol.replace("/", ""),
                "startTime": int(time.time() * 1000 - 86_400_000 * 2),
                "limit": 20,
                "clientOid": client_order_id,
            }
            res_history_orders = (
                await client.private_margin_get_v2_margin_crossed_history_orders(params)
            )
            res_open_orders = (
                await client.private_margin_get_v2_margin_crossed_open_orders(params)
            )

            parsed_history_orders = BitgetUtils.parse_raw_order_res(
                res_history_orders, market_type
            )

            parsed_open_orders = BitgetUtils.parse_raw_order_res(
                res_open_orders, market_type
            )
            res = parsed_history_orders + parsed_open_orders

            if len(res) > 0:
                return res[0]
            else:
                return None

        elif exchange_name == "bybit":
            orders = await self.get_orders_on_symbol(
                exchange_name, market_type, account_id, symbol
            )
            for order in orders:
                if order["clientOrderId"] == client_order_id:
                    return order
            return None

        elif exchange_name == "gateio" and market_type == "spot":
            params: Dict[str, Any] = {
                "unifiedAccount": True,
            }
            params.update(
                self._get_fetch_order_params(
                    exchange_name,
                    market_type,
                    f"t-{client_order_id}",
                )
            )
            res = await client.fetch_order(
                f"t-{client_order_id}", symbol, params=params
            )
            return res

        params = self._get_fetch_order_params(
            exchange_name,
            market_type,
            client_order_id,
        )
        res = await client.fetch_order(
            client_order_id,
            symbol,
            params=params,
        )
        return res
