"""Unit tests for the IBKR session — ibapi backend, mocked end-to-end.

We never instantiate a real IBKRClient socket. Each test patches the
connection helper to return a stub IBKRClient with manually-driven state
so we can exercise the public surface of IBKRSession / IBKRAccountClient
without a gateway.
"""

import threading
import unittest
from types import SimpleNamespace
from unittest.mock import MagicMock, patch

from cpp_live_proxy import ibkr_session as ibkr_session_module
from cpp_live_proxy.ibkr_session import (
    IBKRAccountClient,
    IBKRClient,
    IBKRSession,
)


def _make_stub_client(allowed_strings, managed_accounts):
    """Construct a real IBKRClient with no socket, ready to drive
    callbacks manually. ibapi send-side methods are mocked."""
    client = IBKRClient(allowed_strings=frozenset(allowed_strings))
    client.connect = MagicMock()
    client.disconnect = MagicMock()
    client.isConnected = MagicMock(return_value=True)
    client.run = MagicMock()
    client._next_order_id = 1
    client._ready_event.set()
    client._managed_accounts = list(managed_accounts)
    client._managed_accounts_event.set()
    # reqPositions / reqAccountUpdates / reqOpenOrders all return
    # immediately on a real gateway and the snapshot-done callbacks fire
    # asynchronously. In tests we synchronously fire them from the stub
    # send so the boot-path waiters proceed without timing out.
    def _fake_req_positions():
        client.positionEnd()
    client.reqPositions = MagicMock(side_effect=_fake_req_positions)

    def _fake_req_account_updates(subscribe, account):
        if subscribe and account in allowed_strings:
            client.accountDownloadEnd(account)
    client.reqAccountUpdates = MagicMock(side_effect=_fake_req_account_updates)
    client.placeOrder = MagicMock()
    client.cancelOrder = MagicMock()
    client.reqContractDetails = MagicMock()

    def _fake_req_open_orders():
        client.openOrderEnd()
    # Both variants exist on the real client; reqAllOpenOrders is the one
    # the boot path uses (covers orders placed by other clientIds).
    client.reqOpenOrders = MagicMock(side_effect=_fake_req_open_orders)
    client.reqAllOpenOrders = MagicMock(side_effect=_fake_req_open_orders)
    return client


def _patch_build_and_connect(client):
    """Patch IBKRSession._build_and_connect to return our stub client +
    a finished no-op thread."""
    thread = threading.Thread(target=lambda: None, daemon=True)
    thread.start()
    thread.join()
    return patch.object(
        IBKRSession,
        "_build_and_connect",
        classmethod(lambda cls, *a, **kw: (client, thread)),
    )


class TestIBKRSessionBoot(unittest.TestCase):
    def setUp(self):
        IBKRSession.reset_for_tests()

    def tearDown(self):
        IBKRSession.reset_for_tests()

    def test_connects_when_config_matches_allowlist_and_gateway(self):
        client = _make_stub_client(["U22203249"], ["U22203249"])
        with _patch_build_and_connect(client):
            s = IBKRSession.create_and_connect(
                host="127.0.0.1",
                port=4001,
                account_id_to_ibkr_string={7: "U22203249"},
                allowed_account_ids=frozenset({7}),
            )
        self.assertTrue(s.is_connected())
        self.assertEqual(s.port, 4001)
        client.reqPositions.assert_called_once()
        client.reqAccountUpdates.assert_called_once_with(True, "U22203249")

    def test_drift_between_config_and_allowlist_raises(self):
        client = _make_stub_client(["U22203249"], ["U22203249"])
        with _patch_build_and_connect(client):
            with self.assertRaises(RuntimeError) as ctx:
                IBKRSession.create_and_connect(
                    host="127.0.0.1",
                    port=4001,
                    account_id_to_ibkr_string={7: "U22203249"},
                    allowed_account_ids=frozenset({7, 8}),
                )
            self.assertIn("config drift", str(ctx.exception))

    def test_mapped_string_missing_from_gateway_raises(self):
        client = _make_stub_client(["U22203249"], ["U99999999"])
        with _patch_build_and_connect(client):
            with self.assertRaises(RuntimeError) as ctx:
                IBKRSession.create_and_connect(
                    host="127.0.0.1",
                    port=4001,
                    account_id_to_ibkr_string={7: "U22203249"},
                    allowed_account_ids=frozenset({7}),
                )
            self.assertIn("not present in gateway", str(ctx.exception))

    def test_leaked_gateway_account_logs_warning_but_connects(self):
        client = _make_stub_client(["U22203249"], ["U22203249", "U7924231"])
        with _patch_build_and_connect(client), \
             patch.object(ibkr_session_module, "log") as mock_log:
            s = IBKRSession.create_and_connect(
                host="127.0.0.1",
                port=4001,
                account_id_to_ibkr_string={7: "U22203249"},
                allowed_account_ids=frozenset({7}),
            )
        self.assertTrue(s.is_connected())
        warning_calls = [c for c in mock_log.call_args_list if c.args[2] == "WARNING"]
        self.assertTrue(any("U7924231" in c.args[0] for c in warning_calls))

    def test_double_connect_refused(self):
        client1 = _make_stub_client(["U22203249"], ["U22203249"])
        with _patch_build_and_connect(client1):
            IBKRSession.create_and_connect(
                host="127.0.0.1",
                port=4001,
                account_id_to_ibkr_string={7: "U22203249"},
                allowed_account_ids=frozenset({7}),
            )
        client2 = _make_stub_client(["U22203249"], ["U22203249"])
        with _patch_build_and_connect(client2):
            with self.assertRaises(RuntimeError) as ctx:
                IBKRSession.create_and_connect(
                    host="127.0.0.1",
                    port=4001,
                    account_id_to_ibkr_string={7: "U22203249"},
                    allowed_account_ids=frozenset({7}),
                )
            self.assertIn("already created", str(ctx.exception))


class TestIBKRClientCallbackCachePopulation(unittest.TestCase):
    def setUp(self):
        self.client = IBKRClient(allowed_strings=frozenset({"U22203249"}))

    def _contract(self, symbol, exchange="SMART"):
        return SimpleNamespace(
            symbol=symbol, exchange=exchange, primaryExchange=""
        )

    def test_position_for_unknown_account_dropped(self):
        self.client.position(
            "U7924231", self._contract("SXRV", "IBIS2"), 187.0, 1328.86
        )
        self.assertEqual(self.client._positions, {})

    def test_position_for_allowed_account_cached(self):
        self.client.position("U22203249", self._contract("AAPL"), 10.0, 190.0)
        out = self.client.positions_for("U22203249")
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0]["symbol"], "AAPL")
        self.assertEqual(out[0]["position"], "10.0")

    def test_account_value_for_unknown_account_dropped(self):
        self.client.updateAccountValue(
            "NetLiquidation", "900000", "EUR", "U7924231"
        )
        self.assertEqual(self.client._account_values, {})

    def test_account_value_cached_and_filtered_to_allowed(self):
        self.client.updateAccountValue(
            "NetLiquidation", "50000", "EUR", "U22203249"
        )
        self.client.updateAccountValue(
            "NetLiquidation", "900000", "EUR", "U7924231"
        )
        out = self.client.account_values_for("U22203249")
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0]["value"], "50000")

    def test_disallowed_read_raises(self):
        with self.assertRaises(PermissionError):
            self.client.positions_for("U7924231")
        with self.assertRaises(PermissionError):
            self.client.account_values_for("U7924231")

    def test_qualify_contract_resolves_via_callback(self):
        from ibapi.contract import Contract as RealContract
        result_holder = {}

        def call_qualify():
            try:
                result_holder["c"] = self.client.qualify_contract("AAPL", timeout_s=5)
            except Exception as e:
                result_holder["err"] = e

        t = threading.Thread(target=call_qualify)
        t.start()
        for _ in range(50):
            with self.client._lock:
                if self.client._contract_details_futures:
                    break
            threading.Event().wait(0.02)
        with self.client._lock:
            rid = next(iter(self.client._contract_details_futures))
        qualified = RealContract()
        qualified.symbol = "AAPL"
        qualified.conId = 12345
        self.client.contractDetails(rid, SimpleNamespace(contract=qualified))
        self.client.contractDetailsEnd(rid)
        t.join(timeout=2)
        self.assertNotIn("err", result_holder)
        self.assertEqual(result_holder["c"].conId, 12345)


class TestIBKRClientPlaceOrder(unittest.TestCase):
    def setUp(self):
        self.client = IBKRClient(allowed_strings=frozenset({"U22203249"}))
        self.client._next_order_id = 100
        from ibapi.contract import Contract as RealContract
        c = RealContract()
        c.symbol = "AAPL"
        c.conId = 1
        self.client._qualified_contracts["AAPL"] = c
        self.client.placeOrder = MagicMock()

    def test_place_order_assigns_orderRef_account_and_sends(self):
        fut = self.client.place_order_with_clientid(
            account_str="U22203249",
            symbol="AAPL",
            side="buy",
            order_type="limit",
            amount="3",
            price="190.5",
            client_order_id="cli-7",
        )
        self.client.placeOrder.assert_called_once()
        oid_arg, _, order_arg = self.client.placeOrder.call_args.args
        self.assertEqual(oid_arg, 100)
        self.assertEqual(order_arg.account, "U22203249")
        self.assertEqual(order_arg.orderRef, "cli-7")
        self.assertEqual(order_arg.action, "BUY")
        self.assertEqual(order_arg.orderType, "LMT")
        self.assertEqual(order_arg.lmtPrice, 190.5)
        self.assertFalse(fut.done())

    def test_place_order_refuses_disallowed_account(self):
        with self.assertRaises(PermissionError):
            self.client.place_order_with_clientid(
                account_str="U7924231",
                symbol="AAPL",
                side="buy",
                order_type="limit",
                amount="1",
                price="100",
                client_order_id="x",
            )

    def _synthesize_open_order(self, oid, client_order_id, symbol, amount, price):
        # Simulate the openOrder callback that ibapi fires before the
        # first orderStatus. Populates the cache that orderStatus reads.
        from ibapi.contract import Contract as _C
        from ibapi.order import Order as _O
        c = _C()
        c.symbol = symbol
        o = _O()
        o.action = "BUY"
        o.orderType = "LMT"
        o.totalQuantity = amount
        o.lmtPrice = price
        o.account = "U22203249"
        o.orderRef = client_order_id
        from types import SimpleNamespace as _SN
        self.client.openOrder(oid, c, o, _SN(status="Submitted"))

    def test_orderStatus_callback_resolves_future(self):
        fut = self.client.place_order_with_clientid(
            account_str="U22203249",
            symbol="AAPL",
            side="buy",
            order_type="limit",
            amount="3",
            price="190.5",
            client_order_id="cli-9",
        )
        # openOrder fires before first orderStatus on the gateway; mirror that.
        self._synthesize_open_order(100, "cli-9", "AAPL", 3.0, 190.5)
        self.client.orderStatus(
            orderId=100,
            status="Submitted",
            filled=0,
            remaining=3,
            avgFillPrice=0.0,
            permId=0,
            parentId=0,
            lastFillPrice=0.0,
            clientId=0,
            whyHeld="",
            mktCapPrice=0.0,
        )
        result = fut.result(timeout=1)
        # ccxt-translated status; what the C++ parser expects.
        self.assertEqual(result["status"], "open")
        self.assertEqual(result["clientOrderId"], "cli-9")
        self.assertEqual(result["symbol"], "AAPL")
        self.assertEqual(result["amount"], 3.0)

    def test_orderStatus_invokes_broadcast_callback(self):
        broadcasts = []
        self.client._broadcast_callback = lambda p: broadcasts.append(p)
        self.client.place_order_with_clientid(
            account_str="U22203249",
            symbol="AAPL",
            side="buy",
            order_type="limit",
            amount="3",
            price="190.5",
            client_order_id="cli-stream",
        )
        self._synthesize_open_order(100, "cli-stream", "AAPL", 3.0, 190.5)
        self.client.orderStatus(
            orderId=100,
            status="Filled",
            filled=3,
            remaining=0,
            avgFillPrice=190.5,
            permId=0,
            parentId=0,
            lastFillPrice=190.5,
            clientId=0,
            whyHeld="",
            mktCapPrice=0.0,
        )
        self.assertEqual(len(broadcasts), 1)
        self.assertEqual(broadcasts[0]["event_type"], "order_update")
        # Filled -> closed in ccxt vocabulary.
        self.assertEqual(broadcasts[0]["orders"][0]["status"], "closed")
        self.assertEqual(broadcasts[0]["orders"][0]["filled"], 3.0)


class TestIBKRClientCancelOrder(unittest.TestCase):
    def setUp(self):
        self.client = IBKRClient(allowed_strings=frozenset({"U22203249"}))
        self.client.cancelOrder = MagicMock()

    def test_cancel_unknown_clientOrderId_raises(self):
        with self.assertRaises(LookupError):
            self.client.cancel_by_client_order_id("U22203249", "nope")

    def test_cancel_account_mismatch_refused(self):
        self.client._client_id_to_order_id["cli-x"] = 42
        self.client._open_orders["cli-x"] = {
            "id": "42",
            "clientOrderId": "cli-x",
            "account": "U7924231",
            "status": "Submitted",
        }
        with self.assertRaises(PermissionError):
            self.client.cancel_by_client_order_id("U22203249", "cli-x")

    def test_cancel_resolves_via_clientOrderId_to_order_id(self):
        self.client._client_id_to_order_id["cli-9"] = 42
        self.client._open_orders["cli-9"] = {
            "id": "42",
            "clientOrderId": "cli-9",
            "account": "U22203249",
            "status": "Submitted",
        }
        out = self.client.cancel_by_client_order_id("U22203249", "cli-9")
        self.assertEqual(out["status"], "canceling")
        # ibapi 9.81 EClient.cancelOrder takes (orderId,) only — no
        # manualCancelOrderTime arg until later versions.
        self.client.cancelOrder.assert_called_once_with(42)


class TestIBKRAccountClientGuards(unittest.TestCase):
    def setUp(self):
        IBKRSession.reset_for_tests()
        client = _make_stub_client(["U22203249"], ["U22203249"])
        with _patch_build_and_connect(client):
            self.session = IBKRSession.create_and_connect(
                host="127.0.0.1",
                port=4001,
                account_id_to_ibkr_string={7: "U22203249"},
                allowed_account_ids=frozenset({7}),
            )

    def tearDown(self):
        IBKRSession.reset_for_tests()

    def test_construction_with_disallowed_id_raises(self):
        with self.assertRaises(PermissionError):
            IBKRAccountClient(
                session=self.session,
                internal_account_id=99,
                ibkr_account_string="U22203249",
                allowed_account_ids=frozenset({7}),
            )

    def test_set_sandbox_mode_mismatch_raises(self):
        client = IBKRAccountClient(
            session=self.session,
            internal_account_id=7,
            ibkr_account_string="U22203249",
            allowed_account_ids=frozenset({7}),
        )
        with self.assertRaises(RuntimeError):
            client.set_sandbox_mode(True)
        client.set_sandbox_mode(False)


class TestIBKRAccountClientCreateOrder(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        IBKRSession.reset_for_tests()
        client = _make_stub_client(["U22203249"], ["U22203249"])
        from ibapi.contract import Contract as RealContract
        c = RealContract()
        c.symbol = "AAPL"
        c.conId = 1
        client._qualified_contracts["AAPL"] = c
        with _patch_build_and_connect(client):
            self.session = IBKRSession.create_and_connect(
                host="127.0.0.1",
                port=4001,
                account_id_to_ibkr_string={7: "U22203249"},
                allowed_account_ids=frozenset({7}),
            )
        self.client = client
        self.account_client = IBKRAccountClient(
            session=self.session,
            internal_account_id=7,
            ibkr_account_string="U22203249",
            allowed_account_ids=frozenset({7}),
        )

    def tearDown(self):
        IBKRSession.reset_for_tests()

    async def test_create_order_passes_clientOrderId_and_resolves(self):
        import asyncio

        async def fire_callback_after_send():
            for _ in range(50):
                if self.client.placeOrder.called:
                    break
                await asyncio.sleep(0.02)
            oid_arg = self.client.placeOrder.call_args.args[0]
            order_arg = self.client.placeOrder.call_args.args[2]
            contract_arg = self.client.placeOrder.call_args.args[1]
            # Synthesize the openOrder ack the gateway would send before
            # the first orderStatus.
            from types import SimpleNamespace as _SN
            self.client.openOrder(
                oid_arg, contract_arg, order_arg, _SN(status="Submitted")
            )
            self.client.orderStatus(
                orderId=oid_arg,
                status="Submitted",
                filled=0,
                remaining=3,
                avgFillPrice=0.0,
                permId=0,
                parentId=0,
                lastFillPrice=0.0,
                clientId=0,
                whyHeld="",
                mktCapPrice=0.0,
            )

        result, _ = await asyncio.gather(
            self.account_client.create_order(
                symbol="AAPL",
                type="limit",
                side="buy",
                amount="3",
                price="190.5",
                params={"clientOrderId": "cli-7"},
            ),
            fire_callback_after_send(),
        )
        self.assertEqual(result["clientOrderId"], "cli-7")
        # ccxt-translated status; what state_updater.cpp parses.
        self.assertEqual(result["status"], "open")
        order_arg = self.client.placeOrder.call_args.args[2]
        self.assertEqual(order_arg.account, "U22203249")
        self.assertEqual(order_arg.orderRef, "cli-7")

    async def test_create_order_without_clientOrderId_raises(self):
        with self.assertRaises(ValueError):
            await self.account_client.create_order(
                symbol="AAPL",
                type="limit",
                side="buy",
                amount="1",
                price="100",
                params={},
            )


if __name__ == "__main__":
    unittest.main()
