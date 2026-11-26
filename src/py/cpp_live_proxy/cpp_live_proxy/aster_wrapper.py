"""Aster V3-signed REST client for read-only account queries.

Uses EIP-712 wallet signing (same as C++ AsterClient) to call
authenticated Aster API endpoints. Only handles balance, position,
and order queries — order creation/cancellation is done in C++.
"""

import json
import os
import threading
import time
import urllib.parse
from dataclasses import dataclass
from typing import Optional

import httpx
from eth_account import Account
from cpp_live_proxy._compat import resolve_optional_gcp_secret


ASTER_USE_TESTNET = os.getenv("ASTER_USE_TESTNET", "0") == "1"
ASTER_BASE_URL = os.getenv(
    "ASTER_API_BASE_URL",
    "https://testnet-fapi.asterdex.com" if ASTER_USE_TESTNET else "https://fapi.asterdex.com"
)
ASTER_CHAIN_ID = 714 if ASTER_USE_TESTNET else 1666

EIP712_TYPES = {
    "EIP712Domain": [
        {"name": "name", "type": "string"},
        {"name": "version", "type": "string"},
        {"name": "chainId", "type": "uint256"},
        {"name": "verifyingContract", "type": "address"},
    ],
    "Message": [{"name": "msg", "type": "string"}],
}

_STATUS_MAP = {
    "new": "open",
    "partially_filled": "open",
    "filled": "closed",
    "canceled": "canceled",
    "expired": "canceled",
}


@dataclass
class _AsterAccount:
    account_id: int
    private_key: str
    user_address: str
    signer_address: str


class AsterWrapper:
    _instance: Optional["AsterWrapper"] = None
    _accounts: dict[int, _AsterAccount]
    _nonce_lock: threading.Lock
    _last_nonce_sec: int
    _nonce_counter: int
    _http: httpx.AsyncClient

    def __new__(cls):
        if cls._instance is None:
            cls._instance = super().__new__(cls)
            cls._instance._initialized = False
        return cls._instance

    async def init(self):
        if self._initialized:
            return
        self._accounts = {}
        self._nonce_lock = threading.Lock()
        self._last_nonce_sec = 0
        self._nonce_counter = 0
        self._http = httpx.AsyncClient(timeout=httpx.Timeout(15.0, connect=5.0))
        self._load_accounts()
        self._initialized = True

    def _load_accounts(self):
        accounts_path = os.getenv("ACCOUNTS_JSON_PATH")
        if not accounts_path:
            return

        with open(accounts_path) as f:
            data = json.load(f)

        for item in data.get("aster_futures_accounts", []):
            gcp_ref = item.get("private_key_gcp_secret", "")
            if gcp_ref and os.getenv("GCP_PROJECT_ID", ""):
                pk = resolve_optional_gcp_secret(gcp_ref)
            else:
                pk = item.get("private_key", "")

            if not pk:
                continue

            acct = _AsterAccount(
                account_id=item["account_id"],
                private_key=pk,
                user_address=item["user_address"],
                signer_address=item["signer_address"],
            )
            self._accounts[acct.account_id] = acct

    # ── Nonce ────────────────────────────────────────────────────────────

    def _get_nonce(self) -> int:
        with self._nonce_lock:
            now_sec = int(time.time())
            if now_sec == self._last_nonce_sec:
                self._nonce_counter += 1
            else:
                self._last_nonce_sec = now_sec
                self._nonce_counter = 0
            return now_sec * 1_000_000 + self._nonce_counter

    # ── EIP-712 signing ──────────────────────────────────────────────────

    def _sign_params(self, param_str: str, account: _AsterAccount) -> str:
        domain = {
            "name": "AsterSignTransaction",
            "version": "1",
            "chainId": ASTER_CHAIN_ID,
            "verifyingContract": "0x0000000000000000000000000000000000000000",
        }
        signed = Account.sign_typed_data(
            account.private_key,
            domain_data=domain,
            message_types={"Message": EIP712_TYPES["Message"]},
            message_data={"msg": param_str},
        )
        return signed.signature.hex()

    # ── HTTP ─────────────────────────────────────────────────────────────

    async def _signed_request(
        self, method: str, path: str, params: dict, account_id: int
    ) -> dict | list:
        acct = self._accounts.get(account_id)
        if not acct:
            raise ValueError(f"Aster account {account_id} not found")

        params["nonce"] = str(self._get_nonce())
        params["user"] = acct.user_address
        params["signer"] = acct.signer_address

        param_str = urllib.parse.urlencode(params)
        signature = self._sign_params(param_str, acct)
        full_url = f"{ASTER_BASE_URL}{path}?{param_str}&signature={signature}"

        if method == "GET":
            resp = await self._http.get(full_url)
        elif method == "DELETE":
            resp = await self._http.delete(full_url)
        else:
            resp = await self._http.post(full_url)

        resp.raise_for_status()
        return resp.json()

    # ── Symbol conversion ────────────────────────────────────────────────

    @staticmethod
    def ccxt_symbol_to_raw(symbol: str) -> str:
        """Convert CCXT format (XRP/USDT:USDT) back to raw (XRPUSDT)."""
        return symbol.split(":")[0].replace("/", "")

    # ── Order normalization ──────────────────────────────────────────────

    @staticmethod
    def _normalize_order_to_ccxt(order: dict) -> dict:
        """Convert Aster order response to CCXT-normalized format.

        Same contract as PolymarketWrapper._normalize_order_to_ccxt —
        returns fields expected by OrderResNormalized::from_ccxt_user_data_stream_msg.
        """
        raw_status = order.get("status", "").lower()
        return {
            "symbol": order.get("symbol", ""),
            "clientOrderId": order.get("clientOrderId", ""),
            "id": str(order.get("orderId", "")),
            "status": _STATUS_MAP.get(raw_status, raw_status),
            "price": float(order.get("price", 0)),
            "average": float(order.get("avgPrice", 0)) or float(order.get("price", 0)),
            "amount": float(order.get("origQty", 0)),
            "filled": float(order.get("executedQty", 0)),
            "side": order.get("side", "").lower(),
            "type": order.get("type", "").lower(),
            "timestamp": order.get("time", 0),
            "lastTradeTimestamp": None,
            "lastUpdateTimestamp": order.get("updateTime", None),
            "fee": None,
        }

    # ── Public API ───────────────────────────────────────────────────────

    def has_account(self, account_id: int) -> bool:
        return account_id in self._accounts

    async def get_balance(self, account_id: int) -> list[dict]:
        return await self._signed_request("GET", "/fapi/v3/balance", {}, account_id)

    async def get_positions(
        self, account_id: int, symbol: str | None = None
    ) -> list[dict]:
        params = {}
        if symbol:
            params["symbol"] = symbol
        return await self._signed_request(
            "GET", "/fapi/v3/positionRisk", params, account_id
        )

    async def get_open_orders(
        self, account_id: int, symbol: str | None = None
    ) -> list[dict]:
        """Returns CCXT-normalized open orders."""
        params = {}
        raw_symbol = self.ccxt_symbol_to_raw(symbol) if symbol else None
        if raw_symbol:
            params["symbol"] = raw_symbol
        raw = await self._signed_request(
            "GET", "/fapi/v3/openOrders", params, account_id
        )
        return [self._normalize_order_to_ccxt(o) for o in raw]

    async def query_order(
        self, account_id: int, symbol: str, order_id: str
    ) -> Optional[dict]:
        """Returns CCXT-normalized order or None."""
        raw_symbol = self.ccxt_symbol_to_raw(symbol)
        params = {"symbol": raw_symbol, "orderId": order_id}
        raw = await self._signed_request(
            "GET", "/fapi/v3/order", params, account_id
        )
        if not raw or not isinstance(raw, dict):
            return None
        return self._normalize_order_to_ccxt(raw)
