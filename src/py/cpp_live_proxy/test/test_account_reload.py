import asyncio
import json

import pytest

import cpp_live_proxy.ccxt_wrapper as ccxt_wrapper_module
import cpp_live_proxy.ccxt_wrapper_orders as orders_module
from cpp_live_proxy.ccxt_wrapper import CcxtWrapper


class FakeWsClient:
    def __init__(self):
        self.closed = False

    async def close(self):
        self.closed = True


class FakeExchange:
    def __init__(self, api_key):
        self.api_key = api_key
        self.closed = False
        self.loaded = False
        self.catalog = None
        self.options = {"future": {"listenKey": "dead", "lastAuthenticatedTime": 1}}
        self.clients = {"wss://fstream/ws/dead": FakeWsClient()}

    async def load_markets(self, reload=False):
        self.loaded = True

    def set_markets(self, markets, currencies):
        self.catalog = (markets, currencies)

    async def close(self):
        self.closed = True


@pytest.fixture
def wrapper(tmp_path, monkeypatch):
    accounts_path = tmp_path / "accounts.json"
    monkeypatch.setenv("ACCOUNTS_JSON_PATH", str(accounts_path))
    monkeypatch.setattr(ccxt_wrapper_module, "PRIVATE_ENDPOINTS_IP_POOL", ["", "10.0.0.2"])
    monkeypatch.setattr(orders_module, "SPAWN_PRIVATE_STREAMS", True)
    monkeypatch.setattr("cpp_live_proxy.ccxt_wrapper_init.asyncio.sleep", _no_sleep)

    created = []

    def create(exchange, api_key, secret, **kwargs):
        if api_key == "bad":
            raise ValueError("bad key")
        client = FakeExchange(api_key)
        created.append(client)
        return client

    monkeypatch.setattr(ccxt_wrapper_module, "_create_exchange_client", create)

    w = CcxtWrapper()
    saved = {name: getattr(w, name) for name in _STATE}
    for name in _STATE:
        setattr(w, name, {} if name != "_gateio_catalog" else None)

    async def stream_forever(*args):
        await asyncio.Event().wait()

    monkeypatch.setattr(w, "handle_orders", stream_forever, raising=False)
    monkeypatch.setattr(w, "handle_positions", stream_forever, raising=False)
    w.created = created
    w.write_accounts = lambda doc: accounts_path.write_text(json.dumps(doc))
    yield w
    for name, value in saved.items():
        setattr(w, name, value)


_STATE = [
    "exchange_clients",
    "round_robin_index",
    "_loaded_private_accounts",
    "_failed_private_accounts",
    "_private_stream_tasks",
    "_private_stream_status",
    "_hyperliquid_direct_creds",
    "_gateio_catalog",
]

KEY = "binance_swap_50000_False"


async def _no_sleep(_seconds):
    return None


def binance(access="a1", secret="s1", account_id=50000):
    return {"account_id": account_id, "access": access, "secret": secret}


def run(coro):
    return asyncio.new_event_loop().run_until_complete(coro)


def test_new_account_gets_clients_and_streams(wrapper):
    async def scenario():
        wrapper.write_accounts({"binance_futures_accounts": [binance()]})
        await wrapper.reload_private_accounts(use_testnet=False)
        clients = wrapper.exchange_clients[KEY]
        assert [c.ip_address for c in clients] == ["", "10.0.0.2"]
        assert [c.is_ready for c in clients] == [True, False]
        tasks = wrapper._private_stream_tasks[KEY]
        assert len(tasks) == 2 and not any(t.done() for t in tasks)
        for t in tasks:
            t.cancel()

    run(scenario())


def test_unchanged_file_rebuilds_nothing(wrapper):
    async def scenario():
        wrapper.write_accounts({"binance_futures_accounts": [binance()]})
        await wrapper.reload_private_accounts(use_testnet=False)
        before = list(wrapper.exchange_clients[KEY])
        await wrapper.reload_private_accounts(use_testnet=False)
        assert wrapper.exchange_clients[KEY] == before
        assert len(wrapper.created) == 2
        for t in wrapper._private_stream_tasks[KEY]:
            t.cancel()

    run(scenario())


def test_changed_key_swaps_clients_and_restarts_streams(wrapper):
    async def scenario():
        wrapper.write_accounts({"binance_futures_accounts": [binance()]})
        await wrapper.reload_private_accounts(use_testnet=False)
        old_clients = [c.client for c in wrapper.exchange_clients[KEY]]
        old_tasks = wrapper._private_stream_tasks[KEY]

        wrapper.write_accounts({"binance_futures_accounts": [binance(access="a2", secret="s2")]})
        await wrapper.reload_private_accounts(use_testnet=False)
        await asyncio.gather(*old_tasks, return_exceptions=True)

        assert [c.client.api_key for c in wrapper.exchange_clients[KEY]] == ["a2", "a2"]
        assert all(c.closed for c in old_clients)
        assert all(t.cancelled() for t in old_tasks)
        new_tasks = wrapper._private_stream_tasks[KEY]
        assert not any(t.done() for t in new_tasks)
        for t in new_tasks:
            t.cancel()

    run(scenario())


def test_removed_account_is_closed_and_dropped(wrapper):
    async def scenario():
        wrapper.write_accounts({"binance_futures_accounts": [binance()]})
        await wrapper.reload_private_accounts(use_testnet=False)
        clients = [c.client for c in wrapper.exchange_clients[KEY]]
        tasks = wrapper._private_stream_tasks[KEY]

        wrapper.write_accounts({"binance_futures_accounts": []})
        await wrapper.reload_private_accounts(use_testnet=False)
        await asyncio.gather(*tasks, return_exceptions=True)

        assert KEY not in wrapper.exchange_clients
        assert KEY not in wrapper._private_stream_tasks
        assert wrapper._loaded_private_accounts == {}
        assert all(c.closed for c in clients)
        assert all(t.cancelled() for t in tasks)

    run(scenario())


def test_unreadable_file_keeps_current_accounts(wrapper, tmp_path):
    async def scenario():
        wrapper.write_accounts({"binance_futures_accounts": [binance()]})
        await wrapper.reload_private_accounts(use_testnet=False)
        before = list(wrapper.exchange_clients[KEY])

        (tmp_path / "accounts.json").write_text('{"binance_futures_accounts": [')
        with pytest.raises(json.JSONDecodeError):
            await wrapper.reload_private_accounts(use_testnet=False)
        assert wrapper.exchange_clients[KEY] == before
        for t in wrapper._private_stream_tasks[KEY]:
            t.cancel()

    run(scenario())


def test_failed_account_retries_only_after_its_entry_changes(wrapper):
    async def scenario():
        wrapper.write_accounts({"binance_futures_accounts": [binance(access="bad")]})
        await wrapper.reload_private_accounts(use_testnet=False)
        assert KEY not in wrapper.exchange_clients
        assert ("binance_futures_accounts", 50000) in wrapper._failed_private_accounts

        await wrapper.reload_private_accounts(use_testnet=False)
        assert wrapper.created == []

        wrapper.write_accounts({"binance_futures_accounts": [binance(access="good")]})
        await wrapper.reload_private_accounts(use_testnet=False)
        assert len(wrapper.exchange_clients[KEY]) == 2
        assert wrapper._failed_private_accounts == {}
        for t in wrapper._private_stream_tasks[KEY]:
            t.cancel()

    run(scenario())


def test_gateio_account_uses_shared_catalog(wrapper):
    async def scenario():
        wrapper._gateio_catalog = ({"BTC/USDT": {}}, {"USDT": {}})
        wrapper.write_accounts({"gateio_spot_accounts": [binance(account_id=7)]})
        await wrapper.reload_private_accounts(use_testnet=False)
        clients = wrapper.exchange_clients["gateio_spot_7_False"]
        assert all(c.is_ready for c in clients)
        assert all(c.client.catalog == wrapper._gateio_catalog for c in clients)
        assert not any(c.client.loaded for c in clients)
        for t in wrapper._private_stream_tasks["gateio_spot_7_False"]:
            t.cancel()

    run(scenario())


def test_hyperliquid_creds_follow_the_account(wrapper):
    async def scenario():
        entry = {"account_id": 9, "access": "0xabc", "secret": "k"}
        wrapper.write_accounts({"hyperliquid_futures_accounts": [entry]})
        await wrapper.reload_private_accounts(use_testnet=False)
        assert wrapper._hyperliquid_direct_creds["9"] == {
            "account_address": "0xabc",
            "private_key": "k",
        }
        wrapper.write_accounts({"hyperliquid_futures_accounts": []})
        await wrapper.reload_private_accounts(use_testnet=False)
        assert "9" not in wrapper._hyperliquid_direct_creds

    run(scenario())


def test_stream_restart_takes_a_new_listen_key_and_socket(wrapper):
    async def scenario():
        wrapper.write_accounts({"binance_futures_accounts": [binance()]})
        await wrapper.reload_private_accounts(use_testnet=False)
        client = wrapper.exchange_clients[KEY][0].client
        dead_socket = client.clients["wss://fstream/ws/dead"]
        old_tasks = wrapper._private_stream_tasks[KEY]

        assert await wrapper.restart_private_streams_for_account("binance", "swap", 50000)
        await asyncio.gather(*old_tasks, return_exceptions=True)

        assert client.options["future"] == {"listenKey": None, "lastAuthenticatedTime": 0}
        assert dead_socket.closed and client.clients == {}
        assert not client.closed
        assert all(t.cancelled() for t in old_tasks)
        new_tasks = wrapper._private_stream_tasks[KEY]
        assert not any(t.done() for t in new_tasks)
        assert wrapper._private_stream_status[KEY]["restarts"] == 1
        assert not await wrapper.restart_private_streams_for_account("binance", "swap", 1)
        for t in new_tasks:
            t.cancel()

    run(scenario())
