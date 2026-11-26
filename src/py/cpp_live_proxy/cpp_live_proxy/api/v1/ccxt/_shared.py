import os
import time

from cpp_live_proxy.aster_wrapper import ASTER_BASE_URL as ASTER_URL
from cpp_live_proxy.backends import get_backend
from cpp_live_proxy.ccxt_wrapper import CcxtWrapper
from cpp_live_proxy.polymarket_streamer import PolymarketStreamer
from cpp_live_proxy.polymarket_wrapper import PolymarketWrapper
from cpp_live_proxy.type_models import Order

ccxt_wrapper = CcxtWrapper()
polymarket_wrapper = PolymarketWrapper()
polymarket_streamers: dict[int, PolymarketStreamer] = {}

PREVENT_ORDER_CREATION = str(os.getenv("PREVENT_ORDER_CREATION", "0")) == "1"
ASTER_MOCK_CREDENTIALS = os.getenv("ASTER_MOCK_CREDENTIALS", "0") == "1"
HYPERLIQUID_MOCK_CREDENTIALS = os.getenv("HYPERLIQUID_MOCK_CREDENTIALS", "0") == "1"
POLYMARKET_MOCK_CREDENTIALS = os.getenv("POLYMARKET_MOCK_CREDENTIALS", "0") == "1"

BACKEND = get_backend()

_MOCK_ASTER_CREDENTIALS = {
    "accounts": {
        "1": {
            "private_key": "0x" + "01" * 32,
            "user_address": "0x" + "01" * 20,
            "signer_address": "0x" + "02" * 20,
        }
    }
}

_MOCK_HYPERLIQUID_CREDENTIALS = {
    "accounts": {
        "1": {
            "private_key": "0x" + "01" * 32,
            "account_address": "0x" + "01" * 20,
        }
    }
}

_MOCK_POLYMARKET_CREDENTIALS = {
    "accounts": {
        "1": {
            "private_key": "0x" + "01" * 32,
            "api_key": "00000000-0000-0000-0000-000000000001",
            "api_secret": "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=",
            "api_passphrase": "test-passphrase",
            "maker_address": "0x" + "01" * 20,
            "signer_address": "0x" + "02" * 20,
            "builder_api_key": "00000000-0000-0000-0000-000000000002",
            "builder_secret": "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB=",
            "builder_passphrase": "test-builder-passphrase",
        }
    }
}


def _is_order_expired(order: Order) -> bool:
    now_ms = int(time.time() * 1000)
    if order.server_sending_deadline_ms <= 0:
        print(f"Difference: {now_ms - order.server_sending_deadline_ms}")
        return False
    print(f"Difference: {now_ms - order.server_sending_deadline_ms}")
    return now_ms > order.server_sending_deadline_ms


def set_polymarket_streamer(streamer: PolymarketStreamer, account_id: int = 0):
    global polymarket_streamers
    polymarket_streamers[account_id] = streamer


class Routes:
    CREATE_ORDER = "/create-order"
    CREATE_ORDERS_BULK = "/create-orders-bulk"
    CANCEL_ORDER = "/cancel-order"
    CANCEL_ORDERS_BULK = "/cancel-orders-bulk"
    CANCEL_ORDER_V2 = "/cancel-order-v2"
    EXCHANGE_INFO = "/exchange-info"
    GET_INVENTORY_STATE = "/get-inventory-state"
    GET_ORDERS_ON_SYMBOL = "/orders-on-symbol"
    GET_FULL_ORDERBOOK = "/full-orderbook"
    REPAY_LOAN = "/repay-loan"
    OPEN_ORDERS = "/open-orders"
    QUERY_ORDER = "/query-order"
    GET_SYMBOLS = "/get-symbols"
    GET_ACTIVE_SYMBOLS = "/get-active-symbols"
    FETCH_PRICES = "/fetch-prices"
    GET_SERVER_TIME = "/server-time"
    TEST_IP_ROTATION = "/test-ip-rotation"
    FETCH_BORROWABLE_INVENTORIES = "/borrowable-inventories"
    UPDATE_SWAPS_DEFAULT_LEVERAGE_LEVEL = "/swaps-default-leverage-level"
    QUERY_SWAP_SYMBOL_CONFIGURATION = "/query-swap-symbol-configuration"
    MINT_CONTRACTS = "/mint-contracts"
    MERGE_CONTRACTS = "/merge-contracts"
    REDEEM_WINNINGS = "/redeem-winnings"
    POLYMARKET_POSITIONS = "/polymarket-positions"
    EOA_BALANCES = "/eoa-balances"
    RESOLVE_MARKET = "/resolve-market"
    POLYMARKET_STREAM_SUBSCRIBE = "/polymarket-stream/subscribe"
    POLYMARKET_ALL_OPEN_ORDERS = "/polymarket-all-open-orders"
    GET_CREDENTIALS = "/get-credentials"
    GET_ASTER_CREDENTIALS = "/get-aster-credentials"
    GET_HYPERLIQUID_CREDENTIALS = "/get-hyperliquid-credentials"
