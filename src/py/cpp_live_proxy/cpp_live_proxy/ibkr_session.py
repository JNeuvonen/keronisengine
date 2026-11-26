"""IBKR session — official ibapi backend.

ibapi is threading-based, not asyncio-based. That's the point: uvicorn owns
the event loop; ibapi owns its own dispatcher thread. They share state via
threading.Lock-protected caches and `asyncio.wrap_future` /
`asyncio.run_coroutine_threadsafe` for the cross-thread bridges.

Public surface (unchanged from the ib_insync version):
- IBKRSession.create_and_connect / create_and_connect_async
- IBKRSession.{positions, account_summary, open_trades, place_order,
                cancel_order_by_client_id, query_order_by_client_id}
- IBKRAccountClient: per-account ccxt-shape facade

Defense in depth (unchanged):
1. Boot bidirectional invariant: configured account_id set ==
   IBKR_ALLOWED_ACCOUNT_IDS.
2. Boot: every mapped IBKR string must be in gateway.managedAccounts();
   leaked accounts log WARNING.
3. Runtime: IBKRAccountClient construction asserts internal_id in
   allowlist; every IBKRClient method asserts account string in
   allowed set before any send.
"""

from __future__ import annotations

import asyncio
import concurrent.futures
import threading
import time
from typing import Any, Callable, Dict, Iterable, List, Optional

from ibapi.client import EClient
from ibapi.contract import Contract
from ibapi.order import Order
from ibapi.wrapper import EWrapper

from cpp_live_proxy.gcp_log import log


CONNECT_READY_TIMEOUT_S = 10
ORDER_ACK_TIMEOUT_S = 10
# Per-snapshot timeouts for the boot synchronization. The previous
# implementation slept a fixed 2s after firing reqPositions /
# reqAccountUpdates / reqOpenOrders. That was a race: a slow gateway
# left the cache empty when the first request arrived. We now wait on
# the actual ibapi snapshot-done callbacks (positionEnd,
# accountDownloadEnd, openOrderEnd) and drop the sleep entirely.
SNAPSHOT_TIMEOUT_S = 10

# Subscription-stealing watchdog. IBKR's reqAccountUpdates is
# single-subscriber-per-account: any other ibapi client (TWS opening,
# another debugging session, the live smoke test) silently steals our
# subscription. Our cache then freezes — and worse, the gateway sometimes
# pushes a final clearing update with zero values during the transfer.
# We periodically re-subscribe; the call is idempotent (re-claims the
# subscription if stolen, no-op if we already own it). reqAllOpenOrders
# is cheap and catches mid-session orders placed by external clients.
WATCHDOG_INTERVAL_S = 60


def _ibkr_status_to_ccxt(status: str) -> str:
    """Translate ibapi order status strings to ccxt vocabulary so the
    payload matches what the C++ parser
    (`OrderResNormalized::from_ccxt_user_data_stream_msg` +
    `normalize_status`) expects.

    ibapi vocabulary (from EWrapper docs): PendingSubmit, PendingCancel,
    PreSubmitted, Submitted, ApiCancelled, Cancelled, Filled, Inactive.
    ccxt vocabulary: open, closed, canceled, expired, rejected.
    """
    if status in ("Submitted", "PreSubmitted", "PendingSubmit", "PendingCancel"):
        return "open"
    if status == "Filled":
        return "closed"
    if status in ("Cancelled", "ApiCancelled"):
        return "canceled"
    if status == "Inactive":
        # `Inactive` means the order is dead but neither filled nor cancelled —
        # rejected by gateway / margin check. Map to canceled so the engine
        # frees the slot. (No `rejected` mapping in the parser would buy us
        # different behavior — `normalize_status` collapses both to NOT_WORKING.)
        return "canceled"
    # Defensive default: keep the engine seeing the order as live until the
    # next callback resolves it. NOT_WORKING is the only alternative and we
    # don't want to retire an order on an unknown status.
    return "open"


def _open_order_to_ccxt(entry: Dict[str, Any]) -> Dict[str, Any]:
    """Translate a cache entry (raw ibapi shape) to ccxt-shape so HTTP
    responses match the format C++ state_updater.cpp parses via
    `OrderResNormalized::from_ccxt_user_data_stream_msg`. The cache itself
    stays as venue-truth (raw ibapi statuses, string-typed numbers) for
    debuggability; translation happens at the HTTP boundary.

    Required fields per the C++ parser (order_res.cpp:58-99):
    symbol, clientOrderId, id, status (ccxt vocab), price/average,
    amount, filled, timestamp. Optional: lastTradeTimestamp,
    lastUpdateTimestamp, fee.
    """
    raw_status = entry.get("status", "")
    raw_type = (entry.get("type") or "").lower()

    if raw_type == "lmt":
        ccxt_type = "limit"
    elif raw_type == "mkt":
        ccxt_type = "market"
    else:
        ccxt_type = raw_type

    def _to_float(v: Any) -> float:
        if v in (None, ""):
            return 0.0
        try:
            return float(v)
        except (TypeError, ValueError):
            return 0.0

    avg = _to_float(entry.get("avgFillPrice", 0.0))
    limit_price = _to_float(entry.get("price"))
    # ccxt parser reads `average` first, falls back to `price`. Populate
    # both: average = avg fill (or limit when no fill), price = limit.
    effective_avg = avg if avg > 0.0 else limit_price

    now_ms = int(time.time() * 1000)
    ccxt_status = _ibkr_status_to_ccxt(raw_status)

    out: Dict[str, Any] = {
        "id": str(entry.get("id", "0")),
        "clientOrderId": str(entry.get("clientOrderId", "")),
        "symbol": entry.get("symbol", ""),
        "status": ccxt_status,
        "type": ccxt_type,
        "side": entry.get("side", ""),
        "price": limit_price,
        "average": effective_avg,
        "amount": _to_float(entry.get("amount")),
        "filled": _to_float(entry.get("filled", 0.0)),
        # Default remaining to full amount if missing — covers a brief
        # window after openOrder before the first orderStatus pushes
        # cumulative remaining/filled.
        "remaining": _to_float(
            entry.get("remaining", entry.get("amount", 0.0))
        ),
        "timestamp": now_ms,
        "lastUpdateTimestamp": now_ms,
        # ccxt convention: include the raw venue payload for debug /
        # round-trip. Keeps our own tooling debuggable without losing
        # the venue-native fields the C++ parser doesn't read.
        "info": dict(entry),
    }
    if ccxt_status == "closed":
        out["lastTradeTimestamp"] = now_ms
    return out


# US-equity universe is sourced from Polygon via `USEquityCatalog`
# (cpp_live_proxy/us_equity_catalog.py). Cached on disk under
# $DATA_DIR_ROOT/derived/us_equity_symbols.json with a 24h refresh
# cadence + background refresh daemon. ibapi has no bulk-metadata
# endpoint, so we don't fetch from IBKR — Polygon is canonical for the
# tradeable US equity universe.
#
# Tick / lot rules (precision, limits) are uniform across the universe
# we care about — see DEFAULT_PRICE_TICK / DEFAULT_LOT_SIZE in
# us_equity_catalog.py for the values and rationale.


class IBKRClient(EWrapper, EClient):
    """Owns the ibapi socket. Subclasses both EClient (send) and EWrapper
    (receive callbacks). All callback handlers run on the dispatcher
    thread; cache writes are guarded by self._lock; readers take the same
    lock for snapshot consistency.
    """

    def __init__(
        self,
        allowed_strings: "frozenset",
        broadcast_callback: "Optional[Callable[[Dict[str, Any]], None]]" = None,
    ):
        EClient.__init__(self, wrapper=self)
        EWrapper.__init__(self)
        self._lock = threading.Lock()
        self._allowed_strings = allowed_strings

        # Caches (all access under self._lock).
        self._positions: Dict[str, Dict[str, Dict[str, Any]]] = {}
        self._account_values: Dict[str, Dict[str, Dict[str, Any]]] = {}
        self._open_orders: Dict[str, Dict[str, Any]] = {}  # by clientOrderId
        # `orderId` is unique-per-clientId — TWS/mobile-placed orders all
        # arrive with orderId=0, so multiple of them collide on a single
        # key. `permId` is the exchange-side canonical identifier, unique
        # across all orders on the account regardless of placing client.
        # Use permId as the primary correlation key; keep orderId only
        # for the cancel path (which only works on orders we placed
        # ourselves, where orderId is non-zero).
        self._perm_id_to_client_id: Dict[int, str] = {}
        self._order_id_to_client_id: Dict[int, str] = {}
        self._client_id_to_order_id: Dict[str, int] = {}
        self._qualified_contracts: Dict[str, Contract] = {}
        self._managed_accounts: List[str] = []

        # Order ID allocator (set by nextValidId callback after connect).
        self._next_order_id: Optional[int] = None
        self._order_id_lock = threading.Lock()

        # Wait events for boot phase. Each one is set by the corresponding
        # ibapi snapshot-done callback so the boot path can wait for the
        # cache to actually be populated, instead of sleeping a hopeful
        # number of seconds.
        self._ready_event = threading.Event()  # nextValidId received
        self._managed_accounts_event = threading.Event()
        self._positions_end_event = threading.Event()  # positionEnd
        # Per-account snapshot-done events. Populated lazily by
        # accountDownloadEnd; pre-created in create_and_connect so the
        # boot path's wait() always finds an event to wait on even if
        # the callback fires before the wait starts.
        self._account_download_end_events: Dict[str, threading.Event] = {}
        # Set by openOrderEnd; the boot path waits on it after reqOpenOrders
        # to ensure the cache is seeded with any orders placed by us in a
        # prior session (post-restart) or by the operator via TWS.
        self._open_orders_end_event = threading.Event()
        # Set by connectionClosed (gateway dropped our socket: network
        # blip, gateway restart, or full disconnect). The session's
        # reconnect daemon thread waits on this and retries connect with
        # backoff.
        self._connection_closed_event = threading.Event()

        # Per-request waiter futures (qualifyContract, placeOrder ack).
        # Keyed by reqId or by client_order_id.
        self._contract_details_futures: Dict[int, concurrent.futures.Future] = {}
        self._contract_details_buffer: Dict[int, Contract] = {}
        self._next_req_id = 9000  # avoid collision with order ids
        self._req_id_lock = threading.Lock()
        self._order_status_futures: Dict[str, concurrent.futures.Future] = {}

        # Streaming broadcast — set by IBKRSession at construction.
        self._broadcast_callback = broadcast_callback

    # ---------- EWrapper callbacks (dispatcher thread) ----------

    def error(
        self,
        reqId: int,
        errorCode: int,
        errorString: str,
        advancedOrderRejectJson: str = "",
    ):
        # Informational codes 2104, 2106, 2158 etc. are gateway/data-farm
        # status, not real errors. Filter so the log isn't noisy.
        if 2100 <= errorCode < 2200:
            return
        log(
            f"IBKR error reqId={reqId} code={errorCode}: {errorString}",
            {"errorCode": errorCode, "reqId": reqId},
            "ERROR",
        )
        # Settle a contract-details waiter on error so callers don't hang.
        with self._lock:
            fut = self._contract_details_futures.pop(reqId, None)
            self._contract_details_buffer.pop(reqId, None)
        if fut is not None and not fut.done():
            fut.set_exception(
                RuntimeError(f"IBKR error code {errorCode}: {errorString}")
            )

    def nextValidId(self, orderId: int):
        with self._order_id_lock:
            self._next_order_id = orderId
        self._ready_event.set()

    def connectionClosed(self):
        # Gateway closed our socket. Could be a network blip, gateway
        # restart (daily ~23:45 ET), or operator stopping the gateway.
        # The dispatcher thread will return from run() shortly after this.
        # The session's reconnect daemon waits on this event and retries.
        log("IBKR connection closed by gateway", {}, "WARNING")
        self._connection_closed_event.set()

    def managedAccounts(self, accountsList: str):
        accounts = [a for a in accountsList.split(",") if a]
        with self._lock:
            self._managed_accounts = accounts
        self._managed_accounts_event.set()

    def position(
        self, account: str, contract: Contract, position: float, avgCost: float
    ):
        # Filter at the boundary — we never write entries for accounts
        # that aren't in the allowlist, so reads can't accidentally
        # surface a leaked account.
        if account not in self._allowed_strings:
            return
        with self._lock:
            self._positions.setdefault(account, {})[contract.symbol] = {
                "symbol": contract.symbol,
                "position": str(position),
                "free": str(position),
                "used": "0",
                "avgCost": avgCost,
                "exchange": contract.exchange or contract.primaryExchange,
            }

    def positionEnd(self):
        self._positions_end_event.set()

    def accountDownloadEnd(self, accountName: str):
        # Initial accountValue + portfolio snapshot for this account is
        # complete. Wake the boot path waiting for cache to fill.
        if accountName not in self._allowed_strings:
            return
        ev = self._account_download_end_events.setdefault(
            accountName, threading.Event()
        )
        ev.set()

    def updateAccountValue(
        self, key: str, val: str, currency: str, accountName: str
    ):
        if accountName not in self._allowed_strings:
            return
        with self._lock:
            self._account_values.setdefault(accountName, {})[key] = {
                "tag": key,
                "value": val,
                "currency": currency,
            }

    def updatePortfolio(
        self,
        contract: Contract,
        position: float,
        marketPrice: float,
        marketValue: float,
        averageCost: float,
        unrealizedPNL: float,
        realizedPNL: float,
        accountName: str,
    ):
        # Mirrors `position` updates; reqAccountUpdates pushes both.
        # Kept in sync with the position cache.
        if accountName not in self._allowed_strings:
            return
        with self._lock:
            self._positions.setdefault(accountName, {})[contract.symbol] = {
                "symbol": contract.symbol,
                "position": str(position),
                "free": str(position),
                "used": "0",
                "avgCost": averageCost,
                "exchange": contract.exchange or contract.primaryExchange,
            }

    def contractDetails(self, reqId: int, contractDetails):
        with self._lock:
            self._contract_details_buffer[reqId] = contractDetails.contract

    def contractDetailsEnd(self, reqId: int):
        with self._lock:
            contract = self._contract_details_buffer.pop(reqId, None)
            fut = self._contract_details_futures.pop(reqId, None)
        if fut is not None and not fut.done():
            if contract is None:
                fut.set_exception(
                    RuntimeError(f"IBKR contractDetails: no match for reqId={reqId}")
                )
            else:
                fut.set_result(contract)

    def openOrder(
        self, orderId: int, contract: Contract, order: Order, orderState
    ):
        client_order_id = order.orderRef or ""
        perm_id = int(getattr(order, "permId", 0) or 0)
        with self._lock:
            if client_order_id:
                # Maintain both indices. permId is the canonical key for
                # correlation (unique across all clientIds); orderId is
                # the cancel-path key (only meaningful for orders this
                # client placed, where it's non-zero).
                if perm_id:
                    self._perm_id_to_client_id[perm_id] = client_order_id
                if orderId:
                    self._order_id_to_client_id[orderId] = client_order_id
                    self._client_id_to_order_id[client_order_id] = orderId
                self._open_orders[client_order_id] = {
                    # Expose permId as the canonical `id` so multiple
                    # TWS-placed orders don't collide on the sentinel "0".
                    "id": str(perm_id) if perm_id else str(orderId),
                    "permId": perm_id,
                    "orderId": orderId,
                    "clientOrderId": client_order_id,
                    "symbol": contract.symbol,
                    "side": order.action.lower(),
                    "type": order.orderType.lower(),
                    "amount": str(order.totalQuantity),
                    # `filled` and `remaining` are pre-seeded so HTTP
                    # responses for orders that haven't received any
                    # orderStatus push yet (just openOrder) still emit
                    # numeric values rather than nulls. orderStatus
                    # callback overwrites both with cumulative values.
                    "filled": "0",
                    "remaining": str(order.totalQuantity),
                    "price": (
                        str(order.lmtPrice) if order.orderType == "LMT" else None
                    ),
                    "account": order.account,
                    "status": orderState.status,
                }

    def openOrderEnd(self):
        # End-of-snapshot signal for reqOpenOrders. The boot path waits on
        # this so the cache is populated before the first read.
        self._open_orders_end_event.set()

    def orderStatus(
        self,
        orderId: int,
        status: str,
        filled,
        remaining,
        avgFillPrice: float,
        permId: int,
        parentId: int,
        lastFillPrice: float,
        clientId: int,
        whyHeld: str,
        mktCapPrice: float,
    ):
        # Build a payload the C++ parser
        # (`OrderResNormalized::from_ccxt_user_data_stream_msg`) can read:
        # symbol, clientOrderId, id, status (ccxt-translated), price/average,
        # amount, filled, timestamp.
        # The cache entry from openOrder gives us symbol/amount/limit-price;
        # orderStatus provides cumulative filled and avg fill price.
        ccxt_status = _ibkr_status_to_ccxt(status)
        now_ms = int(time.time() * 1000)
        try:
            filled_f = float(filled)
        except (TypeError, ValueError):
            filled_f = 0.0
        try:
            avg_f = float(avgFillPrice)
        except (TypeError, ValueError):
            avg_f = 0.0

        with self._lock:
            # Correlate by permId first (canonical, non-zero for every
            # accepted order), then fall back to orderId (only useful for
            # our own orders, which always have non-zero orderId).
            client_order_id = (
                self._perm_id_to_client_id.get(permId) if permId else None
            )
            if client_order_id is None and orderId:
                client_order_id = self._order_id_to_client_id.get(orderId)
            if client_order_id is None:
                # Update arrived for an order whose openOrder hasn't reached
                # us yet. Drop quietly; the next orderStatus after openOrder
                # will re-emit with the cache populated.
                fut = None
                payload = None
            else:
                entry = self._open_orders.get(client_order_id)
                if entry is not None:
                    entry["status"] = status
                    entry["filled"] = str(filled)
                    entry["remaining"] = str(remaining)
                    entry["avgFillPrice"] = avg_f
                    # First orderStatus with a permId may arrive before
                    # openOrder gets a permId set (rare). Promote it.
                    if permId and not entry.get("permId"):
                        entry["permId"] = int(permId)
                        entry["id"] = str(int(permId))
                        self._perm_id_to_client_id[int(permId)] = client_order_id
                # Pull symbol/amount/limit-price from the cache (set by
                # openOrder). If openOrder hasn't run yet, fall back to
                # safe defaults — payload is still well-formed.
                symbol = (entry or {}).get("symbol", "")
                try:
                    amount = float((entry or {}).get("amount", "0"))
                except (TypeError, ValueError):
                    amount = 0.0
                try:
                    limit_price = float((entry or {}).get("price") or 0.0)
                except (TypeError, ValueError):
                    limit_price = 0.0

                # ccxt parser reads `average` first (avg fill price), then
                # falls back to `price`. We populate both: average=avg_f
                # (or limit_price when no fill yet), price=limit_price.
                effective_avg = avg_f if avg_f > 0.0 else limit_price

                # Expose permId as `id` (canonical, unique across clients).
                # Fall back to orderId only if permId hasn't been delivered
                # yet, which is rare.
                exposed_id = (
                    str(int(permId)) if permId
                    else (entry or {}).get("id") or str(orderId)
                )
                payload = {
                    "id": exposed_id,
                    "clientOrderId": client_order_id,
                    "symbol": symbol,
                    "status": ccxt_status,
                    "average": effective_avg,
                    "price": limit_price,
                    "amount": amount,
                    "filled": filled_f,
                    "timestamp": now_ms,
                    "lastUpdateTimestamp": now_ms,
                }
                if ccxt_status == "closed":
                    payload["lastTradeTimestamp"] = now_ms
                fut = self._order_status_futures.get(client_order_id)

        # Settle the placeOrder waiter on first non-pending state.
        if (
            fut is not None
            and payload is not None
            and not fut.done()
            and status in {
                "Submitted",
                "PreSubmitted",
                "Filled",
                "Cancelled",
                "ApiCancelled",
                "Inactive",
            }
        ):
            fut.set_result(payload)

        # Stream push to /api/v1/user-data/stream subscribers (C++ engine).
        if payload is not None and self._broadcast_callback is not None:
            try:
                self._broadcast_callback(
                    {
                        "event_type": "order_update",
                        "exchange_name": "ibkr",
                        "market_type": "spot",
                        "orders": [payload],
                    }
                )
            except Exception as e:
                log(f"IBKR broadcast_callback failed: {e}", {}, "ERROR")

    def execDetails(self, reqId: int, contract: Contract, execution):
        # Per-share fill record. The C++ engine extracts fills from the
        # cumulative `filled` delta carried in orderStatus, so we don't
        # broadcast execDetails — would just be unconsumed traffic. Kept
        # as a no-op override to silence the default ibapi handler.
        return

    # ---------- helpers ----------

    def allocate_order_id(self) -> int:
        with self._order_id_lock:
            if self._next_order_id is None:
                raise RuntimeError("nextValidId not yet received from gateway")
            oid = self._next_order_id
            self._next_order_id += 1
        return oid

    def allocate_req_id(self) -> int:
        with self._req_id_lock:
            rid = self._next_req_id
            self._next_req_id += 1
        return rid

    def wait_for_ready(self, timeout_s: float = CONNECT_READY_TIMEOUT_S):
        if not self._ready_event.wait(timeout=timeout_s):
            raise RuntimeError(
                f"IBKR gateway did not deliver nextValidId within {timeout_s}s — "
                f"connection likely failed silently"
            )
        if not self._managed_accounts_event.wait(timeout=timeout_s):
            raise RuntimeError(
                f"IBKR gateway did not deliver managedAccounts within {timeout_s}s"
            )

    def get_managed_accounts(self) -> List[str]:
        with self._lock:
            return list(self._managed_accounts)

    def qualify_contract(self, symbol: str, timeout_s: float = 10) -> Contract:
        with self._lock:
            cached = self._qualified_contracts.get(symbol)
        if cached is not None:
            return cached

        contract = Contract()
        contract.symbol = symbol
        contract.secType = "STK"
        contract.exchange = "SMART"
        contract.currency = "USD"

        rid = self.allocate_req_id()
        fut: concurrent.futures.Future = concurrent.futures.Future()
        with self._lock:
            self._contract_details_futures[rid] = fut
        self.reqContractDetails(rid, contract)
        qualified = fut.result(timeout=timeout_s)
        with self._lock:
            self._qualified_contracts[symbol] = qualified
        return qualified

    def positions_for(self, account_str: str) -> List[Dict[str, Any]]:
        if account_str not in self._allowed_strings:
            raise PermissionError(
                f"IBKR account {account_str!r} is not in allowlist; refused"
            )
        with self._lock:
            return list(self._positions.get(account_str, {}).values())

    def account_values_for(self, account_str: str) -> List[Dict[str, Any]]:
        if account_str not in self._allowed_strings:
            raise PermissionError(
                f"IBKR account {account_str!r} is not in allowlist; refused"
            )
        with self._lock:
            return list(self._account_values.get(account_str, {}).values())

    def open_orders_for(self, account_str: str) -> List[Dict[str, Any]]:
        if account_str not in self._allowed_strings:
            raise PermissionError(
                f"IBKR account {account_str!r} is not in allowlist; refused"
            )
        with self._lock:
            return [
                v.copy()
                for v in self._open_orders.values()
                if v.get("account") == account_str
                and v.get("status") not in {"Cancelled", "Filled", "ApiCancelled"}
            ]

    def all_orders_for(self, account_str: str) -> List[Dict[str, Any]]:
        if account_str not in self._allowed_strings:
            raise PermissionError(
                f"IBKR account {account_str!r} is not in allowlist; refused"
            )
        with self._lock:
            return [
                v.copy()
                for v in self._open_orders.values()
                if v.get("account") == account_str
            ]

    def place_order_with_clientid(
        self,
        account_str: str,
        symbol: str,
        side: str,
        order_type: str,
        amount: str,
        price: Optional[str],
        client_order_id: str,
    ) -> "concurrent.futures.Future":
        if account_str not in self._allowed_strings:
            raise PermissionError(
                f"IBKR account {account_str!r} is not in allowlist; refused"
            )
        if not client_order_id:
            raise ValueError("IBKR place_order requires a client_order_id")
        action = side.upper()
        if action not in {"BUY", "SELL"}:
            raise ValueError(f"IBKR place_order: bad side {side!r}")
        if order_type not in {"limit", "market"}:
            raise ValueError(f"IBKR place_order: unsupported type {order_type!r}")

        contract = self.qualify_contract(symbol)

        order = Order()
        order.action = action
        order.totalQuantity = float(amount)
        if order_type == "limit":
            if price is None:
                raise ValueError("IBKR limit order requires price")
            order.orderType = "LMT"
            order.lmtPrice = float(price)
        else:
            order.orderType = "MKT"
        order.account = account_str
        order.orderRef = client_order_id
        order.eTradeOnly = False
        order.firmQuoteOnly = False
        if not order.account or not order.orderRef:
            raise RuntimeError("IBKR order missing account or orderRef post-assignment")

        oid = self.allocate_order_id()

        fut: concurrent.futures.Future = concurrent.futures.Future()
        with self._lock:
            self._order_status_futures[client_order_id] = fut
            self._client_id_to_order_id[client_order_id] = oid
            self._order_id_to_client_id[oid] = client_order_id

        self.placeOrder(oid, contract, order)
        return fut

    def cancel_by_client_order_id(
        self, account_str: str, client_order_id: str
    ) -> Dict[str, Any]:
        if account_str not in self._allowed_strings:
            raise PermissionError(
                f"IBKR account {account_str!r} is not in allowlist; refused"
            )
        with self._lock:
            oid = self._client_id_to_order_id.get(client_order_id)
            entry = self._open_orders.get(client_order_id)
        if oid is None:
            raise LookupError(
                f"IBKR cancel: no live order with clientOrderId={client_order_id!r}"
            )
        if entry is not None and entry.get("account") != account_str:
            raise PermissionError(
                f"IBKR cancel: clientOrderId {client_order_id!r} maps to account "
                f"{entry.get('account')}, not {account_str}; refused"
            )
        # ibapi 9.81 takes (orderId,) only; newer versions added a
        # `manualCancelOrderTime` second arg. Stay compatible with our
        # pinned version.
        self.cancelOrder(oid)
        return {
            "id": str(oid),
            "clientOrderId": client_order_id,
            "status": "canceling",
        }


class IBKRSession:
    """Process singleton wrapping one IBKRClient and a dispatcher thread."""

    _instance: "Optional[IBKRSession]" = None

    def __init__(
        self,
        client: IBKRClient,
        thread: threading.Thread,
        host: str,
        port: int,
        account_id_to_ibkr_string: Dict[int, str],
        allowed_account_ids: Iterable[int],
    ):
        self._client = client
        self._thread = thread
        self.host = host
        self.port = port
        self._account_id_to_ibkr_string = dict(account_id_to_ibkr_string)
        self._allowed_account_ids = frozenset(allowed_account_ids)
        self._allowed_strings = frozenset(
            self._account_id_to_ibkr_string.values()
        )
        # Watchdog thread re-claims our subscriptions on a fixed cadence.
        # Started by start_watchdog() after connect; stopped by disconnect.
        self._watchdog_stop = threading.Event()
        self._watchdog_thread: Optional[threading.Thread] = None
        # Reconnect daemon. Waits on the client's connection_closed_event,
        # retries connect with backoff, then re-runs the boot sequence
        # (subscriptions + cache seeds). Started by start_reconnect_daemon
        # after the initial connect succeeds.
        self._reconnect_stop = threading.Event()
        self._reconnect_thread: Optional[threading.Thread] = None
        # Persisted for reconnect — must remember how the original connect
        # was parameterized.
        self._client_id: Optional[int] = None
        # Optional broadcast callback; only used by tests / explicit setup.
        self._broadcast_callback: "Optional[Callable[[Dict[str, Any]], None]]" = None

    @classmethod
    def _validate_invariant(
        cls,
        account_id_to_ibkr_string: Dict[int, str],
        allowed_account_ids: Iterable[int],
    ) -> "frozenset":
        if cls._instance is not None:
            raise RuntimeError(
                "IBKRSession already created; refuse to open a second connection"
            )
        allowed_set = frozenset(allowed_account_ids)
        configured_ids = frozenset(account_id_to_ibkr_string.keys())
        if configured_ids != allowed_set:
            raise RuntimeError(
                f"IBKR config drift: accounts.json account_ids={set(configured_ids)} "
                f"!= IBKR_ALLOWED_ACCOUNT_IDS={set(allowed_set)}. "
                f"Refusing to start. Edit both in lockstep."
            )
        return allowed_set

    @classmethod
    def _post_connect_check(
        cls,
        client: IBKRClient,
        thread: threading.Thread,
        host: str,
        port: int,
        account_id_to_ibkr_string: Dict[int, str],
        allowed_set: "frozenset",
        client_id: int = 17,
        broadcast_callback: "Optional[Callable[[Dict[str, Any]], None]]" = None,
    ) -> "IBKRSession":
        managed = set(client.get_managed_accounts())
        mapped_strings = set(account_id_to_ibkr_string.values())
        missing = mapped_strings - managed
        if missing:
            client.disconnect()
            raise RuntimeError(
                f"IBKR mapped accounts {missing} not present in gateway "
                f"managedAccounts={managed}. Wrong gateway login?"
            )
        leaked = managed - mapped_strings
        if leaked:
            log(
                f"IBKR gateway exposes accounts {leaked} that are deliberately "
                f"unmapped; they will be ignored. This is expected if a single "
                f"IBKR login holds both trading and non-trading accounts.",
                {},
                "WARNING",
            )
        session = cls(
            client=client,
            thread=thread,
            host=host,
            port=port,
            account_id_to_ibkr_string=account_id_to_ibkr_string,
            allowed_account_ids=frozenset(account_id_to_ibkr_string.keys()),
        )
        # Persist params needed for reconnect.
        session._client_id = client_id
        session._broadcast_callback = broadcast_callback
        cls._instance = session
        # Start the subscription-stealing watchdog. See WATCHDOG_INTERVAL_S
        # comment for the rationale; in short, ibapi's reqAccountUpdates
        # is single-subscriber-per-account and silently transfers ownership
        # to whichever client subscribed last. Re-subscribing on a fixed
        # cadence makes us self-healing.
        session.start_watchdog()
        # Auto-reconnect daemon: detects connectionClosed (gateway dropped
        # our socket), retries connect with backoff, re-runs the boot
        # sequence (subscriptions + cache seed). Handles network blips
        # and the daily 23:45 ET gateway restart (when the gateway is
        # configured for auto-restart — once-per-week mandatory re-auth
        # still requires operator action).
        session.start_reconnect_daemon()
        log(
            f"IBKR session connected to {host}:{port}; trading accounts: "
            f"{sorted(mapped_strings)}",
            {},
            "INFO",
        )
        return session

    @classmethod
    def _build_and_connect(
        cls,
        host: str,
        port: int,
        client_id: int,
        allowed_strings: "frozenset",
        broadcast_callback: "Optional[Callable[[Dict[str, Any]], None]]",
    ) -> "tuple[IBKRClient, threading.Thread]":
        client = IBKRClient(
            allowed_strings=allowed_strings,
            broadcast_callback=broadcast_callback,
        )
        client.connect(host, port, client_id)
        thread = threading.Thread(
            target=client.run,
            name=f"ibkr-dispatcher-{client_id}",
            daemon=True,
        )
        thread.start()
        client.wait_for_ready()
        return client, thread

    @classmethod
    def create_and_connect(
        cls,
        host: str,
        port: int,
        account_id_to_ibkr_string: Dict[int, str],
        allowed_account_ids: Iterable[int],
        client_id: int = 17,
        broadcast_callback: "Optional[Callable[[Dict[str, Any]], None]]" = None,
    ) -> "IBKRSession":
        """Sync entry point — for scripts and tests. Safe to call from
        any thread; ibapi has no asyncio dependency."""
        allowed_set = cls._validate_invariant(
            account_id_to_ibkr_string, allowed_account_ids
        )
        allowed_strings = frozenset(account_id_to_ibkr_string.values())
        client, thread = cls._build_and_connect(
            host, port, client_id, allowed_strings, broadcast_callback
        )
        # Pre-create the per-account snapshot events BEFORE issuing
        # reqAccountUpdates so the wait below always finds an event,
        # even if accountDownloadEnd races ahead of our wait() call.
        for account_str in allowed_strings:
            client._account_download_end_events.setdefault(
                account_str, threading.Event()
            )

        # Subscribe to streaming caches. Each call returns immediately;
        # the gateway responds asynchronously with snapshot push +
        # snapshot-done callbacks (positionEnd / accountDownloadEnd /
        # openOrderEnd). We wait on those callbacks below — never sleep.
        client.reqPositions()
        for account_str in allowed_strings:
            client.reqAccountUpdates(True, account_str)
        # Seed the open-orders cache from the gateway's authoritative view.
        # `reqAllOpenOrders` returns ALL open orders on the account
        # regardless of which clientId placed them — covers orders placed
        # via TWS, mobile, or a prior proxy session. `reqOpenOrders`
        # would only return orders this clientId placed in this session,
        # which is empty at boot. (One-shot snapshot; ongoing updates
        # for OUR own orders flow via the openOrder/orderStatus callbacks
        # that fire as part of placeOrder. Catching mid-session orders
        # placed by external clients is a follow-up via periodic poll.)
        client.reqAllOpenOrders()

        # Wait for actual completion signals — not a fixed sleep. If a
        # signal doesn't arrive within SNAPSHOT_TIMEOUT_S, log and proceed
        # so a partially-degraded gateway can't hang the proxy boot.
        if not client._positions_end_event.wait(timeout=SNAPSHOT_TIMEOUT_S):
            log(
                f"IBKR positionEnd not received within {SNAPSHOT_TIMEOUT_S}s; "
                f"position cache may be incomplete at boot",
                {},
                "WARNING",
            )
        for account_str in allowed_strings:
            ev = client._account_download_end_events[account_str]
            if not ev.wait(timeout=SNAPSHOT_TIMEOUT_S):
                log(
                    f"IBKR accountDownloadEnd({account_str}) not received "
                    f"within {SNAPSHOT_TIMEOUT_S}s; account-values cache "
                    f"may be incomplete at boot",
                    {},
                    "WARNING",
                )
        if not client._open_orders_end_event.wait(timeout=SNAPSHOT_TIMEOUT_S):
            log(
                f"IBKR openOrderEnd not received within {SNAPSHOT_TIMEOUT_S}s; "
                f"open-orders cache may be incomplete at boot",
                {},
                "WARNING",
            )
        return cls._post_connect_check(
            client,
            thread,
            host,
            port,
            account_id_to_ibkr_string,
            allowed_set,
            client_id=client_id,
            broadcast_callback=broadcast_callback,
        )

    @classmethod
    async def create_and_connect_async(
        cls,
        host: str,
        port: int,
        account_id_to_ibkr_string: Dict[int, str],
        allowed_account_ids: Iterable[int],
        client_id: int = 17,
        broadcast_callback: "Optional[Callable[[Dict[str, Any]], None]]" = None,
    ) -> "IBKRSession":
        """Async entry point — defers the (purely sync) connect work to a
        worker thread so the calling event loop isn't blocked. ibapi
        itself doesn't care about the loop; the only reason this is async
        is to keep uvicorn's lifespan responsive during the ~2s settle."""
        return await asyncio.to_thread(
            cls.create_and_connect,
            host,
            port,
            account_id_to_ibkr_string,
            allowed_account_ids,
            client_id,
            broadcast_callback,
        )

    @classmethod
    def get_instance(cls) -> "Optional[IBKRSession]":
        return cls._instance

    @classmethod
    def reset_for_tests(cls) -> None:
        if cls._instance is not None:
            try:
                cls._instance.disconnect()
            except Exception:
                pass
        cls._instance = None

    def disconnect(self) -> None:
        # Stop helpers before tearing down the socket so they don't
        # try to issue requests on a closing client or attempt to
        # reconnect over a deliberate shutdown.
        self._watchdog_stop.set()
        self._reconnect_stop.set()
        if self._watchdog_thread is not None and self._watchdog_thread.is_alive():
            self._watchdog_thread.join(timeout=2)
        if self._reconnect_thread is not None and self._reconnect_thread.is_alive():
            self._reconnect_thread.join(timeout=2)
        try:
            if self._client.isConnected():
                self._client.disconnect()
        except Exception:
            pass
        if self._thread is not None and self._thread.is_alive():
            self._thread.join(timeout=5)
        type(self)._instance = None

    def is_connected(self) -> bool:
        return self._client.isConnected()

    def start_watchdog(
        self, interval_s: float = WATCHDOG_INTERVAL_S
    ) -> None:
        """Start the subscription-stealing watchdog. Idempotent."""
        if self._watchdog_thread is not None and self._watchdog_thread.is_alive():
            return
        self._watchdog_stop.clear()
        self._watchdog_thread = threading.Thread(
            target=self._watchdog_loop,
            args=(interval_s,),
            name="ibkr-watchdog",
            daemon=True,
        )
        self._watchdog_thread.start()

    def _watchdog_loop(self, interval_s: float) -> None:
        # Periodically force a fresh subscription snapshot. We toggle
        # `reqAccountUpdates(False, account)` then `reqAccountUpdates(True,
        # account)` because a plain `True` re-subscribe behaves as a
        # silent resume: the gateway only delivers per-currency rows
        # (USD/EUR) on a NEW subscription, not on a resume — meaning
        # after a subscription steal+release cycle, our cache would only
        # see metadata (BASE) rows on resume. Verified empirically.
        # Also re-runs reqAllOpenOrders so mid-session orders placed
        # by external clients (TWS, mobile) enter our cache.
        while not self._watchdog_stop.wait(interval_s):
            if not self._client.isConnected():
                continue
            try:
                for account_str in self._allowed_strings:
                    # `False` requires an empty string per ibapi semantics.
                    self._client.reqAccountUpdates(False, "")
                    self._client.reqAccountUpdates(True, account_str)
                self._client.reqAllOpenOrders()
            except Exception as e:
                log(
                    f"IBKR watchdog tick failed: {type(e).__name__}: {e}",
                    {},
                    "ERROR",
                )

    def start_reconnect_daemon(self) -> None:
        """Start the auto-reconnect daemon. Idempotent."""
        if (
            self._reconnect_thread is not None
            and self._reconnect_thread.is_alive()
        ):
            return
        self._reconnect_stop.clear()
        self._reconnect_thread = threading.Thread(
            target=self._reconnect_loop,
            name="ibkr-reconnect",
            daemon=True,
        )
        self._reconnect_thread.start()

    def _reconnect_loop(self) -> None:
        # Block on the client's connection_closed event. When it fires,
        # the dispatcher thread is exiting; we retry connect with
        # exponential backoff (capped at 60s), then re-run the boot
        # sequence so caches refill from the gateway's authoritative
        # view. The watchdog already keeps subscriptions alive across
        # ownership steals, so reconnect just needs to handle the
        # session-level lifecycle.
        backoff_s = 2.0
        max_backoff_s = 60.0
        while not self._reconnect_stop.is_set():
            # Wait up to 1s for the close signal so we can also notice
            # shutdown via _reconnect_stop.
            if not self._client._connection_closed_event.wait(timeout=1.0):
                continue
            # Drain the event so we can detect a future close.
            self._client._connection_closed_event.clear()
            log(
                "IBKR reconnect daemon: connection closed; attempting "
                "to re-establish",
                {},
                "WARNING",
            )
            # Give the dispatcher thread a moment to wind down.
            if self._thread is not None and self._thread.is_alive():
                self._thread.join(timeout=3)
            attempt = 0
            while not self._reconnect_stop.is_set():
                attempt += 1
                try:
                    self._attempt_reconnect()
                    log(
                        f"IBKR reconnect daemon: success on attempt {attempt}",
                        {},
                        "WARNING",
                    )
                    backoff_s = 2.0
                    break
                except Exception as e:
                    log(
                        f"IBKR reconnect attempt {attempt} failed: "
                        f"{type(e).__name__}: {e}; retrying in "
                        f"{backoff_s:.0f}s",
                        {},
                        "ERROR",
                    )
                    if self._reconnect_stop.wait(timeout=backoff_s):
                        return
                    backoff_s = min(backoff_s * 2.0, max_backoff_s)

    def _attempt_reconnect(self) -> None:
        """One reconnect attempt. Throws on any failure so the loop can
        back off and retry."""
        if self._client_id is None:
            raise RuntimeError("reconnect: client_id not persisted")
        # Reset the boot-phase events on the existing client; ibapi's
        # EClient.connect resets internal `done` flags and recreates the
        # socket. We then start a new dispatcher thread (the previous
        # one returned from run() when the connection dropped).
        self._client._ready_event.clear()
        self._client._managed_accounts_event.clear()
        self._client._positions_end_event.clear()
        self._client._open_orders_end_event.clear()
        for ev in self._client._account_download_end_events.values():
            ev.clear()
        # ibapi sets self.done in disconnect; reset for reconnect.
        try:
            self._client.done = False
        except Exception:
            pass

        self._client.connect(self.host, self.port, self._client_id)
        new_thread = threading.Thread(
            target=self._client.run,
            name=f"ibkr-dispatcher-reconn-{self._client_id}",
            daemon=True,
        )
        new_thread.start()
        self._thread = new_thread

        self._client.wait_for_ready()

        # Re-run boot subscriptions. Identical sequence to the initial
        # connect, just on the existing client instance.
        self._client.reqPositions()
        for account_str in self._allowed_strings:
            self._client.reqAccountUpdates(True, account_str)
        self._client.reqAllOpenOrders()
        # Wait for fresh snapshots before we declare success.
        self._client._positions_end_event.wait(timeout=SNAPSHOT_TIMEOUT_S)
        for account_str in self._allowed_strings:
            ev = self._client._account_download_end_events.setdefault(
                account_str, threading.Event()
            )
            ev.wait(timeout=SNAPSHOT_TIMEOUT_S)
        self._client._open_orders_end_event.wait(timeout=SNAPSHOT_TIMEOUT_S)

    # ---------- public read API (sync, all threads safe) ----------

    def positions(self, account_str: str) -> List[Dict[str, Any]]:
        return self._client.positions_for(account_str)

    def account_summary(self, account_str: str) -> List[Dict[str, Any]]:
        return self._client.account_values_for(account_str)

    def open_trades(self, account_str: str) -> List[Dict[str, Any]]:
        return self._client.open_orders_for(account_str)

    def trades(self, account_str: str) -> List[Dict[str, Any]]:
        return self._client.all_orders_for(account_str)

    def query_order_by_client_id(
        self, account_str: str, client_order_id: str
    ) -> Optional[Dict[str, Any]]:
        if account_str not in self._allowed_strings:
            raise PermissionError(
                f"IBKR account {account_str!r} not in allowlist; refused"
            )
        all_orders = self._client.all_orders_for(account_str)
        for o in all_orders:
            if o.get("clientOrderId") == client_order_id:
                return o
        return None

    # ---------- public write API ----------

    def place_order(
        self,
        account_str: str,
        symbol: str,
        side: str,
        order_type: str,
        amount: str,
        price: Optional[str],
        client_order_id: str,
        timeout_s: float = ORDER_ACK_TIMEOUT_S,
    ) -> Dict[str, Any]:
        """Sync — submit + wait for first non-pending orderStatus."""
        fut = self._client.place_order_with_clientid(
            account_str=account_str,
            symbol=symbol,
            side=side,
            order_type=order_type,
            amount=amount,
            price=price,
            client_order_id=client_order_id,
        )
        return fut.result(timeout=timeout_s)

    async def place_order_async(
        self,
        account_str: str,
        symbol: str,
        side: str,
        order_type: str,
        amount: str,
        price: Optional[str],
        client_order_id: str,
        timeout_s: float = ORDER_ACK_TIMEOUT_S,
    ) -> Dict[str, Any]:
        """Async — same as place_order but bridges the dispatcher-thread
        future onto the running event loop via asyncio.wrap_future."""
        fut = self._client.place_order_with_clientid(
            account_str=account_str,
            symbol=symbol,
            side=side,
            order_type=order_type,
            amount=amount,
            price=price,
            client_order_id=client_order_id,
        )
        return await asyncio.wait_for(
            asyncio.wrap_future(fut), timeout=timeout_s
        )

    def cancel_order_by_client_id(
        self, account_str: str, client_order_id: str
    ) -> Dict[str, Any]:
        return self._client.cancel_by_client_order_id(
            account_str, client_order_id
        )


class IBKRAccountClient:
    """Per-account ccxt-shape facade. Pinned at construction to one
    (internal account_id, IBKR account string). Slots into
    CcxtWrapper.exchange_clients alongside ccxt clients."""

    markets: Dict[str, Any]

    def __init__(
        self,
        session: IBKRSession,
        internal_account_id: int,
        ibkr_account_string: str,
        allowed_account_ids: Iterable[int],
    ):
        allowed = frozenset(allowed_account_ids)
        if internal_account_id not in allowed:
            raise PermissionError(
                f"IBKRAccountClient construction refused: internal_account_id="
                f"{internal_account_id} not in allowlist {set(allowed)}"
            )
        self._session = session
        self._account_id = internal_account_id
        self._account_str = ibkr_account_string
        # Seed the markets dict from the Polygon-sourced catalog. The
        # singleton handles disk cache + 24h refresh; first call here
        # may take a few seconds on cold cache / cold boot. Subsequent
        # IBKRAccountClient constructions reuse the cached universe.
        # Copy so per-instance mutations don't leak between accounts.
        from cpp_live_proxy.us_equity_catalog import USEquityCatalog
        self.markets = USEquityCatalog.get_instance().markets()

    @property
    def account_string(self) -> str:
        return self._account_str

    @property
    def internal_account_id(self) -> int:
        return self._account_id

    async def load_markets(self, reload: bool = False) -> Dict[str, Any]:
        # ccxt clients accept a `reload` flag; we don't lazy-load IBKR
        # market metadata yet (US equities only, no symbol catalog).
        # Accept the arg for signature compatibility with periodic_jobs.
        return self.markets

    def set_sandbox_mode(self, on: bool) -> None:
        port = self._session.port
        if on and port != 4002:
            raise RuntimeError(
                f"IBKR sandbox mode requested but gateway port is {port} "
                f"(live=4001/paper=4002)"
            )
        if not on and port != 4001:
            raise RuntimeError(
                f"IBKR live mode requested but gateway port is {port} "
                f"(live=4001/paper=4002)"
            )

    async def create_order(
        self,
        *,
        symbol: str,
        type: str,
        side: str,
        amount: str,
        price: Optional[str] = None,
        params: Optional[Dict[str, Any]] = None,
    ) -> Dict[str, Any]:
        params = params or {}
        client_order_id = params.get("clientOrderId")
        if not client_order_id:
            raise ValueError("IBKR create_order requires params['clientOrderId']")
        return await self._session.place_order_async(
            account_str=self._account_str,
            symbol=symbol,
            side=side,
            order_type=type,
            amount=amount,
            price=price,
            client_order_id=client_order_id,
        )

    async def cancel_order(
        self,
        id: Optional[str],
        symbol: str,
        params: Optional[Dict[str, Any]] = None,
    ) -> Dict[str, Any]:
        params = params or {}
        client_order_id = params.get("clientOrderId") or id
        if not client_order_id:
            raise ValueError("IBKR cancel_order requires clientOrderId")
        return self._session.cancel_order_by_client_id(
            account_str=self._account_str,
            client_order_id=client_order_id,
        )

    async def fetch_order(
        self,
        id: Optional[str],
        symbol: str,
        params: Optional[Dict[str, Any]] = None,
    ) -> Optional[Dict[str, Any]]:
        params = params or {}
        client_order_id = params.get("clientOrderId") or id
        if not client_order_id:
            raise ValueError("IBKR fetch_order requires clientOrderId")
        raw = self._session.query_order_by_client_id(
            account_str=self._account_str,
            client_order_id=client_order_id,
        )
        return _open_order_to_ccxt(raw) if raw is not None else None

    async def fetch_orders(
        self,
        symbol: Optional[str] = None,
        limit: Optional[int] = None,
    ) -> List[Dict[str, Any]]:
        out = self._session.trades(self._account_str)
        if symbol is not None:
            out = [o for o in out if o.get("symbol") == symbol]
        if limit is not None:
            out = out[:limit]
        return [_open_order_to_ccxt(o) for o in out]

    async def fetch_open_orders(
        self,
        symbol: Optional[str] = None,
        params: Optional[Dict[str, Any]] = None,
    ) -> List[Dict[str, Any]]:
        out = self._session.open_trades(self._account_str)
        if symbol is not None:
            out = [o for o in out if o.get("symbol") == symbol]
        return [_open_order_to_ccxt(o) for o in out]

    async def fetch_balance(
        self, params: Optional[Dict[str, Any]] = None
    ) -> Dict[str, Any]:
        rows = self._session.account_summary(self._account_str)
        positions = self._session.positions(self._account_str)

        # Per-currency cash/netAsset rollup. ibapi sends ~20 accountValue
        # entries per currency (one per tag); pick the right tag for each
        # field and emit one row per currency.
        by_currency: Dict[str, Dict[str, Any]] = {}
        cash_value = "0"
        for r in rows:
            tag = r.get("tag")
            currency = r.get("currency", "")
            if not currency or currency == "BASE":
                continue
            entry = by_currency.setdefault(
                currency,
                {"currency": currency, "cash": "0", "netAsset": "0"},
            )
            if tag == "TotalCashBalance" or tag == "TotalCashValue":
                entry["cash"] = r["value"]
                if cash_value == "0":
                    cash_value = r["value"]
            elif tag == "NetLiquidationByCurrency" or tag == "NetLiquidation":
                entry["netAsset"] = r["value"]

        info_rows = list(by_currency.values())
        for p in positions:
            info_rows.append(
                {
                    "symbol": p["symbol"],
                    "position": p["position"],
                    "free": p["free"],
                    "used": p["used"],
                }
            )
        return {
            "info": info_rows,
            "total_cash_value": cash_value,
        }

    async def fetch_positions(
        self, symbols: Optional[List[str]] = None
    ) -> List[Dict[str, Any]]:
        positions = self._session.positions(self._account_str)
        if symbols is not None:
            positions = [p for p in positions if p["symbol"] in symbols]
        return positions
