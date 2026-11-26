from builtins import hasattr
from typing import Optional

from cpp_live_proxy.ccxt_clients import create_exchange_client as _create_exchange_client
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
    MARKET_TYPE_SPOT,
    MARKET_TYPE_SWAP,
    MARKET_TYPES,
    PRIVATE_ENDPOINTS_IP_POOL,
    PUBLIC_ENDPOINTS_IP_POOL,
    SPAWN_PRIVATE_STREAMS,
    STREAM_EVENT_TYPE_ORDER_UPDATE,
    USE_IP_ROTATION,
    USE_PROXY,
)
from cpp_live_proxy.ccxt_helpers import (
    NoThrottle,
    convert_interval_str_to_ms,
    get_client_key as _get_client_key,
)
from cpp_live_proxy.ccxt_wrapper_init import CcxtWrapperInitMixin
from cpp_live_proxy.ccxt_wrapper_inventory import CcxtWrapperInventoryMixin
from cpp_live_proxy.ccxt_wrapper_market_data import (
    CcxtWrapperMarketDataMixin,
    ExchangeRateLimitError,
)
from cpp_live_proxy.ccxt_wrapper_metadata import CcxtWrapperMetadataMixin
from cpp_live_proxy.ccxt_wrapper_orders import CcxtWrapperOrdersMixin
from cpp_live_proxy.ccxt_wrapper_user_data import CcxtWrapperUserDataMixin
from cpp_live_proxy.ccxt_wrapper_core import (
    CcxtClient,
    CcxtWrapperCore,
    RATE_LIMIT_COOLDOWN_S,
    _rate_limit_until,
)


class CcxtWrapper(
    CcxtWrapperCore,
    CcxtWrapperInitMixin,
    CcxtWrapperMetadataMixin,
    CcxtWrapperOrdersMixin,
    CcxtWrapperUserDataMixin,
    CcxtWrapperInventoryMixin,
    CcxtWrapperMarketDataMixin,
):
    async def test_ip_rotation(
        self,
        exchange_name: str,
        market_type: str,
        account_id: Optional[int],
        is_public: bool,
    ):
        try:
            client = self._round_robin_rotate_exchange_client(
                exchange_name, market_type, account_id, is_public
            )
            return await client.fetch("https://httpbin.org/ip")
        except Exception as e:
            prefix = (
                "Failed to get client"
                if "No client found" in str(e)
                else "Error fetching from client"
            )
            return f"{prefix}: {e}"

    async def update_swaps_default_leverage_level(
        self,
        exchange_name: str,
        market_type: str,
        account_id: int,
        symbol: str,
        leverage_level: int,
    ):
        try:
            client = self._round_robin_rotate_exchange_client(
                exchange_name, market_type, account_id, False
            )
            return await client.set_leverage(leverage=leverage_level, symbol=symbol)
        except Exception:
            return None
