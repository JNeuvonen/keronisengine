import asyncio

import cpp_live_proxy.ccxt_wrapper_init as init_module
from cpp_live_proxy.ccxt_wrapper import CcxtWrapper


class LeverageClient:
    """Binance with a 5x sub-account cap; DOGE allows at most 3x."""

    def __init__(self, current):
        self.current = current
        self.posts = []

    async def fetch_leverages(self):
        return {s: {"longLeverage": v, "shortLeverage": v} for s, v in self.current.items()}

    async def set_leverage(self, symbol, leverage):
        self.posts.append((symbol, leverage))
        if leverage > 5:
            raise Exception(
                'binance {"code":-4421,"msg":"Subaccounts are restricted from '
                'using leverage greater than 5x."}'
            )
        if symbol == "DOGE/USDT:USDT" and leverage > 3:
            raise Exception(f'binance {{"code":-4028,"msg":"Leverage {leverage} is not valid"}}')
        self.current[symbol] = leverage


def run(coro):
    return asyncio.new_event_loop().run_until_complete(coro)


def test_subaccount_cap_is_learned_once_and_symbol_caps_stay_local(monkeypatch):
    monkeypatch.setattr(init_module, "LEVERAGE_SET_THROTTLE_S", 0)
    client = LeverageClient(
        {"BTC/USDT:USDT": 1, "DOGE/USDT:USDT": 1, "ETH/USDT:USDT": 1, "SOL/USDT:USDT": 5}
    )
    run(CcxtWrapper().set_leverages_for_binance_swap_markets(client))

    assert client.posts == [
        ("BTC/USDT:USDT", 20),
        ("BTC/USDT:USDT", 5),
        ("DOGE/USDT:USDT", 5),
        ("DOGE/USDT:USDT", 4),
        ("DOGE/USDT:USDT", 3),
        ("ETH/USDT:USDT", 5),
    ]
    assert client.current == {
        "BTC/USDT:USDT": 5, "DOGE/USDT:USDT": 3, "ETH/USDT:USDT": 5, "SOL/USDT:USDT": 5
    }


class Wrapper:
    exchange_name = "binance"
    market_type = "swap"
    account_id = 50000
    is_ready = True

    def __init__(self, client, read_only=False):
        self.client = client
        self.read_only = read_only


def test_each_account_gets_one_job_per_interval_and_a_new_one_on_rebuild(monkeypatch):
    started = []

    async def fake_set(self, client):
        started.append(client)
        await asyncio.Event().wait()

    monkeypatch.setattr(CcxtWrapper, "set_leverages_for_binance_swap_markets", fake_set)

    async def scenario():
        w = CcxtWrapper()
        saved = w.exchange_clients, w._leverage_tasks
        try:
            first, second = object(), object()
            w.exchange_clients = {"binance_swap_50000_False": [Wrapper(first)]}
            w._leverage_tasks = {}
            w._start_leverage_jobs(0)
            w._start_leverage_jobs(10)
            await asyncio.sleep(0)
            assert started == [first]

            w.exchange_clients = {"binance_swap_50000_False": [Wrapper(second)]}
            w._start_leverage_jobs(20)
            await asyncio.sleep(0)
            assert started == [first, second]

            w.exchange_clients = {
                "binance_swap_50000_False": [Wrapper(second)],
                "binance_swap_50001_False": [Wrapper(first)],
            }
            w._start_leverage_jobs(30)
            await asyncio.sleep(0)
            assert started == [first, second, first]
            for task, _, _ in w._leverage_tasks.values():
                task.cancel()
        finally:
            w.exchange_clients, w._leverage_tasks = saved

    run(scenario())


def test_a_read_only_account_gets_no_leverage_job(monkeypatch):
    started = []

    async def fake_set(self, client):
        started.append(client)

    monkeypatch.setattr(CcxtWrapper, "set_leverages_for_binance_swap_markets", fake_set)

    async def scenario():
        w = CcxtWrapper()
        saved = w.exchange_clients, w._leverage_tasks
        try:
            w.exchange_clients = {"binance_swap_50000_False": [Wrapper(object(), read_only=True)]}
            w._leverage_tasks = {}
            w._start_leverage_jobs(0)
            await asyncio.sleep(0)
            assert started == [] and w._leverage_tasks == {}
        finally:
            w.exchange_clients, w._leverage_tasks = saved

    run(scenario())
