import unittest
from cpp_live_proxy.ccxt_wrapper import CcxtWrapper
from keronis_utils.file import File


class TestCcxtWrapper(unittest.IsolatedAsyncioTestCase):
    async def test_balance_fetch(self):
        wrapper = CcxtWrapper()
        await wrapper.init_accounts()
        balance = await wrapper.get_inventory_state("binance", "swap", 1)
        File.dump_as_json_to_file(balance, "binance_swap_balance.json")

    async def test_orders_fetch(self):
        pass
        # wrapper = CcxtWrapper()
        # await wrapper.init_accounts()
        # orders = await wrapper.get_orders_on_symbol("binance", "swap", 1, "SOL/USDT")
        # File.dump_as_json_to_file(orders, "binance_swap_orders.json")


if __name__ == "__main__":
    unittest.main()
