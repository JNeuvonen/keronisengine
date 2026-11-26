import asyncio
import json
import logging
import time

import websockets

from cpp_live_proxy.ccxt_wrapper import CcxtWrapper, STREAM_EVENT_TYPE_ORDER_UPDATE
from cpp_live_proxy.gcp_log import log

STREAM_EVENT_TYPE_FILL_STATUS_UPDATE = "fill_status_update"

logger = logging.getLogger(__name__)

POLYMARKET_WS_USER_URL = "wss://ws-subscriptions-clob.polymarket.com/ws/user"
PING_INTERVAL_S = 5
RECONNECT_BASE_S = 1.0
RECONNECT_MAX_S = 60.0

_POLYMARKET_STATUS_MAP = {
    "LIVE": "open",
    "MATCHED": "closed",
    "CANCELED": "canceled",
    "DELAYED": "open",
    "UNMATCHED": "open",
}

_TRADE_STATUSES = {"MATCHED", "MINED", "CONFIRMED", "RETRYING", "FAILED"}
_TRADE_ACTIONABLE_STATUSES = {"MATCHED", "CONFIRMED", "FAILED"}


class PolymarketStreamer:
    def __init__(
        self,
        api_key: str,
        secret: str,
        passphrase: str,
        account_id: int = 0,
    ):
        self._api_key = api_key
        self._secret = secret
        self._passphrase = passphrase
        self._account_id = account_id

        self._subscribed_condition_ids: set[str] = set()
        self._lock = asyncio.Lock()
        self._ws = None
        self._running = False

    async def start(self):
        self._running = True
        print("[polymarket_ws] waiting for first subscription...")
        while self._running and not self._subscribed_condition_ids:
            await asyncio.sleep(1.0)
        print(
            f"[polymarket_ws] got {len(self._subscribed_condition_ids)} condition_ids, connecting"
        )
        backoff = RECONNECT_BASE_S
        while self._running:
            try:
                await self._connect_and_run()
                backoff = RECONNECT_BASE_S
            except asyncio.CancelledError:
                print("[polymarket_ws] cancelled")
                break
            except Exception as e:
                print(f"[polymarket_ws] error: {e}, reconnecting in {backoff:.1f}s")
                await asyncio.sleep(backoff)
                backoff = min(backoff * 2, RECONNECT_MAX_S)

    def stop(self):
        self._running = False

    async def _connect_and_run(self):
        print(f"[polymarket_ws] connecting to {POLYMARKET_WS_USER_URL}")
        async with websockets.connect(POLYMARKET_WS_USER_URL) as ws:
            self._ws = ws
            print("[polymarket_ws] connected")

            async with self._lock:
                if self._subscribed_condition_ids:
                    await self._send_initial_subscribe(ws)

            ping_task = asyncio.create_task(self._ping_loop(ws))
            try:
                async for raw_msg in ws:
                    if isinstance(raw_msg, bytes):
                        raw_msg = raw_msg.decode("utf-8", errors="replace")
                    if raw_msg == "PONG":
                        continue
                    try:
                        msgs = json.loads(raw_msg)
                    except json.JSONDecodeError:
                        continue
                    if isinstance(msgs, list):
                        for msg in msgs:
                            await self._handle_message(msg)
                    elif isinstance(msgs, dict):
                        await self._handle_message(msgs)
            finally:
                print("[polymarket_ws] disconnected")
                ping_task.cancel()
                self._ws = None

    async def _send_initial_subscribe(self, ws):
        msg = {
            "auth": {
                "apiKey": self._api_key,
                "secret": self._secret,
                "passphrase": self._passphrase,
            },
            "markets": list(self._subscribed_condition_ids),
            "type": "user",
        }
        await ws.send(json.dumps(msg))
        logger.info(
            f"Polymarket ws: subscribed to {len(self._subscribed_condition_ids)} markets"
        )

    async def _ping_loop(self, ws):
        try:
            while True:
                await asyncio.sleep(PING_INTERVAL_S)
                try:
                    await ws.send("PING")
                except Exception:
                    return
        except asyncio.CancelledError:
            pass

    # ── Subscription management ──
    async def set_subscriptions(self, condition_ids: set[str]):
        async with self._lock:
            to_add = condition_ids - self._subscribed_condition_ids

            if to_add and self._ws:
                await self._ws.send(
                    json.dumps(
                        {
                            "markets": list(to_add),
                            "operation": "subscribe",
                        }
                    )
                )

            self._subscribed_condition_ids = set(condition_ids)

            if to_add:
                logger.info(
                    f"Polymarket ws: subscribed +{len(to_add)}, "
                    f"{len(self._subscribed_condition_ids)} total"
                )

    # ── Message handling & normalization ──

    async def _handle_message(self, msg: dict):
        event_type = msg.get("event_type", "")
        if event_type == "trade" or msg.get("status", "").upper() in _TRADE_STATUSES:
            await self._handle_trade_event(msg)
        elif event_type == "order" or "original_size" in msg:
            await self._handle_order_event(msg)

    async def _handle_order_event(self, msg: dict):
        ws_type = msg.get("type", "").upper()
        status_raw = msg.get("status", ws_type).upper()

        if ws_type == "CANCELLATION":
            ccxt_status = "canceled"
        elif ws_type == "PLACEMENT":
            ccxt_status = "open"
        else:
            ccxt_status = _POLYMARKET_STATUS_MAP.get(status_raw, "open")

        timestamp_str = msg.get("timestamp")
        timestamp_ms = _parse_timestamp_ms(timestamp_str)

        original_size = _safe_float(msg.get("original_size"))
        size_matched = _safe_float(msg.get("size_matched"))

        normalized = {
            "symbol": msg.get("market", ""),
            "id": msg.get("id", ""),
            "clientOrderId": "",
            "status": ccxt_status,
            "price": _safe_float(msg.get("price")),
            "average": None,
            "amount": original_size,
            "filled": size_matched,
            "timestamp": timestamp_ms,
            "lastTradeTimestamp": timestamp_ms if ws_type == "UPDATE" else None,
            "lastUpdateTimestamp": timestamp_ms,
            "fee": None,
        }

        payload = {
            "account_id": self._account_id,
            "orders": [normalized],
            "event_type": STREAM_EVENT_TYPE_ORDER_UPDATE,
            "exchange_name": "polymarket",
            "market_type": "spot",
        }
        logger.info(f"Polymarket ws broadcast (order): {json.dumps(payload)}")
        await CcxtWrapper().broadcast(payload)

    async def _handle_trade_event(self, msg: dict):
        status_raw = msg.get("status", "").upper()
        if status_raw not in _TRADE_ACTIONABLE_STATUSES:
            return

        market = msg.get("market", "")

        if status_raw in ("CONFIRMED", "FAILED"):
            taker_id = msg.get("taker_order_id", "")
            severity = "ERROR" if status_raw == "FAILED" else "INFO"
            log(
                f"Polymarket {status_raw} trade: market={market}",
                {"taker_order_id": taker_id, "full_msg": msg},
                severity=severity,
            )
            payload = {
                "event_type": STREAM_EVENT_TYPE_FILL_STATUS_UPDATE,
                "taker_order_id": taker_id,
                "status": status_raw.lower(),
                "market": market,
                "account_id": self._account_id,
            }
            logger.info(
                f"Polymarket ws broadcast (fill_status/{status_raw}): "
                f"{json.dumps(payload)}"
            )
            await CcxtWrapper().broadcast(payload)
            return

        timestamp_str = (
            msg.get("timestamp") or msg.get("match_time") or msg.get("matchtime")
        )
        timestamp_ms = _parse_timestamp_ms(timestamp_str)

        size = _safe_float(msg.get("size"))
        price = _safe_float(msg.get("price"))

        taker_order_id = msg.get("taker_order_id", "")

        maker_orders = msg.get("maker_orders", [])
        for maker in maker_orders:
            maker_id = maker.get("order_id", "")
            matched_amount = _safe_float(maker.get("matched_amount"))
            maker_price = _safe_float(maker.get("price")) or price

            normalized = {
                "symbol": market,
                "id": maker_id,
                "clientOrderId": "",
                "status": "closed",
                "price": maker_price,
                "average": maker_price,
                "amount": matched_amount,
                "filled": matched_amount,
                "timestamp": timestamp_ms,
                "lastTradeTimestamp": timestamp_ms,
                "lastUpdateTimestamp": timestamp_ms,
                "fee": None,
                "taker_order_id": taker_order_id,
            }

            payload = {
                "account_id": self._account_id,
                "orders": [normalized],
                "event_type": STREAM_EVENT_TYPE_ORDER_UPDATE,
                "exchange_name": "polymarket",
                "market_type": "spot",
            }
            logger.info(f"Polymarket ws broadcast (trade/maker): {json.dumps(payload)}")
            await CcxtWrapper().broadcast(payload)

        if taker_order_id and msg.get("trade_owner") == msg.get("owner"):
            normalized = {
                "symbol": market,
                "id": taker_order_id,
                "clientOrderId": "",
                "status": "closed",
                "price": price,
                "average": price,
                "amount": size,
                "filled": size,
                "timestamp": timestamp_ms,
                "lastTradeTimestamp": timestamp_ms,
                "lastUpdateTimestamp": timestamp_ms,
                "fee": None,
                "taker_order_id": taker_order_id,
            }

            payload = {
                "account_id": self._account_id,
                "orders": [normalized],
                "event_type": STREAM_EVENT_TYPE_ORDER_UPDATE,
                "exchange_name": "polymarket",
                "market_type": "spot",
            }
            logger.info(f"Polymarket ws broadcast (trade/taker): {json.dumps(payload)}")
            await CcxtWrapper().broadcast(payload)


def _parse_timestamp_ms(ts) -> int:
    if ts is None:
        return int(time.time() * 1000)
    if isinstance(ts, (int, float)):
        val = int(ts)
        return val * 1000 if val < 1e12 else val
    if isinstance(ts, str):
        try:
            val = int(ts)
            return val * 1000 if val < 1e12 else val
        except (ValueError, TypeError):
            return int(time.time() * 1000)
    return int(time.time() * 1000)


def _safe_float(val, default: float = 0.0) -> float:
    if val is None:
        return default
    try:
        return float(val)
    except (ValueError, TypeError):
        return default
