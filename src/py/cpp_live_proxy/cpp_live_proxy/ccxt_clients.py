import aiohttp
import ccxt.pro as ccxt_pro

from cpp_live_proxy.audit_trace import maybe_trace_configs
from cpp_live_proxy.ccxt_constants import (
    EXCHANGE_NAME_ASTER,
    EXCHANGE_NAME_BINANCE,
    EXCHANGE_NAME_BITGET,
    EXCHANGE_NAME_BYBIT,
    EXCHANGE_NAME_COINBASE,
    EXCHANGE_NAME_GATEIO,
    EXCHANGE_NAME_HYPERLIQUID,
    EXCHANGE_NAME_OKX,
    USE_IP_ROTATION,
)
from cpp_live_proxy.ccxt_helpers import NoThrottle


def create_exchange_session(local_ip: str = "") -> aiohttp.ClientSession:
    """Always create the aiohttp session for ccxt rather than letting
    ccxt build one internally. This is the single chokepoint where the
    audit-log TraceConfig gets attached — without it, ccxt's
    internally-managed session bypasses every observability hook."""
    connector_kwargs = {"force_close": True}
    if local_ip:
        connector_kwargs["local_addr"] = (local_ip, 0)
    connector = aiohttp.TCPConnector(**connector_kwargs)
    return aiohttp.ClientSession(
        connector=connector,
        timeout=aiohttp.ClientTimeout(total=30),
        trace_configs=maybe_trace_configs(),
    )


def create_exchange_client(
    exchange: str,
    api_key: str,
    secret: str,
    default_type: str,
    use_testnet: bool = True,
    local_ip: str = "",
    needs_api_key: bool = True,
    password: str = "",
):
    if exchange == EXCHANGE_NAME_BINANCE:
        # warnOnFetchOpenOrdersWithoutSymbol: ccxt raises an error (not a real
        # warning) when fetch_open_orders is called without a symbol. We use
        # the no-symbol form intentionally for orphan-order reconciliation —
        # iterating ~600 symbols once every 5 minutes would be 600x more HTTP
        # traffic than a single weight-40 bulk call. Acknowledged.
        config = {
            "enableRateLimit": False,
            "options": {
                "defaultType": default_type,
                "warnOnFetchOpenOrdersWithoutSymbol": False,
            },
        }
        if needs_api_key:
            config["apiKey"] = api_key
            config["secret"] = secret
        # Always inject our session so the audit-log TraceConfig attaches.
        # When IP rotation is off, local_ip is "" and TCPConnector binds
        # to the default interface — same behavior as ccxt would do alone.
        config["session"] = create_exchange_session(
            local_ip if USE_IP_ROTATION else "")
        client = ccxt_pro.binance(config)
    elif exchange == EXCHANGE_NAME_BYBIT:
        config = {"enableRateLimit": False, "options": {"defaultType": default_type}}
        if needs_api_key:
            config["apiKey"] = api_key
            config["secret"] = secret
        # Always inject our session so the audit-log TraceConfig attaches.
        # When IP rotation is off, local_ip is "" and TCPConnector binds
        # to the default interface — same behavior as ccxt would do alone.
        config["session"] = create_exchange_session(
            local_ip if USE_IP_ROTATION else "")
        client = ccxt_pro.bybit(config)
    elif exchange == EXCHANGE_NAME_BITGET:
        config = {"enableRateLimit": False, "options": {"defaultType": default_type}}
        if needs_api_key:
            config["apiKey"] = api_key
            config["secret"] = secret
            config["password"] = password
        # Always inject our session so the audit-log TraceConfig attaches.
        # When IP rotation is off, local_ip is "" and TCPConnector binds
        # to the default interface — same behavior as ccxt would do alone.
        config["session"] = create_exchange_session(
            local_ip if USE_IP_ROTATION else "")
        client = ccxt_pro.bitget(config)
    elif exchange == EXCHANGE_NAME_GATEIO:
        config = {
            "enableRateLimit": False,
            "timeout": 30000,
            "options": {"defaultType": default_type},
        }
        if needs_api_key:
            config["apiKey"] = api_key
            config["secret"] = secret
        # Always inject our session so the audit-log TraceConfig attaches.
        # When IP rotation is off, local_ip is "" and TCPConnector binds
        # to the default interface — same behavior as ccxt would do alone.
        config["session"] = create_exchange_session(
            local_ip if USE_IP_ROTATION else "")
        client = ccxt_pro.gate(config)
    elif exchange == EXCHANGE_NAME_HYPERLIQUID:
        config = {
            "enableRateLimit": False,
            "timeout": 30000,
            "options": {"defaultType": default_type},
        }
        if needs_api_key:
            config["apiKey"] = api_key
            config["secret"] = secret
        # Always inject our session so the audit-log TraceConfig attaches.
        # When IP rotation is off, local_ip is "" and TCPConnector binds
        # to the default interface — same behavior as ccxt would do alone.
        config["session"] = create_exchange_session(
            local_ip if USE_IP_ROTATION else "")
        client = ccxt_pro.hyperliquid(config)

        # ccxt 4.5.2's hyperliquid.fetch_spot_markets crashes on spot tokens
        # with a null name (`base` None -> `None + '/'`), which aborts
        # load_markets() and leaves the client permanently not-ready. We trade
        # HL perps only, so skip spot-market parsing entirely.
        async def _skip_spot_markets(params={}):
            return []

        client.fetch_spot_markets = _skip_spot_markets
    elif exchange == EXCHANGE_NAME_OKX:
        config = {
            "enableRateLimit": False,
            "timeout": 30000,
            "options": {"defaultType": default_type},
        }
        if needs_api_key:
            config["apiKey"] = api_key
            config["secret"] = secret
            if password:
                config["password"] = password
        # Always inject our session so the audit-log TraceConfig attaches.
        # When IP rotation is off, local_ip is "" and TCPConnector binds
        # to the default interface — same behavior as ccxt would do alone.
        config["session"] = create_exchange_session(
            local_ip if USE_IP_ROTATION else "")
        client = ccxt_pro.okx(config)
    elif exchange == EXCHANGE_NAME_COINBASE:
        config = {
            "enableRateLimit": False,
            "timeout": 30000,
            "options": {"defaultType": default_type},
        }
        if needs_api_key:
            config["apiKey"] = api_key
            config["secret"] = secret
        # Always inject our session so the audit-log TraceConfig attaches.
        # When IP rotation is off, local_ip is "" and TCPConnector binds
        # to the default interface — same behavior as ccxt would do alone.
        config["session"] = create_exchange_session(
            local_ip if USE_IP_ROTATION else "")
        client = ccxt_pro.coinbase(config)
    elif exchange == EXCHANGE_NAME_ASTER:
        config = {
            "enableRateLimit": False,
            "timeout": 30000,
            "options": {"defaultType": default_type},
        }
        if needs_api_key:
            config["apiKey"] = api_key
            config["secret"] = secret
        # Always inject our session so the audit-log TraceConfig attaches.
        # When IP rotation is off, local_ip is "" and TCPConnector binds
        # to the default interface — same behavior as ccxt would do alone.
        config["session"] = create_exchange_session(
            local_ip if USE_IP_ROTATION else "")
        client = ccxt_pro.aster(config)
    else:
        raise ValueError(f"Unsupported exchange: {exchange}")

    if use_testnet:
        client.set_sandbox_mode(True)
    client.throttle = NoThrottle()
    return client
