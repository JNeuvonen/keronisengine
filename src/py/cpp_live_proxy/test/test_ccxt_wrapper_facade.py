import unittest

from cpp_live_proxy.ccxt_wrapper import (
    CcxtClient,
    CcxtWrapper,
    ExchangeRateLimitError,
    RATE_LIMIT_COOLDOWN_S,
)


class TestCcxtWrapperFacade(unittest.TestCase):
    def setUp(self):
        self.wrapper = CcxtWrapper()
        self.original_exchange_clients = self.wrapper.exchange_clients
        self.original_round_robin_index = self.wrapper.round_robin_index
        self.original_client_key_to_last_load_markets_time = (
            self.wrapper.client_key_to_last_load_markets_time
        )
        self.wrapper.exchange_clients = {}
        self.wrapper.round_robin_index = {}
        self.wrapper.client_key_to_last_load_markets_time = {}

    def tearDown(self):
        self.wrapper.exchange_clients = self.original_exchange_clients
        self.wrapper.round_robin_index = self.original_round_robin_index
        self.wrapper.client_key_to_last_load_markets_time = (
            self.original_client_key_to_last_load_markets_time
        )

    def test_facade_exports_public_surface(self):
        self.assertIsInstance(self.wrapper, CcxtWrapper)
        self.assertIs(CcxtWrapper(), self.wrapper)
        self.assertIs(self.wrapper.RATE_LIMIT_COOLDOWN_S, RATE_LIMIT_COOLDOWN_S)
        self.assertIs(self.wrapper._rate_limit_until, CcxtWrapper._rate_limit_until)
        self.assertTrue(issubclass(CcxtWrapper, CcxtWrapper.__mro__[1]))
        self.assertTrue(hasattr(self.wrapper, "register_listener"))
        self.assertTrue(hasattr(self.wrapper, "disconnect_all_user_data_listeners"))
        self.assertTrue(hasattr(self.wrapper, "test_ip_rotation"))
        self.assertTrue(hasattr(self.wrapper, "get_exchange_symbols_info"))
        self.assertTrue(hasattr(self.wrapper, "create_order"))
        self.assertTrue(hasattr(self.wrapper, "get_inventory_state"))
        self.assertTrue(hasattr(self.wrapper, "fetch_tickers"))

    def test_ccxt_client_get_client_key_public_surface(self):
        client = object()
        wrapper_client = CcxtClient(
            client=client,
            exchange_name="binance",
            market_type="spot",
            account_id=7,
            is_public=False,
            ip_address="127.0.0.1",
        )

        self.assertIs(wrapper_client.client, client)
        self.assertEqual(
            wrapper_client.get_client_key(),
            "binance_spot_7_False",
        )
        self.assertFalse(wrapper_client.is_ready)
        self.assertEqual(wrapper_client.last_fetched_market_info, 0)
        self.assertEqual(wrapper_client.ip_address, "127.0.0.1")

    def test_exchange_rate_limit_error_stays_importable(self):
        error = ExchangeRateLimitError("binance", 429, 12)

        self.assertEqual(error.exchange_name, "binance")
        self.assertEqual(error.status, 429)
        self.assertEqual(error.retry_after_s, 12)
        self.assertIn("binance rate limited", str(error))


if __name__ == "__main__":
    unittest.main()
