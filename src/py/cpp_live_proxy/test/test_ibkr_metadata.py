import unittest

from cpp_live_proxy.ccxt_wrapper import (
    CcxtClient,
    CcxtWrapper,
    EXCHANGE_NAME_IBKR,
    MARKET_TYPE_SPOT,
)


class DummyClient:
    def __init__(self, markets):
        self.markets = markets


class TestIbkrMetadataCompatibility(unittest.TestCase):
    def setUp(self):
        self.wrapper = CcxtWrapper()
        self.original_exchange_clients = self.wrapper.exchange_clients
        self.original_round_robin_index = self.wrapper.round_robin_index
        self.wrapper.exchange_clients = {}
        self.wrapper.round_robin_index = {}

    def tearDown(self):
        self.wrapper.exchange_clients = self.original_exchange_clients
        self.wrapper.round_robin_index = self.original_round_robin_index

    def test_ibkr_stock_markets_are_exposed_via_spot_metadata_path(self):
        # IBKR is USD-quoted. Both entries should pass through; the
        # USDT-only filter no longer applies (per-venue allow-list).
        markets = {
            "AAPL": {
                "base": "AAPL",
                "quote": "USD",
                "active": True,
                "type": "stock",
                "linear": False,
                "expiry": None,
                "margin": False,
                "precision": {"price": 0.01, "amount": 1.0, "base": 1.0, "quote": 0.01},
                "limits": {
                    "amount": {"min": 1.0, "max": 1_000_000.0},
                    "price": {"min": 0.01, "max": None},
                    "cost": {"min": 1.0, "max": None},
                },
            },
            "MSFT": {
                "base": "MSFT",
                "quote": "USD",
                "active": True,
                "type": "stock",
                "linear": False,
                "expiry": None,
                "margin": False,
                "precision": {"price": 0.01, "amount": 1.0, "base": 1.0, "quote": 0.01},
                "limits": {
                    "amount": {"min": 1.0, "max": 1_000_000.0},
                    "price": {"min": 0.01, "max": None},
                    "cost": {"min": 1.0, "max": None},
                },
            },
        }
        client_wrapper = CcxtClient(
            client=DummyClient(markets),
            exchange_name=EXCHANGE_NAME_IBKR,
            market_type=MARKET_TYPE_SPOT,
            account_id=7,
            is_public=False,
        )
        client_wrapper.is_ready = True
        self.wrapper.exchange_clients = {
            f"{EXCHANGE_NAME_IBKR}_{MARKET_TYPE_SPOT}_7_False": [client_wrapper]
        }

        exchange_info = self.wrapper.get_exchange_info(EXCHANGE_NAME_IBKR, MARKET_TYPE_SPOT)
        symbols_info = self.wrapper.get_exchange_symbols_info(EXCHANGE_NAME_IBKR, MARKET_TYPE_SPOT)

        self.assertIs(exchange_info, markets)
        self.assertEqual(len(symbols_info), 2)
        symbols_by_base = {s["base"]: s for s in symbols_info}
        self.assertEqual(symbols_by_base["AAPL"]["symbol"], "AAPL")
        self.assertEqual(symbols_by_base["AAPL"]["type"], MARKET_TYPE_SPOT)
        self.assertEqual(symbols_by_base["AAPL"]["quote"], "USD")
        self.assertEqual(symbols_by_base["MSFT"]["symbol"], "MSFT")
        self.assertEqual(symbols_by_base["MSFT"]["quote"], "USD")

    def test_crypto_venue_still_filters_to_usdt_usdc(self):
        # Sanity check: per-venue allow-list still rejects non-stablecoin
        # quotes for crypto venues. EUR-quoted Binance market should not
        # pass through.
        markets = {
            "BTCEUR": {
                "base": "BTC", "quote": "EUR", "active": True, "type": "spot",
                "linear": False, "expiry": None, "margin": False,
                "precision": {"price": 0.01, "amount": 0.001,
                              "base": 1e-06, "quote": 1e-06},
                "limits": {"amount": {"min": 0.001, "max": 100},
                           "price": {"min": 0.01, "max": None},
                           "cost": {"min": 1, "max": None}},
            },
            "BTCUSDT": {
                "base": "BTC", "quote": "USDT", "active": True, "type": "spot",
                "linear": False, "expiry": None, "margin": False,
                "precision": {"price": 0.01, "amount": 0.001,
                              "base": 1e-06, "quote": 1e-06},
                "limits": {"amount": {"min": 0.001, "max": 100},
                           "price": {"min": 0.01, "max": None},
                           "cost": {"min": 1, "max": None}},
            },
        }
        client_wrapper = CcxtClient(
            client=DummyClient(markets),
            exchange_name="binance",
            market_type=MARKET_TYPE_SPOT,
            account_id=1,
            is_public=False,
        )
        client_wrapper.is_ready = True
        self.wrapper.exchange_clients = {
            f"binance_{MARKET_TYPE_SPOT}_1_False": [client_wrapper]
        }
        symbols_info = self.wrapper.get_exchange_symbols_info(
            "binance", MARKET_TYPE_SPOT
        )
        # Only BTCUSDT survives.
        self.assertEqual(len(symbols_info), 1)
        self.assertEqual(symbols_info[0]["base"], "BTC")
        self.assertEqual(symbols_info[0]["quote"], "USDT")


if __name__ == "__main__":
    unittest.main()
