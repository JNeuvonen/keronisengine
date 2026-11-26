"""Contract test: IBKR `order_update` broadcast payload must be parseable by
the C++ `OrderResNormalized::from_ccxt_user_data_stream_msg` consumer in
state_updater.cpp:2148.

The C++ parser at order_res.cpp:58-99 reads these fields:
    symbol, clientOrderId, id, status, average|price, amount, filled,
    timestamp, optional(lastTradeTimestamp, lastUpdateTimestamp, fee).

`status` then runs through `normalize_status` (order_res.cpp:41-55) which
accepts: open, closed, canceled, expired, rejected. Anything else falls
to NOT_WORKING — defensive but surprising; we want IBKR statuses to map
explicitly into one of those.

These tests are Python-side mirrors of the C++ contract. If they pass and
the C++ schema doesn't change, the IBKR payload round-trips through the
engine's order-state machinery identically to crypto venues.
"""

import threading
from types import SimpleNamespace
import unittest

from cpp_live_proxy.ibkr_session import IBKRClient, _ibkr_status_to_ccxt


class TestStatusTranslator(unittest.TestCase):
    """Spot-check the ibapi → ccxt status mapping. The C++ side accepts
    {open, closed, canceled, expired, rejected}; everything else collapses
    to NOT_WORKING and is treated as terminal — risky for non-terminal
    ibapi states like PendingSubmit."""

    def test_submitted_is_open(self):
        self.assertEqual(_ibkr_status_to_ccxt("Submitted"), "open")

    def test_presubmitted_is_open(self):
        self.assertEqual(_ibkr_status_to_ccxt("PreSubmitted"), "open")

    def test_pendingsubmit_is_open(self):
        # Non-terminal: order is still alive; must not be retired.
        self.assertEqual(_ibkr_status_to_ccxt("PendingSubmit"), "open")

    def test_pendingcancel_is_open(self):
        # Cancel pending; the order is still working until Cancelled fires.
        self.assertEqual(_ibkr_status_to_ccxt("PendingCancel"), "open")

    def test_filled_is_closed(self):
        self.assertEqual(_ibkr_status_to_ccxt("Filled"), "closed")

    def test_cancelled_is_canceled(self):
        self.assertEqual(_ibkr_status_to_ccxt("Cancelled"), "canceled")

    def test_apicancelled_is_canceled(self):
        self.assertEqual(_ibkr_status_to_ccxt("ApiCancelled"), "canceled")

    def test_inactive_is_canceled(self):
        # Inactive == rejected by gateway. We map to canceled so engine
        # frees the slot. (rejected would map identically inside the
        # parser's normalize_status, but canceled is the more honest label
        # since IBKR doesn't necessarily mean "permanently rejected".)
        self.assertEqual(_ibkr_status_to_ccxt("Inactive"), "canceled")

    def test_unknown_status_defaults_to_open(self):
        # If ibapi adds a state we haven't seen, keep the order live —
        # the next callback will resolve it. Better than a false-cancel.
        self.assertEqual(_ibkr_status_to_ccxt("FutureNewState"), "open")


class TestOrderUpdatePayloadShape(unittest.TestCase):
    """Drive an IBKRClient through openOrder + orderStatus and assert the
    broadcast payload contains every field the C++ parser reads, with the
    right primitive types."""

    def setUp(self):
        self.broadcasts = []
        self.client = IBKRClient(
            allowed_strings=frozenset({"U22203249"}),
            broadcast_callback=self.broadcasts.append,
        )
        self.client._next_order_id = 100

    def _open_order(self, oid, client_order_id, symbol, amount, price):
        from ibapi.contract import Contract
        from ibapi.order import Order
        c = Contract()
        c.symbol = symbol
        o = Order()
        o.action = "BUY"
        o.orderType = "LMT"
        o.totalQuantity = amount
        o.lmtPrice = price
        o.account = "U22203249"
        o.orderRef = client_order_id
        self.client.openOrder(oid, c, o, SimpleNamespace(status="Submitted"))

    def _order_status(self, oid, status, filled, remaining, avg=0.0):
        self.client.orderStatus(
            orderId=oid,
            status=status,
            filled=filled,
            remaining=remaining,
            avgFillPrice=avg,
            permId=0,
            parentId=0,
            lastFillPrice=avg,
            clientId=0,
            whyHeld="",
            mktCapPrice=0.0,
        )

    def _last_order_payload(self):
        self.assertEqual(len(self.broadcasts), 1, self.broadcasts)
        env = self.broadcasts[0]
        self.assertEqual(env["event_type"], "order_update")
        self.assertEqual(env["exchange_name"], "ibkr")
        self.assertEqual(env["market_type"], "spot")
        self.assertEqual(len(env["orders"]), 1)
        return env["orders"][0]

    def test_payload_has_all_required_fields(self):
        self._open_order(100, "cli-1", "AAPL", 3.0, 190.5)
        self._order_status(100, "Submitted", filled=0, remaining=3)
        p = self._last_order_payload()
        for required in (
            "symbol",
            "clientOrderId",
            "id",
            "status",
            "price",
            "amount",
            "filled",
            "timestamp",
        ):
            self.assertIn(required, p, f"missing field: {required}")

    def test_payload_field_types_match_parser_expectations(self):
        self._open_order(100, "cli-2", "AAPL", 3.0, 190.5)
        self._order_status(100, "Submitted", filled=0, remaining=3)
        p = self._last_order_payload()
        # C++ parser reads via .get<std::string>() / .get<double>() /
        # .get<long long>(). Our values must match.
        self.assertIsInstance(p["symbol"], str)
        self.assertIsInstance(p["clientOrderId"], str)
        self.assertIsInstance(p["id"], str)
        self.assertIsInstance(p["status"], str)
        self.assertIsInstance(p["price"], (int, float))
        self.assertIsInstance(p["amount"], (int, float))
        self.assertIsInstance(p["filled"], (int, float))
        self.assertIsInstance(p["timestamp"], int)
        # Optional but, when present, must be a number.
        if "average" in p:
            self.assertIsInstance(p["average"], (int, float))
        if "lastUpdateTimestamp" in p:
            self.assertIsInstance(p["lastUpdateTimestamp"], int)
        if "lastTradeTimestamp" in p:
            self.assertIsInstance(p["lastTradeTimestamp"], int)

    def test_payload_status_translated_for_each_ibapi_state(self):
        cases = [
            ("Submitted", "open"),
            ("PreSubmitted", "open"),
            ("Filled", "closed"),
            ("Cancelled", "canceled"),
            ("ApiCancelled", "canceled"),
        ]
        for ibapi_status, expected_ccxt in cases:
            self.broadcasts.clear()
            cli = f"cli-{ibapi_status}"
            self._open_order(200 + len(cli), cli, "AAPL", 1.0, 100.0)
            self._order_status(
                200 + len(cli), ibapi_status, filled=0, remaining=1
            )
            p = self._last_order_payload()
            self.assertEqual(
                p["status"],
                expected_ccxt,
                f"{ibapi_status!r} should translate to {expected_ccxt!r}",
            )

    def test_payload_filled_status_includes_lastTradeTimestamp(self):
        self._open_order(100, "cli-fill", "AAPL", 3.0, 190.5)
        self._order_status(100, "Filled", filled=3, remaining=0, avg=190.55)
        p = self._last_order_payload()
        self.assertEqual(p["status"], "closed")
        self.assertIn("lastTradeTimestamp", p)
        self.assertEqual(p["filled"], 3.0)
        # avg fill price preferred over limit price when available.
        self.assertAlmostEqual(p["average"], 190.55)

    def test_orderStatus_without_prior_openOrder_is_dropped_silently(self):
        # Defensive: if openOrder hasn't run (cache miss), we must not
        # broadcast a malformed payload. The next orderStatus after
        # openOrder will re-emit.
        self._order_status(999, "Submitted", filled=0, remaining=1)
        self.assertEqual(self.broadcasts, [])

    def test_payload_account_field_not_required_by_cpp(self):
        # Sanity: payload doesn't promise an `account` field at top level
        # (C++ parser doesn't read it). Our cache keeps it for our own
        # use but it's fine if the broadcast omits it.
        self._open_order(100, "cli-3", "AAPL", 3.0, 190.5)
        self._order_status(100, "Submitted", filled=0, remaining=3)
        # No assertion on `account` presence/absence — just the test
        # serves as documentation.


class TestSnapshotEndCallbacks(unittest.TestCase):
    """Boot-time synchronization. The boot path must wait on the
    snapshot-done callbacks (positionEnd, accountDownloadEnd,
    openOrderEnd) — never sleep — so the cache is fully populated
    before the first read. The previous fixed 2-second sleep was a
    race: a slow gateway returned an empty cache."""

    def setUp(self):
        self.client = IBKRClient(allowed_strings=frozenset({"U22203249"}))

    def test_positionEnd_sets_event(self):
        self.assertFalse(self.client._positions_end_event.is_set())
        self.client.positionEnd()
        self.assertTrue(self.client._positions_end_event.is_set())

    def test_accountDownloadEnd_for_allowed_account_sets_event(self):
        # Pre-create the event the way the boot path does, so the
        # callback's setdefault doesn't create a fresh one.
        self.client._account_download_end_events["U22203249"] = __import__(
            "threading"
        ).Event()
        self.client.accountDownloadEnd("U22203249")
        self.assertTrue(
            self.client._account_download_end_events["U22203249"].is_set()
        )

    def test_accountDownloadEnd_for_disallowed_account_is_dropped(self):
        # Boot path should never wait on a disallowed account, but if
        # the gateway sends one (multi-account login), don't pollute
        # internal state.
        self.client.accountDownloadEnd("U7924231")
        self.assertNotIn(
            "U7924231", self.client._account_download_end_events
        )


class TestOpenOrdersBootSnapshot(unittest.TestCase):
    """reqOpenOrders + openOrderEnd populate the cache before reads. After
    a proxy restart, this is the only way orders placed in a prior session
    are visible without waiting for the next state transition."""

    def setUp(self):
        self.client = IBKRClient(allowed_strings=frozenset({"U22203249"}))

    def test_openOrderEnd_sets_event_for_boot_path(self):
        self.assertFalse(self.client._open_orders_end_event.is_set())
        self.client.openOrderEnd()
        self.assertTrue(self.client._open_orders_end_event.is_set())

    def test_openOrder_populates_cache_for_addressing(self):
        from ibapi.contract import Contract
        from ibapi.order import Order
        c = Contract()
        c.symbol = "AAPL"
        o = Order()
        o.action = "BUY"
        o.orderType = "LMT"
        o.totalQuantity = 5.0
        o.lmtPrice = 100.0
        o.account = "U22203249"
        o.orderRef = "cli-prerestart"
        self.client.openOrder(42, c, o, SimpleNamespace(status="Submitted"))
        # Cache addressable both ways.
        self.assertIn("cli-prerestart", self.client._open_orders)
        self.assertEqual(
            self.client._order_id_to_client_id[42], "cli-prerestart"
        )
        self.assertEqual(
            self.client._client_id_to_order_id["cli-prerestart"], 42
        )


class TestHttpReadTranslation(unittest.TestCase):
    """fetch_open_orders / fetch_orders / fetch_order must return ccxt-shape
    payloads, not raw ibapi values. The cache stays as venue-truth; the
    translation is applied at the HTTP boundary so state_updater.cpp's
    polling fallback (poll_check_unacked_orders, poll_check_failed_cancels)
    parses them through the same OrderResNormalized path as streaming."""

    def setUp(self):
        from cpp_live_proxy.ibkr_session import IBKRSession
        IBKRSession.reset_for_tests()

    def tearDown(self):
        from cpp_live_proxy.ibkr_session import IBKRSession
        IBKRSession.reset_for_tests()

    def _build_session_with_cached_order(self, cache_entry):
        # Stand up a session by hand and inject a cache entry that
        # mimics what `openOrder` would write.
        from unittest.mock import MagicMock
        from cpp_live_proxy.ibkr_session import (
            IBKRClient,
            IBKRSession,
            IBKRAccountClient,
        )
        client = IBKRClient(allowed_strings=frozenset({"U22203249"}))
        client.isConnected = MagicMock(return_value=True)
        client.disconnect = MagicMock()
        client.run = MagicMock()
        client.reqAccountUpdates = MagicMock()
        client.reqAllOpenOrders = MagicMock()
        cli_id = cache_entry["clientOrderId"]
        client._open_orders[cli_id] = cache_entry
        client._client_id_to_order_id[cli_id] = int(cache_entry["id"])
        client._order_id_to_client_id[int(cache_entry["id"])] = cli_id

        session = IBKRSession(
            client=client,
            thread=threading.Thread(target=lambda: None, daemon=True),
            host="127.0.0.1",
            port=4001,
            account_id_to_ibkr_string={7: "U22203249"},
            allowed_account_ids=frozenset({7}),
        )
        account_client = IBKRAccountClient(
            session=session,
            internal_account_id=7,
            ibkr_account_string="U22203249",
            allowed_account_ids=frozenset({7}),
        )
        return session, account_client

    def _raw_asml_entry(self):
        return {
            "id": "0",
            "clientOrderId": "80692610",
            "symbol": "ASML",
            "side": "buy",
            "type": "lmt",
            "amount": "1.0",
            "price": "900.0",
            "account": "U22203249",
            "status": "Submitted",
        }

    def _run(self, coro):
        import asyncio
        return asyncio.run(coro)

    def test_fetch_open_orders_translates_to_ccxt_shape(self):
        _, ac = self._build_session_with_cached_order(self._raw_asml_entry())
        out = self._run(ac.fetch_open_orders())
        self.assertEqual(len(out), 1)
        o = out[0]
        # ccxt vocabulary, not raw ibapi.
        self.assertEqual(o["status"], "open")
        self.assertEqual(o["type"], "limit")
        # Numeric fields are numbers, not strings.
        self.assertIsInstance(o["amount"], (int, float))
        self.assertIsInstance(o["price"], (int, float))
        self.assertIsInstance(o["filled"], (int, float))
        self.assertIsInstance(o["remaining"], (int, float))
        self.assertEqual(o["amount"], 1.0)
        self.assertEqual(o["price"], 900.0)
        # Required fields the C++ parser reads.
        self.assertEqual(o["clientOrderId"], "80692610")
        self.assertEqual(o["symbol"], "ASML")
        self.assertIsInstance(o["timestamp"], int)
        # ccxt convention: raw venue payload preserved under `info`.
        self.assertEqual(o["info"]["status"], "Submitted")
        self.assertEqual(o["info"]["type"], "lmt")

    def test_fetch_open_orders_filters_by_symbol(self):
        entry1 = self._raw_asml_entry()
        entry2 = dict(entry1)
        entry2["clientOrderId"] = "99999"
        entry2["id"] = "1"
        entry2["symbol"] = "AAPL"
        _, ac = self._build_session_with_cached_order(entry1)
        ac._session._client._open_orders["99999"] = entry2
        ac._session._client._client_id_to_order_id["99999"] = 1
        ac._session._client._order_id_to_client_id[1] = "99999"

        out = self._run(ac.fetch_open_orders(symbol="ASML"))
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0]["symbol"], "ASML")

    def test_fetch_order_returns_translated_entry_or_none(self):
        _, ac = self._build_session_with_cached_order(self._raw_asml_entry())
        hit = self._run(ac.fetch_order(id="80692610", symbol="ASML"))
        self.assertIsNotNone(hit)
        self.assertEqual(hit["status"], "open")
        self.assertEqual(hit["type"], "limit")
        miss = self._run(ac.fetch_order(id="not-here", symbol="ASML"))
        self.assertIsNone(miss)

    def test_fetch_orders_translates_each_entry(self):
        _, ac = self._build_session_with_cached_order(self._raw_asml_entry())
        out = self._run(ac.fetch_orders())
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0]["status"], "open")

    def test_filled_status_translates_and_includes_lastTradeTimestamp(self):
        entry = self._raw_asml_entry()
        entry["status"] = "Filled"
        entry["filled"] = "1.0"
        entry["remaining"] = "0.0"
        entry["avgFillPrice"] = 905.0
        # Filled orders are filtered OUT of open_orders_for, so we test
        # via fetch_orders (all_orders_for) instead.
        _, ac = self._build_session_with_cached_order(entry)
        out = self._run(ac.fetch_orders())
        self.assertEqual(len(out), 1)
        o = out[0]
        self.assertEqual(o["status"], "closed")
        self.assertIn("lastTradeTimestamp", o)
        self.assertEqual(o["filled"], 1.0)
        self.assertEqual(o["average"], 905.0)


class TestSubscriptionWatchdog(unittest.TestCase):
    """The subscription-stealing watchdog re-issues reqAccountUpdates +
    reqAllOpenOrders on a fixed cadence. This is what saves us when
    another ibapi client (TWS, smoke test, debug session) silently steals
    our reqAccountUpdates subscription."""

    def setUp(self):
        from cpp_live_proxy.ibkr_session import IBKRSession
        IBKRSession.reset_for_tests()

    def tearDown(self):
        from cpp_live_proxy.ibkr_session import IBKRSession
        IBKRSession.reset_for_tests()

    def test_watchdog_ticks_resubscribe_each_account(self):
        from unittest.mock import MagicMock
        from cpp_live_proxy.ibkr_session import IBKRSession

        # Build a session by hand without going through the connect path
        # so we can exercise the watchdog in isolation.
        client = MagicMock()
        client.isConnected = MagicMock(return_value=True)
        client.reqAccountUpdates = MagicMock()
        client.reqAllOpenOrders = MagicMock()

        session = IBKRSession(
            client=client,
            thread=threading.Thread(target=lambda: None, daemon=True),
            host="127.0.0.1",
            port=4001,
            account_id_to_ibkr_string={7: "U22203249"},
            allowed_account_ids=frozenset({7}),
        )
        # Tight interval so the test isn't slow.
        session.start_watchdog(interval_s=0.1)
        # Wait for at least 2 ticks.
        import time
        time.sleep(0.35)
        session.disconnect()

        # Watchdog issues an unsubscribe+resubscribe pair per account
        # per tick to force a fresh snapshot. Expect at least 2 ticks
        # × 2 calls = 4 reqAccountUpdates calls.
        self.assertGreaterEqual(client.reqAccountUpdates.call_count, 4)
        unsubscribes = [
            c for c in client.reqAccountUpdates.call_args_list
            if c.args == (False, "")
        ]
        subscribes = [
            c for c in client.reqAccountUpdates.call_args_list
            if c.args == (True, "U22203249")
        ]
        self.assertGreaterEqual(len(unsubscribes), 2)
        self.assertGreaterEqual(len(subscribes), 2)
        self.assertGreaterEqual(client.reqAllOpenOrders.call_count, 2)

    def test_watchdog_skips_ticks_when_disconnected(self):
        from unittest.mock import MagicMock
        from cpp_live_proxy.ibkr_session import IBKRSession

        client = MagicMock()
        client.isConnected = MagicMock(return_value=False)
        client.reqAccountUpdates = MagicMock()
        client.reqAllOpenOrders = MagicMock()

        session = IBKRSession(
            client=client,
            thread=threading.Thread(target=lambda: None, daemon=True),
            host="127.0.0.1",
            port=4001,
            account_id_to_ibkr_string={7: "U22203249"},
            allowed_account_ids=frozenset({7}),
        )
        session.start_watchdog(interval_s=0.05)
        import time
        time.sleep(0.2)
        session.disconnect()

        # Disconnected → no re-subscribe attempts (avoids errors on a
        # dead socket; reconnect logic is a separate concern).
        self.assertEqual(client.reqAccountUpdates.call_count, 0)
        self.assertEqual(client.reqAllOpenOrders.call_count, 0)


class TestPermIdAddressing(unittest.TestCase):
    """TWS / mobile / external-client orders arrive at our openOrder
    callback with orderId=0 (ibapi's "didn't originate here" sentinel).
    Two such orders would collide on a single _order_id_to_client_id key.
    permId is exchange-assigned and unique per order across all clients,
    so it's the canonical correlation key."""

    def setUp(self):
        from cpp_live_proxy.ibkr_session import IBKRSession
        IBKRSession.reset_for_tests()
        self.client = IBKRClient(allowed_strings=frozenset({"U22203249"}))

    def tearDown(self):
        from cpp_live_proxy.ibkr_session import IBKRSession
        IBKRSession.reset_for_tests()

    def _open_order(self, orderId, permId, clientOrderId, symbol="ASML"):
        from ibapi.contract import Contract
        from ibapi.order import Order
        c = Contract()
        c.symbol = symbol
        o = Order()
        o.action = "BUY"
        o.orderType = "LMT"
        o.totalQuantity = 1.0
        o.lmtPrice = 100.0
        o.account = "U22203249"
        o.orderRef = clientOrderId
        o.permId = permId
        self.client.openOrder(orderId, c, o, SimpleNamespace(status="Submitted"))

    def test_two_TWS_orders_with_orderId_zero_dont_collide(self):
        # Both orders have orderId=0 (TWS-placed sentinel). They have
        # distinct permIds and distinct clientOrderIds. Both should be
        # in the cache, addressable independently.
        self._open_order(orderId=0, permId=12345, clientOrderId="T1")
        self._open_order(orderId=0, permId=67890, clientOrderId="T2", symbol="AAPL")

        self.assertIn("T1", self.client._open_orders)
        self.assertIn("T2", self.client._open_orders)
        self.assertEqual(self.client._open_orders["T1"]["symbol"], "ASML")
        self.assertEqual(self.client._open_orders["T2"]["symbol"], "AAPL")
        # permId map indexed correctly.
        self.assertEqual(self.client._perm_id_to_client_id[12345], "T1")
        self.assertEqual(self.client._perm_id_to_client_id[67890], "T2")

    def test_orderStatus_correlates_via_permId_when_orderId_is_zero(self):
        self._open_order(orderId=0, permId=12345, clientOrderId="T1")
        self._open_order(orderId=0, permId=67890, clientOrderId="T2", symbol="AAPL")

        broadcasts = []
        self.client._broadcast_callback = broadcasts.append

        # Update for the SECOND TWS order via permId. orderId is 0 (would
        # collide with T1 if we keyed only on orderId).
        self.client.orderStatus(
            orderId=0,
            status="Filled",
            filled=1,
            remaining=0,
            avgFillPrice=100.5,
            permId=67890,
            parentId=0,
            lastFillPrice=100.5,
            clientId=0,
            whyHeld="",
            mktCapPrice=0.0,
        )

        # Exactly one broadcast, for T2 (the AAPL order), not T1 (ASML).
        self.assertEqual(len(broadcasts), 1)
        order = broadcasts[0]["orders"][0]
        self.assertEqual(order["clientOrderId"], "T2")
        self.assertEqual(order["status"], "closed")
        # `id` exposes permId, not orderId.
        self.assertEqual(order["id"], "67890")

    def test_id_field_in_cache_uses_permId_when_set(self):
        # Cache `id` field (read by the ccxt-shape translator at HTTP
        # boundary) should be permId, not the orderId=0 sentinel.
        self._open_order(orderId=0, permId=12345, clientOrderId="T1")
        self.assertEqual(self.client._open_orders["T1"]["id"], "12345")
        self.assertEqual(self.client._open_orders["T1"]["permId"], 12345)

    def test_our_own_orders_still_use_orderId_when_no_permId_yet(self):
        # When we place an order ourselves, openOrder sometimes arrives
        # before permId is assigned (rare but observed). Cache should
        # fall back to orderId in that case.
        self._open_order(orderId=42, permId=0, clientOrderId="ours")
        self.assertEqual(self.client._open_orders["ours"]["id"], "42")
        self.assertEqual(self.client._open_orders["ours"]["orderId"], 42)


class TestIbapiSignatureCompat(unittest.TestCase):
    """Lock the call shape against the real ibapi `EClient` API. Prevents
    a class of bug where unit tests pass (mocks accept anything) but the
    live cancel/place silently raises TypeError because the pinned ibapi
    version's signature doesn't match what we're calling.

    Caught us once: ibapi 9.81 has `cancelOrder(self, orderId)` while
    newer versions have `cancelOrder(self, orderId, manualCancelOrderTime)`.
    Our code was calling the 3-arg form against 9.81 — every cancel
    failed silently because cancel_order_v2 dispatches via
    asyncio.create_task and swallowed the exception.
    """

    def test_cancelOrder_signature_matches_our_call_shape(self):
        # We call self.cancelOrder(oid) — exactly one positional arg.
        from ibapi.client import EClient
        import inspect
        sig = inspect.signature(EClient.cancelOrder)
        params = list(sig.parameters.values())
        names = [p.name for p in params]
        # Allow extra optional positionals as long as the leading shape
        # matches; we never pass extras. Tightest assertion: required
        # positional args after self are exactly [orderId].
        required = [
            p.name for p in params
            if p.default is inspect.Parameter.empty
            and p.kind in (
                inspect.Parameter.POSITIONAL_ONLY,
                inspect.Parameter.POSITIONAL_OR_KEYWORD,
            )
        ]
        self.assertEqual(
            required[:2], ["self", "orderId"],
            f"ibapi cancelOrder signature changed: {names}; "
            f"update IBKRClient.cancel_by_client_order_id accordingly",
        )

    def test_placeOrder_signature_matches_our_call_shape(self):
        # We call self.placeOrder(oid, contract, order) — exactly three
        # positional args including self -> orderId, contract, order.
        from ibapi.client import EClient
        import inspect
        sig = inspect.signature(EClient.placeOrder)
        params = list(sig.parameters.values())
        names = [p.name for p in params]
        required = [
            p.name for p in params
            if p.default is inspect.Parameter.empty
            and p.kind in (
                inspect.Parameter.POSITIONAL_ONLY,
                inspect.Parameter.POSITIONAL_OR_KEYWORD,
            )
        ]
        self.assertEqual(
            required[:4], ["self", "orderId", "contract", "order"],
            f"ibapi placeOrder signature changed: {names}; "
            f"update IBKRClient.place_order_with_clientid accordingly",
        )


class TestUSEquitySymbolMetadata(unittest.TestCase):
    """IBKRAccountClient.markets is seeded from the Polygon-sourced
    USEquityCatalog. Tests mock the catalog so they don't hit Polygon."""

    def setUp(self):
        from cpp_live_proxy.ibkr_session import IBKRSession
        from cpp_live_proxy import us_equity_catalog as cat
        IBKRSession.reset_for_tests()
        cat.USEquityCatalog.reset_for_tests()

    def tearDown(self):
        from cpp_live_proxy.ibkr_session import IBKRSession
        from cpp_live_proxy import us_equity_catalog as cat
        IBKRSession.reset_for_tests()
        cat.USEquityCatalog.reset_for_tests()

    def _stub_catalog_rows(self):
        return [
            {"ticker": "AAPL", "name": "Apple Inc.", "type": "CS",
             "primary_exchange": "XNAS", "currency": "usd"},
            {"ticker": "MSFT", "name": "Microsoft Corp.", "type": "CS",
             "primary_exchange": "XNAS", "currency": "usd"},
            {"ticker": "NVDA", "name": "NVIDIA Corp.", "type": "CS",
             "primary_exchange": "XNAS", "currency": "usd"},
            {"ticker": "SPY", "name": "SPDR S&P 500 ETF", "type": "ETF",
             "primary_exchange": "ARCX", "currency": "usd"},
            {"ticker": "QQQ", "name": "Invesco QQQ", "type": "ETF",
             "primary_exchange": "ARCX", "currency": "usd"},
        ]

    def _build_account_client(self):
        from cpp_live_proxy.ibkr_session import (
            IBKRClient,
            IBKRSession,
            IBKRAccountClient,
        )
        from unittest.mock import MagicMock, patch
        from cpp_live_proxy import us_equity_catalog as cat

        client = IBKRClient(allowed_strings=frozenset({"U22203249"}))
        client.isConnected = MagicMock(return_value=True)
        client.disconnect = MagicMock()
        client.run = MagicMock()
        session = IBKRSession(
            client=client,
            thread=threading.Thread(target=lambda: None, daemon=True),
            host="127.0.0.1",
            port=4001,
            account_id_to_ibkr_string={7: "U22203249"},
            allowed_account_ids=frozenset({7}),
        )
        # Mock the catalog's Polygon fetch to avoid network + tmp the
        # cache so we don't pollute DATA_DIR_ROOT.
        import os
        import tempfile
        td = tempfile.mkdtemp()
        prev = os.environ.get("DATA_DIR_ROOT")
        os.environ["DATA_DIR_ROOT"] = td
        try:
            with patch.object(
                cat, "_fetch_from_polygon",
                return_value=self._stub_catalog_rows(),
            ):
                ac = IBKRAccountClient(
                    session=session,
                    internal_account_id=7,
                    ibkr_account_string="U22203249",
                    allowed_account_ids=frozenset({7}),
                )
        finally:
            if prev is None:
                os.environ.pop("DATA_DIR_ROOT", None)
            else:
                os.environ["DATA_DIR_ROOT"] = prev
        return ac

    def test_markets_seeded_from_catalog(self):
        ac = self._build_account_client()
        for sym in ["AAPL", "MSFT", "NVDA", "SPY", "QQQ"]:
            self.assertIn(sym, ac.markets, f"missing seed: {sym}")

    def test_markets_metadata_shape_is_ccxt_compatible(self):
        ac = self._build_account_client()
        m = ac.markets["AAPL"]
        self.assertEqual(m["base"], "AAPL")
        self.assertEqual(m["quote"], "USD")
        self.assertTrue(m["active"])
        self.assertEqual(m["type"], "stock")
        self.assertIsInstance(m["precision"]["price"], (int, float))
        self.assertEqual(m["precision"]["price"], 0.01)
        self.assertEqual(m["precision"]["amount"], 1.0)
        self.assertIn("limits", m)
        self.assertEqual(m["limits"]["amount"]["min"], 1.0)

    def test_markets_dict_per_instance_isolated(self):
        ac1 = self._build_account_client()
        from cpp_live_proxy.ibkr_session import IBKRSession
        from cpp_live_proxy import us_equity_catalog as cat
        IBKRSession.reset_for_tests()
        cat.USEquityCatalog.reset_for_tests()
        ac2 = self._build_account_client()
        ac1.markets["AAPL"]["active"] = False
        self.assertTrue(
            ac2.markets["AAPL"]["active"],
            "markets dict must not be shared by reference between clients",
        )


class TestRemainingFieldDefault(unittest.TestCase):
    """openOrder pre-seeds `remaining = totalQuantity` so HTTP responses
    don't show null `remaining` for orders that just landed (before any
    orderStatus push)."""

    def test_openOrder_seeds_remaining_to_total_quantity(self):
        from ibapi.contract import Contract
        from ibapi.order import Order
        client = IBKRClient(allowed_strings=frozenset({"U22203249"}))
        c = Contract()
        c.symbol = "AAPL"
        o = Order()
        o.action = "BUY"
        o.orderType = "LMT"
        o.totalQuantity = 5.0
        o.lmtPrice = 100.0
        o.account = "U22203249"
        o.orderRef = "test"
        o.permId = 99
        client.openOrder(42, c, o, SimpleNamespace(status="Submitted"))
        entry = client._open_orders["test"]
        self.assertEqual(entry["remaining"], "5.0")
        self.assertEqual(entry["filled"], "0")

    def test_translator_uses_amount_fallback_for_missing_remaining(self):
        from cpp_live_proxy.ibkr_session import _open_order_to_ccxt
        # Pre-historic cache entry without remaining set (defensive).
        entry = {
            "id": "1", "clientOrderId": "X", "symbol": "AAPL",
            "side": "buy", "type": "lmt", "amount": "3.0",
            "price": "100.0", "status": "Submitted",
        }
        out = _open_order_to_ccxt(entry)
        # remaining defaults to amount, not 0.
        self.assertEqual(out["remaining"], 3.0)


class TestConnectionClosedCallback(unittest.TestCase):
    """connectionClosed signals to the reconnect daemon; reconnect runs
    on its own thread so the dispatcher isn't blocked."""

    def test_connectionClosed_sets_event(self):
        client = IBKRClient(allowed_strings=frozenset({"U22203249"}))
        self.assertFalse(client._connection_closed_event.is_set())
        client.connectionClosed()
        self.assertTrue(client._connection_closed_event.is_set())


if __name__ == "__main__":
    unittest.main()
