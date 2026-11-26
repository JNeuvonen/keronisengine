from typing import Dict, Optional

import ccxt.pro as ccxt_pro

from cpp_live_proxy.ccxt_helpers import get_client_key as _get_client_key

RATE_LIMIT_COOLDOWN_S = 60
_rate_limit_until: Dict[str, float] = {}


class CcxtClient:
    client: ccxt_pro.Exchange
    last_fetched_market_info: int
    is_ready: bool
    ip_address: Optional[str]
    exchange_name: str
    market_type: str
    account_id: Optional[int]
    is_public: bool

    def __init__(
        self,
        client: ccxt_pro.Exchange,
        exchange_name: str,
        market_type: str,
        account_id: Optional[int],
        is_public: bool,
        ip_address: Optional[str] = None,
    ):
        self.client = client
        self.exchange_name = exchange_name
        self.market_type = market_type
        self.account_id = account_id
        self.is_public = is_public
        self.ip_address = ip_address
        self.last_fetched_market_info = 0
        self.is_ready = False
        # Its key can only read (accounts.json "read_only"): no leverage changes.
        self.read_only = False

    def get_client_key(self) -> str:
        return _get_client_key(
            self.exchange_name, self.market_type, self.account_id, self.is_public
        )


class CcxtWrapperCore:
    _instance = None
    RATE_LIMIT_COOLDOWN_S = RATE_LIMIT_COOLDOWN_S
    _rate_limit_until = _rate_limit_until

    exchange_clients: Dict[str, list[CcxtClient]] = {}
    round_robin_index: Dict[str, int] = {}
    client_key_to_last_load_markets_time: Dict[str, int] = {}

    def __new__(cls):
        if cls._instance is None:
            cls._instance = super(CcxtWrapperCore, cls).__new__(cls)
            cls._instance._initialized = False
        return cls._instance

    def __init__(self):
        if self._initialized:
            return
        self.listeners = set()
        self._ibkr_session = None
        # GCP-resolved Hyperliquid direct-signing creds, populated at startup by
        # _init_hyperliquid_accounts and read back by /get-hyperliquid-credentials
        # (polymarket-style: creds sourced from the ccxt layer, not re-read here).
        self._hyperliquid_direct_creds: dict = {}
        # (list name, account id) -> fingerprint of the accounts.json entry
        self._loaded_private_accounts: dict = {}
        self._failed_private_accounts: dict = {}
        self._private_stream_tasks: dict = {}
        # client key -> {"started_ms", "last_order_event_ms", "restarts"}
        self._private_stream_status: dict = {}
        self._leverage_tasks: dict = {}
        self._gateio_catalog = None
        self._initialized = True

    def _log_rate_limit_hit(
        self,
        exchange_name: str,
        market_type: str,
        http_status: int,
        cooldown_s: int,
    ):
        print(
            f"Rate limit hit for {exchange_name}:{market_type} (HTTP {http_status}); cooldown {cooldown_s}s"
        )

    def _pick_any_private_client(
        self, exchange_name: str, market_type: str
    ):
        """Returns the first ready private client for (exchange, market_type),
        regardless of account_id. Used by auth-required MARKET_DATA endpoints
        like fapi /historicalTrades, where a valid `X-MBX-APIKEY` header is
        mandatory but the specific account doesn't matter (no signing, no
        per-account semantics). Public clients have api_key="" — fapi rejects
        them with -2014; spot is lenient and silently accepted them, masking
        the bug for years."""
        prefix = f"{exchange_name}_{market_type}_"
        suffix = "_False"
        for key, clients in self.exchange_clients.items():
            if not (key.startswith(prefix) and key.endswith(suffix)):
                continue
            for c in clients:
                if c.is_ready:
                    return c.client
        raise ValueError(
            f"No ready private client for {exchange_name}:{market_type}"
        )

    def _round_robin_rotate_exchange_client(
        self,
        exchange_name: str,
        market_type: str,
        account_id: Optional[int],
        is_public: bool,
    ):
        key = _get_client_key(exchange_name, market_type, account_id, is_public)
        idx = self.round_robin_index.get(key, 0)

        # Both paths produce the same error string so callers can catch
        # `"No client found for key"` uniformly. Empty-list happens when a
        # venue's init was cut short by the per-init timeout — the key gets
        # created upfront but no client wrapper ever lands in the list.
        if key not in self.exchange_clients or not self.exchange_clients[key]:
            raise ValueError(f"No client found for key: {key}")

        clients = self.exchange_clients[key]
        # Guard against round_robin_index drifting past len() after a partial
        # init resets the list size mid-flight.
        if idx >= len(clients):
            idx = 0
        max_iterations = 25
        iteration_count = 0

        while iteration_count < max_iterations:
            client = clients[idx]
            if client.is_ready:
                self.round_robin_index[key] = idx + 1 if idx + 1 < len(clients) else 0
                return client.client

            idx = idx + 1 if idx + 1 < len(clients) else 0
            iteration_count += 1

        raise ValueError(
            f"No ready client found for key: {key} after {max_iterations} iterations"
        )

