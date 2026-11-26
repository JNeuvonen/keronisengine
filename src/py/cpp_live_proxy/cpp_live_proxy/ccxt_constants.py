import os
import random

EXCHANGE_NAME_BINANCE = "binance"
EXCHANGE_NAME_BYBIT = "bybit"
EXCHANGE_NAME_BITGET = "bitget"
EXCHANGE_NAME_GATEIO = "gateio"
EXCHANGE_NAME_HYPERLIQUID = "hyperliquid"
EXCHANGE_NAME_OKX = "okx"
EXCHANGE_NAME_COINBASE = "coinbase"
EXCHANGE_NAME_ASTER = "aster"
EXCHANGE_NAME_IBKR = "ibkr"

_ibkr_ids_env = os.getenv("IBKR_ALLOWED_ACCOUNT_IDS", "7")
IBKR_ALLOWED_ACCOUNT_IDS = frozenset(
    int(x.strip()) for x in _ibkr_ids_env.split(",") if x.strip()
)

IBKR_GATEWAY_HOST = os.getenv("IBKR_GATEWAY_HOST", "127.0.0.1")
IBKR_GATEWAY_PORT_LIVE = 4001
IBKR_GATEWAY_PORT_PAPER = 4002

MARKET_TYPE_SWAP = "swap"
MARKET_TYPE_SPOT = "spot"
MARKET_TYPES = [MARKET_TYPE_SWAP, MARKET_TYPE_SPOT]

ORDER_TYPE_LIMIT = "limit"
ORDER_TYPE_MARKET = "market"
ORDER_TYPE_LIMIT_MAKER = "limit_maker"
ORDER_TYPE_FILL_OR_KILL = "fill_or_kill"
ORDER_TYPE_IMMEDIATE_OR_CANCEL = "immediate_or_cancel"

STREAM_EVENT_TYPE_ORDER_UPDATE = "order_update"
STREAM_EVENT_TYPE_BALANCE_UPDATE = "position_update"
STREAM_EVENT_TYPE_ORDER_CREATE_FAILED = "order_create_failed"
STREAM_EVENT_TYPE_ORDER_CANCEL_FAILED = "order_cancel_failed"

SPAWN_PRIVATE_STREAMS = os.getenv("SPAWN_PRIVATE_STREAMS", "0") == "1"
USE_PROXY = os.getenv("USE_PROXY", "0") == "1"
USE_IP_ROTATION = os.getenv("USE_IP_ROTATION", "0") == "1"

PRIVATE_ENDPOINTS_IP_POOL = [
    ip.strip()
    for ip in os.getenv("PRIVATE_ENDPOINTS_IP_POOL", "").split(",")
    if ip.strip()
]
PUBLIC_ENDPOINTS_IP_POOL = [
    ip.strip()
    for ip in os.getenv("PUBLIC_ENDPOINTS_IP_POOL", "").split(",")
    if ip.strip()
]

# Reload ccxt market metadata periodically so tick/step changes don't go stale.
MARKET_LOAD_INTERVAL_SECONDS = random.randint(20 * 60, 40 * 60)

if USE_IP_ROTATION and PRIVATE_ENDPOINTS_IP_POOL and PUBLIC_ENDPOINTS_IP_POOL:
    random.shuffle(PRIVATE_ENDPOINTS_IP_POOL)
    random.shuffle(PUBLIC_ENDPOINTS_IP_POOL)
else:
    PRIVATE_ENDPOINTS_IP_POOL = [""]
    PUBLIC_ENDPOINTS_IP_POOL = [""]
