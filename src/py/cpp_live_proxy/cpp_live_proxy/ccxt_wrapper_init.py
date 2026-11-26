import asyncio
import hashlib
import json
import os
import re
import time

import ccxt.pro as ccxt_pro

from cpp_live_proxy.ccxt_constants import (
    EXCHANGE_NAME_ASTER,
    EXCHANGE_NAME_BINANCE,
    EXCHANGE_NAME_BITGET,
    EXCHANGE_NAME_BYBIT,
    EXCHANGE_NAME_COINBASE,
    EXCHANGE_NAME_GATEIO,
    EXCHANGE_NAME_HYPERLIQUID,
    EXCHANGE_NAME_IBKR,
    EXCHANGE_NAME_OKX,
    MARKET_LOAD_INTERVAL_SECONDS,
    MARKET_TYPE_SPOT,
    MARKET_TYPE_SWAP,
    MARKET_TYPES,
    SPAWN_PRIVATE_STREAMS,
)
from cpp_live_proxy.ccxt_helpers import get_client_key as _get_client_key
from cpp_live_proxy.gcp_log import log
from cpp_live_proxy._compat import get_current_time_in_ms, resolve_optional_gcp_secret


def _ccxt_wrapper_module():
    import cpp_live_proxy.ccxt_wrapper as ccxt_wrapper_module

    return ccxt_wrapper_module


def _private_endpoints_ip_pool():
    return _ccxt_wrapper_module().PRIVATE_ENDPOINTS_IP_POOL


def _public_endpoints_ip_pool():
    return _ccxt_wrapper_module().PUBLIC_ENDPOINTS_IP_POOL


def _create_exchange_client_from_wrapper(*args, **kwargs):
    return _ccxt_wrapper_module()._create_exchange_client(*args, **kwargs)


def _wrapper_hasattr(obj, name):
    return _ccxt_wrapper_module().hasattr(obj, name)


def _make_ccxt_client(*args, **kwargs):
    from cpp_live_proxy.ccxt_wrapper import CcxtClient

    return CcxtClient(*args, **kwargs)


def _resolve_account_secret(item: dict) -> str:
    """Resolve an account secret from plaintext `secret` or optional GCP Secret Manager."""
    gcp_ref = item.get("secret_gcp_secret", "")
    if gcp_ref and os.getenv("GCP_PROJECT_ID", ""):
        return resolve_optional_gcp_secret(gcp_ref)
    return item.get("secret", "")


# list name -> (exchange, market_type, ccxt default_type, ready when its market
# load fails). The last flag preserves each venue's original startup behavior.
PRIVATE_ACCOUNT_LISTS = {
    "binance_futures_accounts": (EXCHANGE_NAME_BINANCE, MARKET_TYPE_SWAP, "future", True),
    "binance_spot_accounts": (EXCHANGE_NAME_BINANCE, MARKET_TYPE_SPOT, "margin", True),
    "bybit_futures_accounts": (EXCHANGE_NAME_BYBIT, MARKET_TYPE_SWAP, "future", True),
    "bybit_spot_accounts": (EXCHANGE_NAME_BYBIT, MARKET_TYPE_SPOT, "spot", True),
    "bitget_futures_accounts": (EXCHANGE_NAME_BITGET, MARKET_TYPE_SWAP, "future", True),
    "bitget_spot_accounts": (EXCHANGE_NAME_BITGET, MARKET_TYPE_SPOT, "margin", True),
    "gateio_futures_accounts": (EXCHANGE_NAME_GATEIO, MARKET_TYPE_SWAP, "future", True),
    "gateio_spot_accounts": (EXCHANGE_NAME_GATEIO, MARKET_TYPE_SPOT, "spot", True),
    "hyperliquid_futures_accounts": (EXCHANGE_NAME_HYPERLIQUID, MARKET_TYPE_SWAP, "swap", False),
    "okx_futures_accounts": (EXCHANGE_NAME_OKX, MARKET_TYPE_SWAP, "future", False),
    "okx_spot_accounts": (EXCHANGE_NAME_OKX, MARKET_TYPE_SPOT, "spot", False),
    "coinbase_spot_accounts": (EXCHANGE_NAME_COINBASE, MARKET_TYPE_SPOT, "spot", False),
    "aster_futures_accounts": (EXCHANGE_NAME_ASTER, MARKET_TYPE_SWAP, "future", False),
}

ACCOUNTS_RELOAD_INTERVAL_S = 10
MAX_LEVERAGE = 20
LEVERAGE_SET_INTERVAL_S = 4 * 3600
# Paced well under binance's 2400/min IP weight limit.
LEVERAGE_SET_THROTTLE_S = 2
_SUBACCOUNT_CAP = re.compile(r'"code":-4421,.*?greater than (\d+)x')


def _current_leverage(entry) -> int:
    levels = [
        v for v in (entry.get("longLeverage"), entry.get("shortLeverage")) if v is not None
    ]
    return int(min(levels)) if levels else 0


def _subaccount_leverage_cap(error_text: str):
    match = _SUBACCOUNT_CAP.search(error_text)
    return int(match.group(1)) if match else None


def _read_accounts_file() -> dict:
    accounts_json_path = os.getenv("ACCOUNTS_JSON_PATH")
    if accounts_json_path is None:
        raise ValueError("ACCOUNTS_JSON_PATH is not set")
    with open(accounts_json_path) as f:
        return json.load(f)


async def _close_clients(wrappers) -> None:
    for wrapper in wrappers:
        try:
            await wrapper.client.close()
        except Exception:
            pass


def _account_fingerprint(item: dict) -> str:
    return hashlib.sha256(json.dumps(item, sort_keys=True).encode()).hexdigest()


class CcxtWrapperInitMixin:
    async def _init_binance_accounts(self, use_testnet: bool = True):
        accounts = _read_accounts_file()

        for idx, ip in enumerate(_public_endpoints_ip_pool()):
            for market_type in MARKET_TYPES:
                key = _get_client_key(EXCHANGE_NAME_BINANCE, market_type, None, True)
                if key not in self.exchange_clients:
                    self.exchange_clients[key] = []

                client = _create_exchange_client_from_wrapper(
                    exchange=EXCHANGE_NAME_BINANCE,
                    api_key="",
                    secret="",
                    default_type=(
                        "future" if market_type == MARKET_TYPE_SWAP else "spot"
                    ),
                    use_testnet=use_testnet,
                    local_ip=ip,
                    needs_api_key=False,
                )
                if idx == 0:
                    try:
                        await client.load_markets()
                        await asyncio.sleep(1)
                    except Exception as e:
                        log(
                            f"Error loading markets for public {market_type} client: {type(e).__name__}: {e}",
                            {},
                            "ERROR",
                        )

                client_wrapper = _make_ccxt_client(
                    client=client,
                    exchange_name=EXCHANGE_NAME_BINANCE,
                    market_type=market_type,
                    account_id=None,
                    is_public=True,
                    ip_address=ip,
                )

                if idx == 0:
                    client_wrapper.is_ready = True
                    client_wrapper.last_fetched_market_info = get_current_time_in_ms()
                self.exchange_clients[key].append(client_wrapper)

        await self._add_private_accounts(
            accounts, ["binance_futures_accounts", "binance_spot_accounts"], use_testnet
        )

    async def _init_bybit_accounts(self, use_testnet: bool = True):
        accounts = _read_accounts_file()

        for idx, ip in enumerate(_public_endpoints_ip_pool()):
            for market_type in MARKET_TYPES:
                key = _get_client_key(EXCHANGE_NAME_BYBIT, market_type, None, True)
                if key not in self.exchange_clients:
                    self.exchange_clients[key] = []

                client = _create_exchange_client_from_wrapper(
                    exchange=EXCHANGE_NAME_BYBIT,
                    api_key="",
                    secret="",
                    default_type=(
                        "future" if market_type == MARKET_TYPE_SWAP else "spot"
                    ),
                    use_testnet=use_testnet,
                    local_ip=ip,
                    needs_api_key=False,
                )
                if idx == 0:
                    try:
                        await client.load_markets()
                        await asyncio.sleep(1)
                    except Exception as e:
                        log(
                            f"Error loading markets for public {market_type} client: {type(e).__name__}: {e}",
                            {},
                            "ERROR",
                        )

                client_wrapper = _make_ccxt_client(
                    client=client,
                    exchange_name=EXCHANGE_NAME_BYBIT,
                    market_type=market_type,
                    account_id=None,
                    is_public=True,
                    ip_address=ip,
                )

                if idx == 0:
                    client_wrapper.is_ready = True
                    client_wrapper.last_fetched_market_info = get_current_time_in_ms()
                self.exchange_clients[key].append(client_wrapper)

        await self._add_private_accounts(
            accounts, ["bybit_futures_accounts", "bybit_spot_accounts"], use_testnet
        )

    async def _init_bitget_accounts(self, use_testnet: bool = True):
        accounts = _read_accounts_file()

        for idx, ip in enumerate(_public_endpoints_ip_pool()):
            for market_type in MARKET_TYPES:
                key = _get_client_key(EXCHANGE_NAME_BITGET, market_type, None, True)
                if key not in self.exchange_clients:
                    self.exchange_clients[key] = []

                client = _create_exchange_client_from_wrapper(
                    exchange=EXCHANGE_NAME_BITGET,
                    api_key="",
                    secret="",
                    default_type=(
                        "future" if market_type == MARKET_TYPE_SWAP else "spot"
                    ),
                    use_testnet=use_testnet,
                    local_ip=ip,
                    needs_api_key=False,
                )
                if idx == 0:
                    try:
                        await client.load_markets()
                        await asyncio.sleep(1)
                    except Exception as e:
                        log(
                            f"Error loading markets for public {market_type} client: {type(e).__name__}: {e}",
                            {},
                            "ERROR",
                        )

                client_wrapper = _make_ccxt_client(
                    client=client,
                    exchange_name=EXCHANGE_NAME_BITGET,
                    market_type=market_type,
                    account_id=None,
                    is_public=True,
                    ip_address=ip,
                )

                if idx == 0:
                    client_wrapper.is_ready = True
                    client_wrapper.last_fetched_market_info = get_current_time_in_ms()
                self.exchange_clients[key].append(client_wrapper)

        await self._add_private_accounts(
            accounts, ["bitget_futures_accounts", "bitget_spot_accounts"], use_testnet
        )

    async def _init_gateio_accounts(self, use_testnet: bool = True):
        accounts = _read_accounts_file()

        # Gateio's catalog is ~6300 markets and one `load_markets` takes
        # ~10s. The venue catalog is a property of the venue itself, not of
        # any client config, so we fetch it ONCE and hand the result to
        # every (ip × market_type × account) client via `set_markets` — a
        # synchronous in-memory copy. Without this, each fresh client
        # re-fetches the same 6300-row payload and the venue init blows
        # past the 30s per-init timeout, leaving empty client lists at
        # registry keys.
        # All workers warm up at once on restart and gate rejects some of
        # the burst; a worker that gives up never serves gateio.
        warmup_attempts = 4
        for attempt in range(1, warmup_attempts + 1):
            warmup = _create_exchange_client_from_wrapper(
                exchange=EXCHANGE_NAME_GATEIO,
                api_key="",
                secret="",
                default_type="spot",
                use_testnet=use_testnet,
                local_ip="",
                needs_api_key=False,
            )
            try:
                await warmup.load_markets()
                cached_markets = warmup.markets
                cached_currencies = warmup.currencies
                self._gateio_catalog = (cached_markets, cached_currencies)
                break
            except Exception as e:
                log(
                    f"gateio catalog warmup attempt {attempt} failed: "
                    f"{type(e).__name__}: {e}",
                    {},
                    "WARNING",
                )
                if attempt == warmup_attempts:
                    raise
            finally:
                await warmup.close()
            await asyncio.sleep(5 * attempt)

        now_ms = get_current_time_in_ms()

        def _register(client, *, market_type, account_id, is_public, ip):
            client.set_markets(cached_markets, cached_currencies)
            wrapper = _make_ccxt_client(
                client=client,
                exchange_name=EXCHANGE_NAME_GATEIO,
                market_type=market_type,
                account_id=account_id,
                is_public=is_public,
                ip_address=ip,
            )
            wrapper.is_ready = True
            wrapper.last_fetched_market_info = now_ms
            key = _get_client_key(
                EXCHANGE_NAME_GATEIO, market_type, account_id, is_public)
            self.exchange_clients.setdefault(key, []).append(wrapper)

        for ip in _public_endpoints_ip_pool():
            for market_type in MARKET_TYPES:
                client = _create_exchange_client_from_wrapper(
                    exchange=EXCHANGE_NAME_GATEIO,
                    api_key="",
                    secret="",
                    default_type=(
                        "future" if market_type == MARKET_TYPE_SWAP else "spot"
                    ),
                    use_testnet=use_testnet,
                    local_ip=ip,
                    needs_api_key=False,
                )
                _register(client, market_type=market_type, account_id=None,
                          is_public=True, ip=ip)

        await self._add_private_accounts(
            accounts, ["gateio_futures_accounts", "gateio_spot_accounts"], use_testnet
        )

    async def _init_hyperliquid_accounts(self, use_testnet: bool = True):
        accounts = _read_accounts_file()

        for idx, ip in enumerate(_public_endpoints_ip_pool()):
            key = _get_client_key(
                EXCHANGE_NAME_HYPERLIQUID, MARKET_TYPE_SWAP, None, True
            )
            if key not in self.exchange_clients:
                self.exchange_clients[key] = []

            client = _create_exchange_client_from_wrapper(
                exchange=EXCHANGE_NAME_HYPERLIQUID,
                api_key="",
                secret="",
                default_type="swap",
                use_testnet=use_testnet,
                local_ip=ip,
                needs_api_key=False,
            )
            load_success = False
            if idx == 0:
                try:
                    await client.load_markets()
                    await asyncio.sleep(1)
                    load_success = True
                except Exception as e:
                    log(
                        f"Error loading markets for hyperliquid public swap client: {type(e).__name__}: {e}",
                        {},
                        "ERROR",
                    )

            client_wrapper = _make_ccxt_client(
                client=client,
                exchange_name=EXCHANGE_NAME_HYPERLIQUID,
                market_type=MARKET_TYPE_SWAP,
                account_id=None,
                is_public=True,
                ip_address=ip,
            )

            if idx == 0 and load_success:
                client_wrapper.is_ready = True
                client_wrapper.last_fetched_market_info = get_current_time_in_ms()
            self.exchange_clients[key].append(client_wrapper)

        await self._add_private_accounts(
            accounts, ["hyperliquid_futures_accounts"], use_testnet
        )

    async def _init_okx_accounts(self, use_testnet: bool = True):
        accounts = _read_accounts_file()

        for idx, ip in enumerate(_public_endpoints_ip_pool()):
            for market_type in MARKET_TYPES:
                key = _get_client_key(EXCHANGE_NAME_OKX, market_type, None, True)
                if key not in self.exchange_clients:
                    self.exchange_clients[key] = []

                client = _create_exchange_client_from_wrapper(
                    exchange=EXCHANGE_NAME_OKX,
                    api_key="",
                    secret="",
                    default_type=(
                        "future" if market_type == MARKET_TYPE_SWAP else "spot"
                    ),
                    use_testnet=use_testnet,
                    local_ip=ip,
                    needs_api_key=False,
                )
                load_success = False
                if idx == 0:
                    try:
                        await client.load_markets()
                        await asyncio.sleep(1)
                        load_success = True
                    except Exception as e:
                        log(
                            f"Error loading markets for okx public {market_type} client: {type(e).__name__}: {e}",
                            {},
                            "ERROR",
                        )

                client_wrapper = _make_ccxt_client(
                    client=client,
                    exchange_name=EXCHANGE_NAME_OKX,
                    market_type=market_type,
                    account_id=None,
                    is_public=True,
                    ip_address=ip,
                )

                if idx == 0 and load_success:
                    client_wrapper.is_ready = True
                    client_wrapper.last_fetched_market_info = get_current_time_in_ms()
                self.exchange_clients[key].append(client_wrapper)

        await self._add_private_accounts(
            accounts, ["okx_futures_accounts", "okx_spot_accounts"], use_testnet
        )

    async def _init_coinbase_accounts(self, use_testnet: bool = True):
        accounts = _read_accounts_file()

        for idx, ip in enumerate(_public_endpoints_ip_pool()):
            for market_type in MARKET_TYPES:
                key = _get_client_key(
                    EXCHANGE_NAME_COINBASE, market_type, None, True
                )
                if key not in self.exchange_clients:
                    self.exchange_clients[key] = []

                client = _create_exchange_client_from_wrapper(
                    exchange=EXCHANGE_NAME_COINBASE,
                    api_key="",
                    secret="",
                    default_type=(
                        "future" if market_type == MARKET_TYPE_SWAP else "spot"
                    ),
                    use_testnet=use_testnet,
                    local_ip=ip,
                    needs_api_key=False,
                )
                load_success = False
                if idx == 0:
                    try:
                        await client.load_markets()
                        await asyncio.sleep(1)
                        load_success = True
                    except Exception as e:
                        log(
                            f"Error loading markets for coinbase public {market_type} client: {type(e).__name__}: {e}",
                            {},
                            "ERROR",
                        )

                client_wrapper = _make_ccxt_client(
                    client=client,
                    exchange_name=EXCHANGE_NAME_COINBASE,
                    market_type=market_type,
                    account_id=None,
                    is_public=True,
                    ip_address=ip,
                )

                if idx == 0 and load_success:
                    client_wrapper.is_ready = True
                    client_wrapper.last_fetched_market_info = get_current_time_in_ms()
                self.exchange_clients[key].append(client_wrapper)

        await self._add_private_accounts(
            accounts, ["coinbase_spot_accounts"], use_testnet
        )

    async def _init_ibkr_session(self, use_testnet: bool = True):
        """Connect to IB Gateway, validate the account_id <-> ibkr_account
        mapping against IBKR_ALLOWED_ACCOUNT_IDS, and register one
        IBKRAccountClient per allowlisted account.

        Single-instance by design: only the worker with
        SPAWN_PRIVATE_STREAMS=1 owns the IBKR session. Public-streams
        workers proxy IBKR-bearing requests to this worker via the
        forward middleware in main.py. The IB Gateway accepts only one
        TCP connection per clientId, and IBKR's stateful subscription
        + order-id allocation make per-worker connections costlier than
        useful — same shape as the crypto private-stream subscriptions.

        Bidirectional invariant: the int set in accounts.json["ibkr_spot_accounts"]
        must equal IBKR_ALLOWED_ACCOUNT_IDS. Drift on either side -> raise.
        """
        from cpp_live_proxy.ccxt_constants import (
            IBKR_ALLOWED_ACCOUNT_IDS,
            IBKR_GATEWAY_HOST,
            IBKR_GATEWAY_PORT_LIVE,
            IBKR_GATEWAY_PORT_PAPER,
            SPAWN_PRIVATE_STREAMS,
        )
        from cpp_live_proxy.ibkr_session import IBKRAccountClient, IBKRSession

        if not SPAWN_PRIVATE_STREAMS:
            log(
                "IBKR session init skipped on public-streams worker; "
                "IBKR-bearing requests forwarded to private-streams worker.",
                {},
                "INFO",
            )
            return

        accounts = _read_accounts_file()

        entries = accounts.get("ibkr_spot_accounts", [])
        if not entries:
            if IBKR_ALLOWED_ACCOUNT_IDS:
                raise RuntimeError(
                    f"IBKR_ALLOWED_ACCOUNT_IDS={set(IBKR_ALLOWED_ACCOUNT_IDS)} but "
                    f"accounts.json has no ibkr_spot_accounts entries"
                )
            log("No IBKR accounts configured; skipping IBKR session init.", {}, "INFO")
            return

        account_id_to_ibkr_string: dict = {}
        for entry in entries:
            if "ibkr_account" not in entry:
                raise RuntimeError(
                    f"IBKR accounts.json entry missing 'ibkr_account' key: {entry}. "
                    f"Schema migrated: drop 'access'/'secret', add "
                    f"'ibkr_account': '<U-string>'."
                )
            account_id_to_ibkr_string[entry["account_id"]] = entry["ibkr_account"]

        port = IBKR_GATEWAY_PORT_PAPER if use_testnet else IBKR_GATEWAY_PORT_LIVE
        # Each uvicorn process needs a distinct IBKR clientId; the gateway
        # rejects duplicate clientIds with "Error 326". Default 17 for
        # single-process; override via IBKR_CLIENT_ID for multi-process
        # local dev (set per-uvicorn-process in the launcher script).
        ibkr_client_id = int(os.getenv("IBKR_CLIENT_ID", "17"))
        # Streaming wire-up: ibapi's dispatcher thread will call this
        # callback on every orderStatus/execDetails. We schedule the
        # broadcast on the running asyncio loop via run_coroutine_threadsafe
        # so it crosses cleanly back to uvicorn's loop.
        loop = asyncio.get_running_loop()
        wrapper = self

        def _ibkr_broadcast(payload: dict) -> None:
            try:
                asyncio.run_coroutine_threadsafe(
                    wrapper.broadcast(payload), loop
                )
            except Exception as e:
                log(f"IBKR broadcast scheduling failed: {e}", {}, "ERROR")

        try:
            session = await IBKRSession.create_and_connect_async(
                host=IBKR_GATEWAY_HOST,
                port=port,
                account_id_to_ibkr_string=account_id_to_ibkr_string,
                allowed_account_ids=IBKR_ALLOWED_ACCOUNT_IDS,
                client_id=ibkr_client_id,
                broadcast_callback=_ibkr_broadcast,
            )
        except Exception as e:
            log(
                f"IBKR session init failed: {type(e).__name__}: {e}. "
                f"IBKR clients will NOT be registered.",
                {},
                "ERROR",
            )
            raise

        self._ibkr_session = session

        for internal_id, ibkr_string in account_id_to_ibkr_string.items():
            key = _get_client_key(
                EXCHANGE_NAME_IBKR, MARKET_TYPE_SPOT, internal_id, False
            )
            if key not in self.exchange_clients:
                self.exchange_clients[key] = []
            ibkr_client = IBKRAccountClient(
                session=session,
                internal_account_id=internal_id,
                ibkr_account_string=ibkr_string,
                allowed_account_ids=IBKR_ALLOWED_ACCOUNT_IDS,
            )
            client_wrapper = _make_ccxt_client(
                client=ibkr_client,
                exchange_name=EXCHANGE_NAME_IBKR,
                market_type=MARKET_TYPE_SPOT,
                account_id=internal_id,
                is_public=False,
                ip_address="",
            )
            client_wrapper.is_ready = True
            client_wrapper.last_fetched_market_info = get_current_time_in_ms()
            self.exchange_clients[key].append(client_wrapper)

    async def _init_aster_accounts(self, use_testnet: bool = True):
        if not _wrapper_hasattr(ccxt_pro, "aster"):
            log("CCXT does not support aster (upgrade ccxt for native support). Skipping aster CCXT init — using AsterWrapper for authenticated queries.", {}, "WARNING")
            return

        accounts = _read_accounts_file()

        for idx, ip in enumerate(_public_endpoints_ip_pool()):
            key = _get_client_key(
                EXCHANGE_NAME_ASTER, MARKET_TYPE_SWAP, None, True
            )
            if key not in self.exchange_clients:
                self.exchange_clients[key] = []

            client = _create_exchange_client_from_wrapper(
                exchange=EXCHANGE_NAME_ASTER,
                api_key="",
                secret="",
                default_type="future",
                use_testnet=use_testnet,
                local_ip=ip,
                needs_api_key=False,
            )
            load_success = False
            if idx == 0:
                try:
                    await client.load_markets()
                    await asyncio.sleep(1)
                    load_success = True
                except Exception as e:
                    log(
                        f"Error loading markets for aster public swap client: {type(e).__name__}: {e}",
                        {},
                        "ERROR",
                    )

            client_wrapper = _make_ccxt_client(
                client=client,
                exchange_name=EXCHANGE_NAME_ASTER,
                market_type=MARKET_TYPE_SWAP,
                account_id=None,
                is_public=True,
                ip_address=ip,
            )

            if idx == 0 and load_success:
                client_wrapper.is_ready = True
                client_wrapper.last_fetched_market_info = get_current_time_in_ms()
            self.exchange_clients[key].append(client_wrapper)

        await self._add_private_accounts(
            accounts, ["aster_futures_accounts"], use_testnet
        )

    async def _add_private_accounts(self, accounts: dict, list_names, use_testnet: bool):
        for list_name in list_names:
            for item in accounts.get(list_name, []):
                await self._set_private_account(list_name, item, use_testnet)

    async def _set_private_account(self, list_name: str, item: dict, use_testnet: bool):
        """Builds the account's clients, then swaps them in, so a key change
        never leaves the account without a client."""
        exchange, market_type, default_type, ready_without_markets = (
            PRIVATE_ACCOUNT_LISTS[list_name]
        )
        if exchange == EXCHANGE_NAME_ASTER and not _wrapper_hasattr(ccxt_pro, "aster"):
            return
        account_id = item["account_id"]
        if exchange == EXCHANGE_NAME_HYPERLIQUID:
            # access is the main wallet address; the agent key comes from GCP.
            secret = _resolve_account_secret(item)
        else:
            secret = item["secret"]
        gateio_catalog = getattr(self, "_gateio_catalog", None)

        wrappers = []
        for idx, ip in enumerate(_private_endpoints_ip_pool()):
            client = _create_exchange_client_from_wrapper(
                exchange=exchange,
                api_key=item["access"],
                secret=secret,
                default_type=default_type,
                use_testnet=use_testnet,
                local_ip=ip,
                needs_api_key=True,
                password=item.get("password", ""),
            )
            wrapper = _make_ccxt_client(
                client=client,
                exchange_name=exchange,
                market_type=market_type,
                account_id=account_id,
                is_public=False,
                ip_address=ip,
            )
            wrapper.read_only = bool(item.get("read_only", False))
            if exchange == EXCHANGE_NAME_GATEIO and gateio_catalog is not None:
                client.set_markets(*gateio_catalog)
                wrapper.is_ready = True
                wrapper.last_fetched_market_info = get_current_time_in_ms()
            elif idx == 0:
                loaded = False
                try:
                    await client.load_markets()
                    await asyncio.sleep(1)
                    loaded = True
                except Exception as e:
                    log(
                        f"Error loading markets for {list_name} account {account_id}: "
                        f"{type(e).__name__}: {e}",
                        {},
                        "ERROR",
                    )
                if loaded or ready_without_markets:
                    wrapper.is_ready = True
                    wrapper.last_fetched_market_info = get_current_time_in_ms()
            wrappers.append(wrapper)

        key = _get_client_key(exchange, market_type, account_id, False)
        replaced = self.exchange_clients.get(key, [])
        self.exchange_clients[key] = wrappers
        self.round_robin_index.pop(key, None)
        if exchange == EXCHANGE_NAME_HYPERLIQUID:
            self._hyperliquid_direct_creds[str(account_id)] = {
                "account_address": item["access"],
                "private_key": secret,
            }
        self._loaded_private_accounts[(list_name, account_id)] = _account_fingerprint(item)
        self._restart_private_streams(key, wrappers[0] if wrappers else None)
        await _close_clients(replaced)

    async def _remove_private_account(self, list_name: str, account_id):
        exchange, market_type, _, _ = PRIVATE_ACCOUNT_LISTS[list_name]
        key = _get_client_key(exchange, market_type, account_id, False)
        self._loaded_private_accounts.pop((list_name, account_id), None)
        self._restart_private_streams(key, None)
        removed = self.exchange_clients.pop(key, [])
        self.round_robin_index.pop(key, None)
        if exchange == EXCHANGE_NAME_HYPERLIQUID:
            self._hyperliquid_direct_creds.pop(str(account_id), None)
        await _close_clients(removed)

    async def reload_private_accounts(self, use_testnet: bool):
        """Applies accounts.json to the running clients: adds new accounts,
        rebuilds changed ones, drops removed ones. An entry that fails to build
        is retried only after its content changes."""
        accounts = _read_accounts_file()
        desired = {}
        for list_name in PRIVATE_ACCOUNT_LISTS:
            for item in accounts.get(list_name, []):
                desired[(list_name, item["account_id"])] = item

        for list_name, account_id in list(self._loaded_private_accounts):
            if (list_name, account_id) not in desired:
                await self._remove_private_account(list_name, account_id)
                log(f"[accounts] removed {list_name} {account_id}", {}, "INFO")

        for (list_name, account_id), item in desired.items():
            fingerprint = _account_fingerprint(item)
            if self._loaded_private_accounts.get((list_name, account_id)) == fingerprint:
                continue
            if self._failed_private_accounts.get((list_name, account_id)) == fingerprint:
                continue
            try:
                await self._set_private_account(list_name, item, use_testnet)
                self._failed_private_accounts.pop((list_name, account_id), None)
                log(f"[accounts] loaded {list_name} {account_id}", {}, "INFO")
            except Exception as e:
                self._failed_private_accounts[(list_name, account_id)] = fingerprint
                log(
                    f"[accounts] failed to load {list_name} {account_id}: {type(e).__name__}",
                    {},
                    "ERROR",
                )

    async def reload_private_accounts_forever(self, use_testnet: bool):
        while True:
            await asyncio.sleep(ACCOUNTS_RELOAD_INTERVAL_S)
            try:
                await self.reload_private_accounts(use_testnet)
            except Exception as e:
                log(f"[accounts] reload failed: {type(e).__name__}: {e}", {}, "ERROR")

    async def init_accounts(self, use_testnet: bool = True):
        # Each exchange init runs in its own task with its own timeout +
        # try/except. A hang or exception in one venue (e.g. aster
        # load_markets stalling on a network blip) MUST NOT prevent other
        # venues from initializing — each one is independent.
        per_init_timeout_s = 300
        inits = [
            ("ibkr", self._init_ibkr_session),
            ("aster", self._init_aster_accounts),
            ("binance", self._init_binance_accounts),
            ("bybit", self._init_bybit_accounts),
            ("bitget", self._init_bitget_accounts),
            ("gateio", self._init_gateio_accounts),
            ("hyperliquid", self._init_hyperliquid_accounts),
            ("okx", self._init_okx_accounts),
            ("coinbase", self._init_coinbase_accounts),
        ]

        # Some inits early-return without doing work (e.g. IBKR on the
        # public-streams worker). Those advertise via this sentinel so
        # the "ok" diagnostic line accurately reflects whether the init
        # actually opened a connection.
        from cpp_live_proxy.ccxt_constants import SPAWN_PRIVATE_STREAMS

        async def run_one(name, fn):
            import sys
            try:
                await asyncio.wait_for(fn(use_testnet), timeout=per_init_timeout_s)
                if name == "ibkr" and not SPAWN_PRIVATE_STREAMS:
                    msg = f"[init] {name}: skipped (forwarded to private worker)"
                else:
                    msg = f"[init] {name}: ok"
                print(msg, file=sys.stderr, flush=True)
                log(msg, {}, "INFO")
            except asyncio.TimeoutError:
                msg = (
                    f"[init] {name}: TIMEOUT after {per_init_timeout_s}s; "
                    f"venue clients may be partially registered"
                )
                print(msg, file=sys.stderr, flush=True)
                log(msg, {}, "ERROR")
            except Exception as e:
                import traceback
                msg = f"[init] {name}: {type(e).__name__}: {e}"
                print(msg, file=sys.stderr, flush=True)
                traceback.print_exc(file=sys.stderr)
                log(msg, {}, "ERROR")

        await asyncio.gather(*(run_one(n, f) for n, f in inits))

        asyncio.create_task(self.reload_private_accounts_forever(use_testnet))
        asyncio.create_task(self.periodic_jobs())

    async def set_leverages_for_binance_swap_markets(self, client):
        """Raises every symbol to the highest leverage the account allows, up
        to MAX_LEVERAGE. A sub-account's cap (-4421) applies to every symbol,
        so it lowers the starting level for the rest; a symbol's own lower
        maximum (-4028) does not."""
        ceiling = MAX_LEVERAGE
        leverages = await client.fetch_leverages()
        for symbol, current in leverages.items():
            if _current_leverage(current) >= ceiling:
                continue
            level = ceiling
            while level >= 1:
                await asyncio.sleep(LEVERAGE_SET_THROTTLE_S)
                try:
                    await client.set_leverage(symbol=symbol, leverage=level)
                    break
                except Exception as e:
                    cap = _subaccount_leverage_cap(str(e))
                    if cap is not None:
                        ceiling = min(cap, level - 1)
                        level = ceiling
                    else:
                        level -= 1

    def _start_leverage_jobs(self, now: float):
        for client_key, client_list in list(self.exchange_clients.items()):
            wrapper = next(
                (
                    w
                    for w in client_list
                    if w.is_ready
                    and w.exchange_name == EXCHANGE_NAME_BINANCE
                    and w.market_type == MARKET_TYPE_SWAP
                    and w.account_id is not None
                    and not w.read_only
                ),
                None,
            )
            if wrapper is None:
                continue
            job = self._leverage_tasks.get(client_key)
            if job is not None:
                task, client, started = job
                if client is wrapper.client and (
                    not task.done() or now - started < LEVERAGE_SET_INTERVAL_S
                ):
                    continue
                task.cancel()
            self._leverage_tasks[client_key] = (
                asyncio.create_task(
                    self.set_leverages_for_binance_swap_markets(wrapper.client)
                ),
                wrapper.client,
                now,
            )

    async def periodic_jobs(self):
        # Periodic maintenance: refresh market metadata caches and retry failed clients.
        while True:
            try:
                current_time = time.time()
                for client_key, client_list in list(self.exchange_clients.items()):
                    for client_wrapper in client_list:
                        client_key = client_wrapper.get_client_key()
                        if client_key not in self.client_key_to_last_load_markets_time:
                            self.client_key_to_last_load_markets_time[client_key] = 0

                        if (
                            current_time
                            - self.client_key_to_last_load_markets_time[client_key]
                            > MARKET_LOAD_INTERVAL_SECONDS
                            or client_wrapper.is_ready is False
                        ):
                            try:
                                await client_wrapper.client.load_markets(True)
                                client_wrapper.is_ready = True
                                self.client_key_to_last_load_markets_time[
                                    client_key
                                ] = current_time
                            except Exception as e:
                                self.client_key_to_last_load_markets_time[
                                    client_key
                                ] = current_time
                                import traceback
                                print(
                                    f"Failed to load markets for {client_key}: "
                                    f"{type(e).__name__}: {e}\n"
                                    f"{traceback.format_exc()}"
                                )
                            await asyncio.sleep(1)

                sleep_time = 15

                if SPAWN_PRIVATE_STREAMS:
                    self._start_leverage_jobs(current_time)
                await asyncio.sleep(sleep_time)
            except Exception as e:
                print(f"Error in periodic_jobs: {e}")
                await asyncio.sleep(15)

