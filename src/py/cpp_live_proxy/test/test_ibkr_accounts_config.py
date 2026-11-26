import json
import os
import tempfile
import threading
import unittest
from pathlib import Path
from unittest.mock import MagicMock, patch

from cpp_live_proxy import ccxt_constants
from cpp_live_proxy.ccxt_constants import IBKR_ALLOWED_ACCOUNT_IDS
from cpp_live_proxy.ccxt_wrapper import (
    CcxtWrapper,
    EXCHANGE_NAME_IBKR,
    MARKET_TYPE_SPOT,
)
from cpp_live_proxy.ibkr_session import IBKRClient, IBKRSession


def _stub_client(allowed_strings, managed):
    c = IBKRClient(allowed_strings=frozenset(allowed_strings))
    c.connect = MagicMock()
    c.disconnect = MagicMock()
    c.isConnected = MagicMock(return_value=True)
    c.run = MagicMock()
    c._next_order_id = 1
    c._ready_event.set()
    c._managed_accounts = list(managed)
    c._managed_accounts_event.set()
    def _fake_req_positions():
        c.positionEnd()
    c.reqPositions = MagicMock(side_effect=_fake_req_positions)

    def _fake_req_account_updates(subscribe, account):
        if subscribe and account in allowed_strings:
            c.accountDownloadEnd(account)
    c.reqAccountUpdates = MagicMock(side_effect=_fake_req_account_updates)

    def _fake_req_open_orders():
        c.openOrderEnd()
    c.reqOpenOrders = MagicMock(side_effect=_fake_req_open_orders)
    c.reqAllOpenOrders = MagicMock(side_effect=_fake_req_open_orders)
    return c


def _patch_build(client):
    t = threading.Thread(target=lambda: None, daemon=True)
    t.start(); t.join()
    return patch.object(
        IBKRSession,
        "_build_and_connect",
        classmethod(lambda cls, *a, **kw: (client, t)),
    )


class TestIbkrAccountsConfigShape(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        IBKRSession.reset_for_tests()
        # _init_ibkr_session gates on SPAWN_PRIVATE_STREAMS — the public
        # worker forwards IBKR traffic instead of opening its own session.
        # In tests we always want the init path to run.
        self._prev_spawn = ccxt_constants.SPAWN_PRIVATE_STREAMS
        ccxt_constants.SPAWN_PRIVATE_STREAMS = True

    def tearDown(self):
        IBKRSession.reset_for_tests()
        ccxt_constants.SPAWN_PRIVATE_STREAMS = self._prev_spawn

    async def test_init_ibkr_session_registers_allowlisted_account(self):
        wrapper = CcxtWrapper()
        original = wrapper.exchange_clients
        wrapper.exchange_clients = {}

        accounts_payload = {
            "ibkr_spot_accounts": [
                {"account_id": 7, "ibkr_account": "U22203249"}
            ]
        }
        with tempfile.TemporaryDirectory() as tmp_dir:
            accounts_path = Path(tmp_dir) / "accounts.json"
            accounts_path.write_text(json.dumps(accounts_payload))
            os.environ["ACCOUNTS_JSON_PATH"] = str(accounts_path)

            client = _stub_client(["U22203249"], ["U22203249", "U7924231"])
            with _patch_build(client):
                await wrapper._init_ibkr_session(use_testnet=False)

        private_key = f"{EXCHANGE_NAME_IBKR}_{MARKET_TYPE_SPOT}_7_False"
        self.assertIn(private_key, wrapper.exchange_clients)
        self.assertEqual(len(wrapper.exchange_clients[private_key]), 1)
        ibkr_client = wrapper.exchange_clients[private_key][0].client
        self.assertEqual(ibkr_client.account_string, "U22203249")
        self.assertEqual(ibkr_client.internal_account_id, 7)

        wrapper.exchange_clients = original

    async def test_legacy_schema_with_access_secret_raises(self):
        wrapper = CcxtWrapper()
        original = wrapper.exchange_clients
        wrapper.exchange_clients = {}

        legacy_payload = {
            "ibkr_spot_accounts": [
                {"account_id": 7, "access": "k", "secret": "s"}
            ]
        }
        with tempfile.TemporaryDirectory() as tmp_dir:
            accounts_path = Path(tmp_dir) / "accounts.json"
            accounts_path.write_text(json.dumps(legacy_payload))
            os.environ["ACCOUNTS_JSON_PATH"] = str(accounts_path)

            with self.assertRaises(RuntimeError) as ctx:
                await wrapper._init_ibkr_session(use_testnet=False)
            self.assertIn("ibkr_account", str(ctx.exception))

        wrapper.exchange_clients = original

    async def test_account_id_drift_from_allowlist_raises(self):
        wrapper = CcxtWrapper()
        original = wrapper.exchange_clients
        wrapper.exchange_clients = {}

        drift_payload = {
            "ibkr_spot_accounts": [
                {"account_id": 99, "ibkr_account": "U22203249"}
            ]
        }
        with tempfile.TemporaryDirectory() as tmp_dir:
            accounts_path = Path(tmp_dir) / "accounts.json"
            accounts_path.write_text(json.dumps(drift_payload))
            os.environ["ACCOUNTS_JSON_PATH"] = str(accounts_path)

            client = _stub_client(["U22203249"], ["U22203249"])
            with _patch_build(client):
                with self.assertRaises(RuntimeError) as ctx:
                    await wrapper._init_ibkr_session(use_testnet=False)
            self.assertIn("config drift", str(ctx.exception))

        wrapper.exchange_clients = original

    async def test_mapped_account_missing_from_gateway_raises(self):
        wrapper = CcxtWrapper()
        original = wrapper.exchange_clients
        wrapper.exchange_clients = {}

        accounts_payload = {
            "ibkr_spot_accounts": [
                {"account_id": 7, "ibkr_account": "U22203249"}
            ]
        }
        with tempfile.TemporaryDirectory() as tmp_dir:
            accounts_path = Path(tmp_dir) / "accounts.json"
            accounts_path.write_text(json.dumps(accounts_payload))
            os.environ["ACCOUNTS_JSON_PATH"] = str(accounts_path)

            client = _stub_client(["U22203249"], ["U99999999"])
            with _patch_build(client):
                with self.assertRaises(RuntimeError) as ctx:
                    await wrapper._init_ibkr_session(use_testnet=False)
            self.assertIn("not present in gateway", str(ctx.exception))

        wrapper.exchange_clients = original

    def test_allowlist_is_canonical_value(self):
        self.assertEqual(IBKR_ALLOWED_ACCOUNT_IDS, frozenset({7}))


if __name__ == "__main__":
    unittest.main()
