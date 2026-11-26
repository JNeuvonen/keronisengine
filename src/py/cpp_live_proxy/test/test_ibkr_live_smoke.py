"""Live IBKR smoke test against the running IB Gateway — gated by IBKR_LIVE=1.

NO ORDER PLACEMENT. NO CANCELS. Read-only verification that the ibapi
session connects, the bidirectional invariant fires, and account-scoped
caches are populated.

Run with:
    GOOGLE_APPLICATION_CREDENTIALS=/path/to/key.json \\
    IBKR_LIVE=1 python -m pytest \\
      src/py/apps/cpp_live_proxy/test/test_ibkr_live_smoke.py -v -s
"""

import os
import unittest

from cpp_live_proxy.ccxt_constants import (
    IBKR_ALLOWED_ACCOUNT_IDS,
    IBKR_GATEWAY_HOST,
    IBKR_GATEWAY_PORT_LIVE,
)
from cpp_live_proxy.ibkr_session import IBKRAccountClient, IBKRSession


@unittest.skipUnless(
    os.environ.get("IBKR_LIVE") == "1",
    "IBKR_LIVE=1 required to run live IBKR smoke (needs running IB Gateway)",
)
class TestIbkrLiveSmoke(unittest.TestCase):
    ALLOWED_INTERNAL_ID = 7
    ALLOWED_IBKR_STRING = "U22203249"

    @classmethod
    def setUpClass(cls):
        IBKRSession.reset_for_tests()
        cls.session = IBKRSession.create_and_connect(
            host=IBKR_GATEWAY_HOST,
            port=IBKR_GATEWAY_PORT_LIVE,
            account_id_to_ibkr_string={
                cls.ALLOWED_INTERNAL_ID: cls.ALLOWED_IBKR_STRING
            },
            allowed_account_ids=IBKR_ALLOWED_ACCOUNT_IDS,
            client_id=43,
        )
        cls.client = IBKRAccountClient(
            session=cls.session,
            internal_account_id=cls.ALLOWED_INTERNAL_ID,
            ibkr_account_string=cls.ALLOWED_IBKR_STRING,
            allowed_account_ids=IBKR_ALLOWED_ACCOUNT_IDS,
        )

    @classmethod
    def tearDownClass(cls):
        s = IBKRSession.get_instance()
        if s is not None:
            s.disconnect()
        IBKRSession.reset_for_tests()

    def test_session_is_connected(self):
        self.assertTrue(self.session.is_connected())

    def test_disallowed_internal_id_construction_refused(self):
        with self.assertRaises(PermissionError):
            IBKRAccountClient(
                session=self.session,
                internal_account_id=99,
                ibkr_account_string=self.ALLOWED_IBKR_STRING,
                allowed_account_ids=IBKR_ALLOWED_ACCOUNT_IDS,
            )

    def test_session_refuses_disallowed_account_string(self):
        with self.assertRaises(PermissionError):
            self.session.positions("U7924231")
        with self.assertRaises(PermissionError):
            self.session.account_summary("U7924231")

    def test_positions_for_allowed_account_returns_list(self):
        positions = self.session.positions(self.ALLOWED_IBKR_STRING)
        self.assertIsInstance(positions, list)
        for p in positions:
            self.assertIn("symbol", p)
            self.assertIn("position", p)

    def test_balance_for_allowed_account_returns_summary(self):
        rows = self.session.account_summary(self.ALLOWED_IBKR_STRING)
        self.assertIsInstance(rows, list)
        cash_rows = [r for r in rows if r["tag"] == "TotalCashValue"]
        self.assertTrue(cash_rows, f"no TotalCashValue tag in summary: {rows}")
        self.assertGreater(float(cash_rows[0]["value"]), 0.0)


@unittest.skipUnless(
    os.environ.get("IBKR_LIVE") == "1",
    "IBKR_LIVE=1 required",
)
class TestIbkrBootInvariants(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        s = IBKRSession.get_instance()
        if s is not None:
            s.disconnect()
        IBKRSession.reset_for_tests()

    def tearDown(self):
        s = IBKRSession.get_instance()
        if s is not None:
            s.disconnect()
        IBKRSession.reset_for_tests()

    def test_drift_between_config_and_allowlist_raises(self):
        with self.assertRaises(RuntimeError) as ctx:
            IBKRSession.create_and_connect(
                host=IBKR_GATEWAY_HOST,
                port=IBKR_GATEWAY_PORT_LIVE,
                account_id_to_ibkr_string={7: "U22203249", 99: "U99999999"},
                allowed_account_ids=IBKR_ALLOWED_ACCOUNT_IDS,
                client_id=44,
            )
        self.assertIn("config drift", str(ctx.exception))


if __name__ == "__main__":
    unittest.main()
