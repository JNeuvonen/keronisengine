import unittest
from unittest.mock import AsyncMock

from cpp_live_proxy.ccxt_wrapper import (
    CcxtWrapper,
    EXCHANGE_NAME_IBKR,
    MARKET_TYPE_SPOT,
)
from cpp_live_proxy.type_models import Order


class TestIbkrOrderLifecycle(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.wrapper = CcxtWrapper()
        self.client = AsyncMock()
        self.original_rotate = self.wrapper._round_robin_rotate_exchange_client
        self.original_target = self.wrapper._get_target_client_from_order
        self.wrapper._round_robin_rotate_exchange_client = lambda *args, **kwargs: self.client
        self.wrapper._get_target_client_from_order = lambda order: self.client

    def tearDown(self):
        self.wrapper._round_robin_rotate_exchange_client = self.original_rotate
        self.wrapper._get_target_client_from_order = self.original_target

    def _make_order(self):
        return Order(
            type="limit",
            side="buy",
            status="open",
            symbol="AAPL",
            exchange_name=EXCHANGE_NAME_IBKR,
            market_type=MARKET_TYPE_SPOT,
            quote_asset="USDT",
            base_asset="AAPL",
            create_timestamp=0,
            expires_on_ms=0,
            account_id=7,
            price="190.5",
            quantity="3",
            filled_quantity="0",
            sizing_coeff_delta="0",
            client_order_id="ibkr-client-1",
            exchange_order_id="",
            strategy_id=11,
        )

    async def test_create_order_uses_standard_client_order_id_for_ibkr(self):
        order = self._make_order()

        await self.wrapper.create_order(order)

        self.client.create_order.assert_awaited_once_with(
            symbol="AAPL",
            type="limit",
            side="buy",
            amount="3",
            price="190.5",
            params={"clientOrderId": "ibkr-client-1"},
        )

    async def test_cancel_order_uses_ibkr_client_order_id_param(self):
        order = self._make_order()

        await self.wrapper.cancel_order(order)

        self.client.cancel_order.assert_awaited_once_with(
            None,
            "AAPL",
            params={"clientOrderId": "ibkr-client-1"},
        )

    async def test_cancel_order_v2_uses_ibkr_client_order_id_param(self):
        await self.wrapper.cancel_order_v2(
            client_order_id="ibkr-client-2",
            exchange_name=EXCHANGE_NAME_IBKR,
            market_type=MARKET_TYPE_SPOT,
            symbol="MSFT",
            account_id=7,
        )

        self.client.cancel_order.assert_awaited_once_with(
            None,
            "MSFT",
            params={"clientOrderId": "ibkr-client-2"},
        )

    async def test_query_order_uses_ibkr_client_order_id_param(self):
        self.client.fetch_order.return_value = {"id": "venue-1"}

        result = await self.wrapper.query_order(
            symbol="AAPL",
            exchange_name=EXCHANGE_NAME_IBKR,
            market_type=MARKET_TYPE_SPOT,
            account_id=7,
            client_order_id="ibkr-client-3",
        )

        self.client.fetch_order.assert_awaited_once_with(
            "ibkr-client-3",
            "AAPL",
            params={"clientOrderId": "ibkr-client-3"},
        )
        self.assertEqual(result, {"id": "venue-1"})

    async def test_orders_on_symbol_and_open_orders_use_standard_fetches(self):
        self.client.fetch_orders.return_value = [{"id": "history"}]
        self.client.fetch_open_orders.return_value = [{"id": "open"}]

        orders = await self.wrapper.get_orders_on_symbol(
            EXCHANGE_NAME_IBKR, MARKET_TYPE_SPOT, 7, "AAPL"
        )
        open_orders = await self.wrapper.get_all_open_orders(
            "AAPL", EXCHANGE_NAME_IBKR, MARKET_TYPE_SPOT, 7
        )

        self.client.fetch_orders.assert_awaited_once_with(symbol="AAPL", limit=1000)
        self.client.fetch_open_orders.assert_awaited_once_with(symbol="AAPL")
        self.assertEqual(orders, [{"id": "history"}])
        self.assertEqual(open_orders, [{"id": "open"}])


class TestIbkrInventoryParsing(unittest.TestCase):
    def test_ibkr_inventory_state_parses_cash_and_positions(self):
        wrapper = CcxtWrapper()
        inventory_state = {
            "info": [
                {"currency": "USD", "cash": "1200.50", "netAsset": "1200.50"},
                {"symbol": "AAPL", "position": "4", "free": "4"},
                {"symbol": "MSFT", "position": "-1", "free": "-1", "used": "0"},
            ]
        }

        parsed = wrapper._parse_inventory_state(
            EXCHANGE_NAME_IBKR,
            MARKET_TYPE_SPOT,
            inventory_state,
        )

        self.assertEqual(
            parsed["quote_assets"],
            [
                {
                    "asset": "USD",
                    "quantity": "1200.50",
                    "free": "1200.50",
                    "used": "0.0",
                    "price": 1,
                }
            ],
        )
        self.assertEqual(
            parsed["inventory"],
            [
                {
                    "total": "4",
                    "used": "0",
                    "free": "4",
                    "debt": "0",
                    "used_margin": "0",
                    "maint_margin": "0",
                    "asset": "AAPL",
                    "quote_conversion_symbol": "AAPLUSDT",
                    "exchange_name": EXCHANGE_NAME_IBKR,
                    "market_type": MARKET_TYPE_SPOT,
                },
                {
                    "total": "-1",
                    "used": "0",
                    "free": "-1",
                    "debt": "0",
                    "used_margin": "0",
                    "maint_margin": "0",
                    "asset": "MSFT",
                    "quote_conversion_symbol": "MSFTUSDT",
                    "exchange_name": EXCHANGE_NAME_IBKR,
                    "market_type": MARKET_TYPE_SPOT,
                },
            ],
        )


if __name__ == "__main__":
    unittest.main()
