import os
import json
import asyncio
import logging
import time
from typing import Optional
import requests
import httpx
from web3 import Web3

from cpp_live_proxy.type_models import Order
from cpp_live_proxy.polymarket_constants import (
    GAMMA_API_URL,
    DATA_API_URL,
    USDC_ADDRESS,
    PUSD_ADDRESS,
    CTF_ADDRESS,
    NEG_RISK_ADAPTER,
    COLLATERAL_ONRAMP,
    COLLATERAL_ONRAMP_ABI,
    COLLATERAL_OFFRAMP,
    COLLATERAL_OFFRAMP_ABI,
    DEFAULT_POLYGON_RPC,
    USDC_DECIMALS,
    MAX_UINT256,
    ERC20_APPROVE_ABI,
    ERC20_BALANCE_OF_ABI,
    ERC20_ALLOWANCE_ABI,
    ERC1155_IS_APPROVED_ABI,
    ERC1155_BALANCE_OF_ABI,
    CTF_SPLIT_POSITION_ABI,
    CTF_MERGE_POSITIONS_ABI,
    NEG_RISK_SPLIT_POSITION_ABI,
    NEG_RISK_MERGE_POSITIONS_ABI,
    CTF_REDEEM_POSITIONS_ABI,
    NEG_RISK_REDEEM_POSITIONS_ABI,
)
from py_clob_client.client import ClobClient
from py_clob_client.clob_types import (
    OpenOrderParams,
    OrderArgs,
    OrderType as ClobOrderType,
    PostOrdersArgs,
    RequestArgs,
    CreateOrderOptions,
)
from py_clob_client.headers.headers import create_level_2_headers
from py_clob_client.utilities import order_to_json, price_valid
from py_builder_signing_sdk.config import BuilderConfig
from py_builder_signing_sdk.sdk_types import BuilderApiKeyCreds

logger = logging.getLogger(__name__)


class _PolymarketAccount:
    def __init__(
        self,
        account_id: int,
        client: ClobClient,
        w3: Optional[Web3],
        eoa_account,
        private_key: str = "",
        builder_api_key: str = "",
        builder_secret: str = "",
        builder_passphrase: str = "",
    ):
        self.account_id = account_id
        self.client = client
        self.w3 = w3
        self.eoa_account = eoa_account
        self.private_key = private_key
        self.builder_api_key = builder_api_key
        self.builder_secret = builder_secret
        self.builder_passphrase = builder_passphrase
        self.order_options_cache: dict[str, tuple[str, bool, int]] = {}


class PolymarketWrapper:
    _instance: Optional["PolymarketWrapper"] = None

    def __new__(cls):
        if cls._instance is None:
            cls._instance = super().__new__(cls)
            cls._instance._initialized = False
        return cls._instance

    def __init__(self):
        if self._initialized:
            return
        self._initialized = True
        self._accounts: dict[int, _PolymarketAccount] = {}
        self._default_account_id: Optional[int] = None
        self._http_client = httpx.AsyncClient(
            http2=True,
            limits=httpx.Limits(max_connections=500, max_keepalive_connections=100),
            timeout=httpx.Timeout(30.0, connect=10.0),
        )

    @staticmethod
    def _resolve_gcp_secret(secret_ref: str) -> str:
        from cpp_live_proxy._compat import resolve_optional_gcp_secret

        return resolve_optional_gcp_secret(secret_ref)

    @staticmethod
    def _resolve_secret(
        account: dict, key: str, gcp_key: str, default: Optional[str] = None
    ) -> str:
        gcp_ref = account.get(gcp_key, "")
        if gcp_ref:
            return PolymarketWrapper._resolve_gcp_secret(gcp_ref)
        value = account.get(key, "")
        if value:
            return value
        if default is not None:
            return default
        raise ValueError(f"polymarket account must have either '{gcp_key}' or '{key}'")

    @staticmethod
    def _load_accounts() -> list[dict]:
        accounts_json_path = os.getenv("ACCOUNTS_JSON_PATH", "")
        if not accounts_json_path:
            raise ValueError("ACCOUNTS_JSON_PATH env var not set")
        with open(accounts_json_path, "r") as f:
            accounts = json.load(f)
        polymarket_accounts = accounts.get("polymarket_accounts", [])
        if not polymarket_accounts:
            raise ValueError("No polymarket_accounts found in accounts JSON")
        return polymarket_accounts

    async def init(self):
        raw_accounts = await asyncio.to_thread(self._load_accounts)

        for raw_account in raw_accounts:
            account_id = raw_account.get("account_id", 0)

            private_key = await asyncio.to_thread(
                self._resolve_secret,
                raw_account,
                "private_key",
                "private_key_gcp_secret",
            )
            builder_api_key = await asyncio.to_thread(
                self._resolve_secret,
                raw_account,
                "builder_api_key",
                "builder_api_key_gcp_secret",
            )
            builder_secret = await asyncio.to_thread(
                self._resolve_secret,
                raw_account,
                "builder_secret",
                "builder_secret_gcp_secret",
            )
            builder_passphrase = await asyncio.to_thread(
                self._resolve_secret,
                raw_account,
                "builder_passphrase",
                "builder_passphrase_gcp_secret",
            )

            chain_id = raw_account.get("chain_id", 137)
            host = raw_account.get("host", "https://clob.polymarket.com")
            signature_type = raw_account.get("signature_type")
            funder = raw_account.get("funder")

            builder_creds = BuilderApiKeyCreds(
                key=builder_api_key,
                secret=builder_secret,
                passphrase=builder_passphrase,
            )
            builder_config = BuilderConfig(local_builder_creds=builder_creds)

            client = ClobClient(
                host=host,
                key=private_key,
                chain_id=chain_id,
                signature_type=signature_type,
                funder=funder,
                builder_config=builder_config,
            )

            creds = await asyncio.to_thread(client.derive_api_key)
            client.set_api_creds(creds)

            rpc_url = await asyncio.to_thread(
                self._resolve_secret,
                raw_account,
                "polygon_rpc_url",
                "polygon_rpc_url_gcp_secret",
                DEFAULT_POLYGON_RPC,
            )
            w3, eoa_account = self._init_web3(rpc_url, private_key)

            acct = _PolymarketAccount(
                account_id=account_id,
                client=client,
                w3=w3,
                eoa_account=eoa_account,
                private_key=private_key,
                builder_api_key=builder_api_key,
                builder_secret=builder_secret,
                builder_passphrase=builder_passphrase,
            )
            self._accounts[account_id] = acct

            if self._default_account_id is None:
                self._default_account_id = account_id

            logger.info(f"Initialized polymarket account_id={account_id}")

    @staticmethod
    def _init_web3(rpc_url: str, private_key: str) -> tuple[Optional[Web3], object]:
        w3 = Web3(Web3.HTTPProvider(rpc_url))
        try:
            chain_id = w3.eth.chain_id
            if chain_id != 137:
                raise RuntimeError(
                    f"RPC returned chain_id {chain_id}, expected 137 (Polygon)"
                )
        except Exception as e:
            logger.warning(
                f"Web3 RPC init failed ({rpc_url}): {e}. On-chain ops will be unavailable."
            )
            return None, None

        eoa_account = w3.eth.account.from_key(private_key)
        logger.info(f"Web3 initialized: EOA={eoa_account.address}, RPC={rpc_url}")
        return w3, eoa_account

    def _get_account(self, account_id: int = 0) -> _PolymarketAccount:
        if account_id == 0 and self._default_account_id is not None:
            account_id = self._default_account_id
        acct = self._accounts.get(account_id)
        if acct is None:
            raise RuntimeError(f"Polymarket account_id={account_id} not found")
        return acct

    def get_all_accounts(self) -> dict[int, _PolymarketAccount]:
        return self._accounts

    def get_client(self, account_id: int = 0) -> ClobClient:
        return self._get_account(account_id).client

    def get_private_key(self, account_id: int = 0) -> str:
        return self._get_account(account_id).private_key

    def get_builder_api_key(self, account_id: int = 0) -> str:
        return self._get_account(account_id).builder_api_key

    def get_builder_secret(self, account_id: int = 0) -> str:
        return self._get_account(account_id).builder_secret

    def get_builder_passphrase(self, account_id: int = 0) -> str:
        return self._get_account(account_id).builder_passphrase

    @staticmethod
    def _ensure_web3(acct: _PolymarketAccount):
        if acct.w3 is None or acct.eoa_account is None:
            raise RuntimeError(
                f"Web3 not initialized for account_id={acct.account_id}. Check RPC connectivity."
            )

    @staticmethod
    def _send_eoa_tx(
        acct: _PolymarketAccount, to: str, data: str, value: int = 0
    ) -> dict:
        import time as _time

        PolymarketWrapper._ensure_web3(acct)
        w3 = acct.w3
        eoa = acct.eoa_account

        t0 = _time.time()
        nonce = w3.eth.get_transaction_count(eoa.address, "pending")
        tx = {
            "from": eoa.address,
            "to": Web3.to_checksum_address(to),
            "data": data,
            "value": value,
            "nonce": nonce,
            "chainId": 137,
        }
        tx["gasPrice"] = int(w3.eth.gas_price * 2)
        print(f"    [tx] Estimating gas... ({_time.time() - t0:.1f}s)")
        tx["gas"] = w3.eth.estimate_gas(tx)
        print(f"    [tx] Gas={tx['gas']}, sending... ({_time.time() - t0:.1f}s)")
        signed = eoa.sign_transaction(tx)
        tx_hash = w3.eth.send_raw_transaction(signed.raw_transaction)
        print(
            f"    [tx] Sent {tx_hash.hex()}, waiting for confirmation... ({_time.time() - t0:.1f}s)"
        )
        receipt = w3.eth.wait_for_transaction_receipt(
            tx_hash, timeout=180, poll_latency=2
        )
        print(
            f"    [tx] Confirmed in block {receipt['blockNumber']} ({_time.time() - t0:.1f}s)"
        )
        return {
            "tx_hash": receipt["transactionHash"].hex(),
            "status": "success" if receipt["status"] == 1 else "failed",
            "gas_used": receipt["gasUsed"],
            "block_number": receipt["blockNumber"],
            "logs": [
                {
                    "address": log["address"],
                    "topics": [
                        t.hex() if hasattr(t, "hex") else str(t)
                        for t in log.get("topics", [])
                    ],
                    "data": (
                        log["data"].hex()
                        if hasattr(log["data"], "hex")
                        else str(log["data"])
                    ),
                }
                for log in receipt.get("logs", [])
            ],
        }

    # ── Market resolution ──

    @staticmethod
    def resolve_market(slug: str) -> dict:
        resp = requests.get(
            f"{GAMMA_API_URL}/events/slug/{slug}",
            headers={"Accept": "application/json"},
        )
        resp.raise_for_status()
        event = resp.json()

        markets = event.get("markets", [])
        if not markets:
            raise RuntimeError(f"No markets found for slug: {slug}")

        market = markets[0]
        condition_id = market.get("conditionId", "")
        neg_risk = bool(market.get("negRisk", False))

        raw_token_ids = json.loads(market.get("clobTokenIds", "[]"))
        outcomes = json.loads(market.get("outcomes", "[]"))

        tokens = []
        for token_id, outcome in zip(raw_token_ids, outcomes):
            tokens.append({"token_id": token_id, "outcome": outcome})

        return {
            "condition_id": condition_id,
            "neg_risk": neg_risk,
            "tokens": tokens,
            "outcomes": outcomes,
            "question": market.get("question", ""),
            "active": market.get("active", True),
            "closed": market.get("closed", False),
            "market": market,
        }

    # ── On-chain operations ──

    def mint_complete_sets(
        self, condition_id: str, neg_risk: bool, num_sets: int, account_id: int = 0
    ) -> dict:
        acct = self._get_account(account_id)
        self._ensure_web3(acct)
        if not condition_id:
            raise ValueError("condition_id is required")

        amount = num_sets * (10**USDC_DECIMALS)
        condition_bytes = bytes.fromhex(condition_id.replace("0x", ""))

        # The CLOB-tradeable CTF outcome tokens are USDC.e-collateralized
        # (their clobTokenIds == getPositionId(USDC.e, ...)). Minting with pUSD
        # would produce orphan, untradeable token IDs. So splitPosition uses
        # USDC.e. (Trading/order settlement is separate and uses pUSD.)
        usdc = acct.w3.eth.contract(
            address=Web3.to_checksum_address(USDC_ADDRESS), abi=ERC20_BALANCE_OF_ABI
        )
        usdc_balance = usdc.functions.balanceOf(acct.eoa_account.address).call()
        if usdc_balance < amount:
            raise RuntimeError(
                f"Insufficient USDC.e for mint: have {usdc_balance / 10**USDC_DECIMALS:.6f}, need {num_sets}"
            )

        if neg_risk:
            contract = acct.w3.eth.contract(
                address=Web3.to_checksum_address(NEG_RISK_ADAPTER),
                abi=NEG_RISK_SPLIT_POSITION_ABI,
            )
            data = contract.encode_abi(
                abi_element_identifier="splitPosition",
                args=[condition_bytes, amount],
            )
            result = self._send_eoa_tx(acct, NEG_RISK_ADAPTER, data)
        else:
            parent_collection = b"\x00" * 32
            partition = [1, 2]
            contract = acct.w3.eth.contract(
                address=Web3.to_checksum_address(CTF_ADDRESS),
                abi=CTF_SPLIT_POSITION_ABI,
            )
            data = contract.encode_abi(
                abi_element_identifier="splitPosition",
                args=[
                    Web3.to_checksum_address(USDC_ADDRESS),
                    parent_collection,
                    condition_bytes,
                    partition,
                    amount,
                ],
            )
            result = self._send_eoa_tx(acct, CTF_ADDRESS, data)

        result["num_sets"] = num_sets
        result["condition_id"] = condition_id
        # Track for lag-free inventory enumeration (data-api /activity lags).
        PolymarketWrapper._TOUCHED_CONDITIONS.add(condition_id)
        return result

    def merge_complete_sets(
        self, condition_id: str, neg_risk: bool, num_sets: int, account_id: int = 0
    ) -> dict:
        acct = self._get_account(account_id)
        self._ensure_web3(acct)
        if not condition_id:
            raise ValueError("condition_id is required")

        amount = num_sets * (10**USDC_DECIMALS)
        condition_bytes = bytes.fromhex(condition_id.replace("0x", ""))

        if neg_risk:
            contract = acct.w3.eth.contract(
                address=Web3.to_checksum_address(NEG_RISK_ADAPTER),
                abi=NEG_RISK_MERGE_POSITIONS_ABI,
            )
            data = contract.encode_abi(
                abi_element_identifier="mergePositions",
                args=[condition_bytes, amount],
            )
            result = self._send_eoa_tx(acct, NEG_RISK_ADAPTER, data)
        else:
            parent_collection = b"\x00" * 32
            partition = [1, 2]
            contract = acct.w3.eth.contract(
                address=Web3.to_checksum_address(CTF_ADDRESS),
                abi=CTF_MERGE_POSITIONS_ABI,
            )
            data = contract.encode_abi(
                abi_element_identifier="mergePositions",
                args=[
                    Web3.to_checksum_address(USDC_ADDRESS),
                    parent_collection,
                    condition_bytes,
                    partition,
                    amount,
                ],
            )
            result = self._send_eoa_tx(acct, CTF_ADDRESS, data)

        result["num_sets"] = num_sets
        result["condition_id"] = condition_id
        return result

    @staticmethod
    def _sum_usdc_transfers_to(logs, recipient: str) -> int:
        """Sum USDC.e transferred to `recipient` from ERC20 Transfer events in
        tx logs (raw wei). CTF redeem of USDC.e-collateralized positions
        returns USDC.e.

        Logs are expected to be sanitized dicts with hex strings (from _send_eoa_tx).
        """
        recipient_padded = recipient.lower().replace("0x", "").zfill(64)
        collateral_lower = USDC_ADDRESS.lower()
        transfer_sig = (
            "ddf252ad1be2c89b69c2b068fc378daa952ba7f163c4a11628f55a4df523b3ef"
        )
        total = 0
        for log in logs:
            if log["address"].lower() != collateral_lower:
                continue
            topics = log["topics"]
            if len(topics) < 3:
                continue
            topic0 = (
                topics[0].replace("0x", "")
                if isinstance(topics[0], str)
                else topics[0].hex()
            )
            if topic0 != transfer_sig:
                continue
            topic2 = (
                topics[2].replace("0x", "")
                if isinstance(topics[2], str)
                else topics[2].hex()
            )
            if topic2.zfill(64) == recipient_padded:
                data_str = (
                    log["data"].replace("0x", "")
                    if isinstance(log["data"], str)
                    else log["data"].hex()
                )
                raw = data_str or "0"
                total += int(raw, 16)
        return total

    def redeem_winnings(
        self, condition_id: str, neg_risk: bool, tokens: list, account_id: int = 0
    ) -> dict:
        import time as _time

        acct = self._get_account(account_id)
        self._ensure_web3(acct)
        if not condition_id:
            raise ValueError("condition_id is required")

        condition_bytes = bytes.fromhex(condition_id.replace("0x", ""))
        addr = acct.eoa_account.address

        ctf = acct.w3.eth.contract(
            address=Web3.to_checksum_address(CTF_ADDRESS), abi=ERC1155_BALANCE_OF_ABI
        )

        t0 = _time.time()
        print(f"    [redeem] Reading token balances for {addr}...")
        token_balances = []
        has_tokens = False
        for tok in tokens:
            token_id = int(tok["token_id"])
            balance = ctf.functions.balanceOf(addr, token_id).call()
            print(f"    [redeem]   {tok.get('outcome', '?')}: balance={balance} (token_id={str(token_id)[:20]}...)")
            token_balances.append({"token_id": tok["token_id"], "balance": balance})
            if balance > 0:
                has_tokens = True
        print(f"    [redeem] Token balances read ({_time.time() - t0:.1f}s)")

        if not has_tokens:
            return {"status": "no_tokens", "token_balances": token_balances}

        t0 = _time.time()
        if neg_risk:
            amounts = [tb["balance"] for tb in token_balances]
            contract = acct.w3.eth.contract(
                address=Web3.to_checksum_address(NEG_RISK_ADAPTER),
                abi=NEG_RISK_REDEEM_POSITIONS_ABI,
            )
            data = contract.encode_abi(
                abi_element_identifier="redeemPositions",
                args=[condition_bytes, amounts],
            )
            print(f"    [redeem] Sending neg_risk redeemPositions tx...")
            result = self._send_eoa_tx(acct, NEG_RISK_ADAPTER, data)
        else:
            parent_collection = b"\x00" * 32
            index_sets = [1, 2]
            contract = acct.w3.eth.contract(
                address=Web3.to_checksum_address(CTF_ADDRESS),
                abi=CTF_REDEEM_POSITIONS_ABI,
            )
            data = contract.encode_abi(
                abi_element_identifier="redeemPositions",
                args=[
                    Web3.to_checksum_address(USDC_ADDRESS),
                    parent_collection,
                    condition_bytes,
                    index_sets,
                ],
            )
            print(f"    [redeem] Sending CTF redeemPositions tx...")
            result = self._send_eoa_tx(acct, CTF_ADDRESS, data)
        print(f"    [redeem] Tx confirmed ({_time.time() - t0:.1f}s)")

        usdc_wei = self._sum_usdc_transfers_to(result["logs"], addr)
        result["usdc_gained"] = usdc_wei / 10**USDC_DECIMALS
        result["token_balances"] = token_balances
        result["condition_id"] = condition_id
        return result

    # ── Read-only queries ──

    def get_eoa_balances(self, account_id: int = 0) -> dict:
        acct = self._get_account(account_id)
        self._ensure_web3(acct)
        addr = acct.eoa_account.address
        pol_balance = acct.w3.eth.get_balance(addr)
        usdc = acct.w3.eth.contract(
            address=Web3.to_checksum_address(USDC_ADDRESS),
            abi=ERC20_BALANCE_OF_ABI + ERC20_ALLOWANCE_ABI,
        )
        usdc_balance = usdc.functions.balanceOf(addr).call()
        # pUSD is the v2 collateral; this is the spendable trading balance.
        pusd = acct.w3.eth.contract(
            address=Web3.to_checksum_address(PUSD_ADDRESS), abi=ERC20_BALANCE_OF_ABI
        )
        pusd_balance = pusd.functions.balanceOf(addr).call()
        return {
            "address": addr,
            "pol_balance": float(acct.w3.from_wei(pol_balance, "ether")),
            "usdc_balance": usdc_balance / 10**USDC_DECIMALS,
            "usdc_balance_raw": usdc_balance,
            "pusd_balance": pusd_balance / 10**USDC_DECIMALS,
            "pusd_balance_raw": pusd_balance,
        }

    def unwrap_pusd_to_usdce(self, amount_usd: float, account_id: int = 0) -> dict:
        """Unwrap `amount_usd` of pUSD -> USDC.e via CollateralOfframp (approving
        the offramp first if needed). Used by the collateral rebalancer to keep
        minting (USDC.e) funded as mint->sell drains USDC.e into pUSD. Clamps to
        the available pUSD balance."""
        acct = self._get_account(account_id)
        self._ensure_web3(acct)
        addr = acct.eoa_account.address
        pusd = acct.w3.eth.contract(
            address=Web3.to_checksum_address(PUSD_ADDRESS),
            abi=ERC20_BALANCE_OF_ABI + ERC20_ALLOWANCE_ABI + ERC20_APPROVE_ABI,
        )
        bal = pusd.functions.balanceOf(addr).call()
        amt = min(int(round(amount_usd * 10**USDC_DECIMALS)), bal)
        if amt <= 0:
            return {"status": "noop", "reason": "no pUSD", "unwrapped_usd": 0.0}
        if pusd.functions.allowance(
            addr, Web3.to_checksum_address(COLLATERAL_OFFRAMP)
        ).call() < amt:
            approve_data = pusd.encode_abi(
                abi_element_identifier="approve",
                args=[Web3.to_checksum_address(COLLATERAL_OFFRAMP), MAX_UINT256],
            )
            # approve pUSD spend by the offramp
            self._send_eoa_tx(acct, PUSD_ADDRESS, approve_data)
        offramp = acct.w3.eth.contract(
            address=Web3.to_checksum_address(COLLATERAL_OFFRAMP),
            abi=COLLATERAL_OFFRAMP_ABI,
        )
        data = offramp.encode_abi(
            abi_element_identifier="unwrap",
            args=[Web3.to_checksum_address(USDC_ADDRESS), addr, amt],
        )
        result = self._send_eoa_tx(acct, COLLATERAL_OFFRAMP, data)
        result["unwrapped_usd"] = amt / 10**USDC_DECIMALS
        return result

    def wrap_usdce_to_pusd(self, amount_usd: float, account_id: int = 0) -> dict:
        """Wrap `amount_usd` of USDC.e -> pUSD via CollateralOnramp (approving the
        onramp first if needed). Mirror of unwrap_pusd_to_usdce: used by the
        collateral rebalancer to keep trading collateral (pUSD) funded when
        pUSD-buy -> USDC.e-redeem drains pUSD into USDC.e. Clamps to the available
        USDC.e balance."""
        acct = self._get_account(account_id)
        self._ensure_web3(acct)
        addr = acct.eoa_account.address
        usdce = acct.w3.eth.contract(
            address=Web3.to_checksum_address(USDC_ADDRESS),
            abi=ERC20_BALANCE_OF_ABI + ERC20_ALLOWANCE_ABI + ERC20_APPROVE_ABI,
        )
        bal = usdce.functions.balanceOf(addr).call()
        amt = min(int(round(amount_usd * 10**USDC_DECIMALS)), bal)
        if amt <= 0:
            return {"status": "noop", "reason": "no USDC.e", "wrapped_usd": 0.0}
        if usdce.functions.allowance(
            addr, Web3.to_checksum_address(COLLATERAL_ONRAMP)
        ).call() < amt:
            approve_data = usdce.encode_abi(
                abi_element_identifier="approve",
                args=[Web3.to_checksum_address(COLLATERAL_ONRAMP), MAX_UINT256],
            )
            self._send_eoa_tx(acct, USDC_ADDRESS, approve_data)
        onramp = acct.w3.eth.contract(
            address=Web3.to_checksum_address(COLLATERAL_ONRAMP),
            abi=COLLATERAL_ONRAMP_ABI,
        )
        data = onramp.encode_abi(
            abi_element_identifier="wrap",
            args=[Web3.to_checksum_address(USDC_ADDRESS), addr, amt],
        )
        result = self._send_eoa_tx(acct, COLLATERAL_ONRAMP, data)
        result["wrapped_usd"] = amt / 10**USDC_DECIMALS
        return result

    @staticmethod
    def get_positions(address: str) -> list[dict]:
        all_positions: list[dict] = []
        offset = 0
        limit = 500
        while True:
            resp = requests.get(
                f"{DATA_API_URL}/positions",
                params={
                    "user": address,
                    "sizeThreshold": 0,
                    "limit": limit,
                    "offset": offset,
                    "sortBy": "TOKENS",
                    "sortDirection": "DESC",
                },
            )
            resp.raise_for_status()
            batch = resp.json()
            if not batch:
                break
            all_positions.extend(batch)
            if len(batch) < limit:
                break
            offset += limit
        return all_positions

    def get_token_balances(self, tokens: list, account_id: int = 0) -> list[dict]:
        acct = self._get_account(account_id)
        self._ensure_web3(acct)
        ctf = acct.w3.eth.contract(
            address=Web3.to_checksum_address(CTF_ADDRESS), abi=ERC1155_BALANCE_OF_ABI
        )
        addr = acct.eoa_account.address
        result = []
        for tok in tokens:
            token_id = int(tok["token_id"])
            balance = ctf.functions.balanceOf(addr, token_id).call()
            result.append(
                {
                    "token_id": tok["token_id"],
                    "outcome": tok.get("outcome", ""),
                    "balance": balance,
                    "balance_display": balance / 10**USDC_DECIMALS,
                }
            )
        return result

    # condition_id -> [(token_id_str, outcome_index)] ; getPositionId is
    # deterministic so this is cached across the 5s-polled inventory calls.
    _POSITION_ID_CACHE: dict = {}
    # Conditions this proxy has minted/merged this run. The data-api /activity
    # lags fresh on-chain actions, so we track them in-process to surface
    # just-minted inventory immediately (no indexing wait).
    _TOUCHED_CONDITIONS: set = set()
    # condition_id -> {outcome_index: outcome_name} (from gamma), cached.
    _OUTCOME_NAME_CACHE: dict = {}

    def _condition_outcomes(self, condition_id: str) -> dict:
        if condition_id in PolymarketWrapper._OUTCOME_NAME_CACHE:
            return PolymarketWrapper._OUTCOME_NAME_CACHE[condition_id]
        names = {}
        try:
            r = requests.get(
                f"{GAMMA_API_URL}/markets",
                params={"condition_ids": condition_id}, timeout=15,
            )
            mk = r.json() if r.status_code == 200 else []
            if mk:
                outs = mk[0].get("outcomes")
                outs = json.loads(outs) if isinstance(outs, str) else (outs or [])
                names = {i: o for i, o in enumerate(outs)}
        except Exception:
            names = {}
        if names:  # only cache positive resolutions
            PolymarketWrapper._OUTCOME_NAME_CACHE[condition_id] = names
        return names
    _CTF_POSITION_ABI = [
        {"inputs": [{"name": "p", "type": "bytes32"}, {"name": "c", "type": "bytes32"},
                    {"name": "i", "type": "uint256"}], "name": "getCollectionId",
         "outputs": [{"type": "bytes32"}], "stateMutability": "view", "type": "function"},
        {"inputs": [{"name": "t", "type": "address"}, {"name": "c", "type": "bytes32"}],
         "name": "getPositionId", "outputs": [{"type": "uint256"}],
         "stateMutability": "view", "type": "function"},
    ]

    def _condition_token_ids(self, acct, condition_id: str) -> list:
        """USDC.e-collateralized CTF position IDs (== tradeable clobTokenIds)
        for a binary condition: [(token_id_str, outcome_index)]."""
        if condition_id in PolymarketWrapper._POSITION_ID_CACHE:
            return PolymarketWrapper._POSITION_ID_CACHE[condition_id]
        ctf = acct.w3.eth.contract(
            address=Web3.to_checksum_address(CTF_ADDRESS), abi=self._CTF_POSITION_ABI
        )
        cb = bytes.fromhex(condition_id.replace("0x", ""))
        out = []
        for idx in (1, 2):
            coll = ctf.functions.getCollectionId(b"\x00" * 32, cb, idx).call()
            pid = ctf.functions.getPositionId(
                Web3.to_checksum_address(USDC_ADDRESS), coll
            ).call()
            out.append((str(pid), idx - 1))
        PolymarketWrapper._POSITION_ID_CACHE[condition_id] = out
        return out

    def get_onchain_inventory(self, account_id: int = 0, lookback_secs: int = 3600) -> list:
        """True held inventory from on-chain CTF balances. The data-api
        /positions nets balanced complete sets to invisible, so we enumerate
        the account's recently-active token IDs (trades give them directly;
        splits are computed on-chain) and read balanceOf for each. Returns
        [{asset: condition_id, outcome, size, token_id}] for nonzero balances.
        """
        import time as _time

        acct = self._get_account(account_id)
        self._ensure_web3(acct)
        addr = acct.eoa_account.address
        cutoff = _time.time() - lookback_secs

        token_cond: dict = {}   # token_id -> condition_id
        token_name: dict = {}   # token_id -> outcome name
        cond_names: dict = {}   # condition_id -> {outcome_index: name}

        def _recent(typ):
            try:
                r = requests.get(
                    f"{DATA_API_URL}/activity",
                    params={"user": addr, "type": typ, "limit": 500},
                    timeout=20,
                )
                return r.json() if r.status_code == 200 else []
            except Exception:
                return []

        # Traded tokens come with token IDs + outcome names directly.
        for a in _recent("TRADE"):
            if float(a.get("timestamp", 0)) < cutoff:
                continue
            tok = str(a.get("asset", "") or "")
            cond = a.get("conditionId", "")
            oi = a.get("outcomeIndex")
            if tok:
                token_cond[tok] = cond
                token_name[tok] = a.get("outcome", "")
            if cond and isinstance(oi, int) and 0 <= oi < 2:
                cond_names.setdefault(cond, {})[oi] = a.get("outcome", "")

        # Condition universe = proxy-minted-this-run (lag-free) + recent splits.
        conds = set(PolymarketWrapper._TOUCHED_CONDITIONS)
        for a in _recent("SPLIT"):
            if float(a.get("timestamp", 0)) >= cutoff and a.get("conditionId"):
                conds.add(a["conditionId"])
        for cond in conds:
            names = cond_names.get(cond) or self._condition_outcomes(cond)
            for tok, oi in self._condition_token_ids(acct, cond):
                if tok not in token_cond:
                    token_cond[tok] = cond
                    token_name[tok] = (names or {}).get(oi, "")

        if not token_cond:
            return []

        bals = self.get_token_balances(
            [{"token_id": t, "outcome": token_name.get(t, "")} for t in token_cond],
            account_id,
        )
        inv = []
        live_conds = set()
        for b in bals:
            if b["balance"] > 0:
                tok = b["token_id"]
                cond = token_cond.get(tok, "")
                live_conds.add(cond)
                inv.append(
                    {
                        "asset": cond,
                        "outcome": token_name.get(tok, ""),
                        "size": b["balance_display"],
                        "token_id": tok,
                    }
                )
        # Drop fully-exited (0-balance) tracked conditions so the set stays bounded.
        PolymarketWrapper._TOUCHED_CONDITIONS = {
            c for c in PolymarketWrapper._TOUCHED_CONDITIONS if c in live_conds
        }
        return inv

    def get_eoa_address(self, account_id: int = 0) -> str:
        acct = self._get_account(account_id)
        if acct.eoa_account is not None:
            return acct.eoa_account.address
        if acct.client is not None:
            return acct.client.get_address()
        raise RuntimeError(f"PolymarketWrapper account_id={account_id} not initialized")

    # ── CLOB operations ──

    @staticmethod
    def _map_order_type(order_type: str) -> ClobOrderType:
        if order_type == "fill_or_kill":
            return ClobOrderType.FOK
        elif order_type == "immediate_or_cancel":
            return ClobOrderType.FAK
        return ClobOrderType.GTC

    async def _resolve_order_options(
        self, acct: _PolymarketAccount, token_id: str
    ) -> tuple[str, bool, int]:
        cached = acct.order_options_cache.get(token_id)
        if cached is not None:
            return cached

        host = acct.client.host

        async def fetch_tick_size():
            resp = await self._http_client.get(
                f"{host}/tick-size?token_id={token_id}",
                headers={"Accept": "*/*", "Connection": "keep-alive"},
            )
            resp.raise_for_status()
            return str(resp.json()["minimum_tick_size"])

        async def fetch_neg_risk():
            resp = await self._http_client.get(
                f"{host}/neg-risk?token_id={token_id}",
                headers={"Accept": "*/*", "Connection": "keep-alive"},
            )
            resp.raise_for_status()
            return resp.json()["neg_risk"]

        async def fetch_fee_rate():
            resp = await self._http_client.get(
                f"{host}/fee-rate?token_id={token_id}",
                headers={"Accept": "*/*", "Connection": "keep-alive"},
            )
            resp.raise_for_status()
            return resp.json().get("base_fee") or 0

        tick_size, neg_risk, fee_rate_bps = await asyncio.gather(
            fetch_tick_size(), fetch_neg_risk(), fetch_fee_rate()
        )
        acct.order_options_cache[token_id] = (tick_size, neg_risk, fee_rate_bps)
        return tick_size, neg_risk, fee_rate_bps

    async def _sign_order(self, acct: _PolymarketAccount, order: Order):
        token_id = order.clob_id
        tick_size, neg_risk, fee_rate_bps = await self._resolve_order_options(
            acct, token_id
        )

        order_args = OrderArgs(
            token_id=token_id,
            price=float(order.price),
            size=float(order.quantity),
            side=order.side.upper(),
            fee_rate_bps=fee_rate_bps,
        )

        if not price_valid(order_args.price, tick_size):
            raise ValueError(
                f"price ({order_args.price}), min: {tick_size} - max: {1 - float(tick_size)}"
            )

        signed = await asyncio.to_thread(
            acct.client.builder.create_order,
            order_args,
            CreateOrderOptions(tick_size=tick_size, neg_risk=neg_risk),
        )
        return signed

    async def _post_order_async(
        self,
        acct: _PolymarketAccount,
        signed_order,
        order_type: ClobOrderType,
        post_only: bool = False,
    ) -> dict:
        body = order_to_json(
            signed_order, acct.client.creds.api_key, order_type, post_only
        )
        serialized_body = json.dumps(body, separators=(",", ":"), ensure_ascii=False)

        request_args = RequestArgs(
            method="POST",
            request_path="/order",
            body=body,
            serialized_body=serialized_body,
        )
        headers = create_level_2_headers(
            acct.client.signer, acct.client.creds, request_args
        )

        if acct.client.can_builder_auth():
            builder_headers = acct.client._generate_builder_headers(
                request_args, headers
            )
            if builder_headers is not None:
                headers = builder_headers

        headers["User-Agent"] = "py_clob_client"
        headers["Accept"] = "*/*"
        headers["Content-Type"] = "application/json"
        headers["Connection"] = "keep-alive"

        url = f"{acct.client.host}/order"
        try:
            resp = await self._http_client.post(
                url, headers=headers, content=serialized_body.encode("utf-8")
            )
        except httpx.RequestError as e:
            raise RuntimeError(
                f"Polymarket HTTP request failed: {type(e).__name__}: {e}"
            ) from e

        if resp.status_code != 200:
            raise RuntimeError(
                f"Polymarket order POST failed: status={resp.status_code} body={resp.text}"
            )

        try:
            return resp.json()
        except ValueError:
            return {"raw_text": resp.text}

    async def _post_orders_async(
        self,
        acct: _PolymarketAccount,
        signed_orders: list[tuple],
    ) -> dict | list:
        body = [
            order_to_json(signed, acct.client.creds.api_key, order_type, post_only)
            for signed, order_type, post_only in signed_orders
        ]
        serialized_body = json.dumps(body, separators=(",", ":"), ensure_ascii=False)

        request_args = RequestArgs(
            method="POST",
            request_path="/orders",
            body=body,
            serialized_body=serialized_body,
        )
        headers = create_level_2_headers(
            acct.client.signer, acct.client.creds, request_args
        )

        if acct.client.can_builder_auth():
            builder_headers = acct.client._generate_builder_headers(
                request_args, headers
            )
            if builder_headers is not None:
                headers = builder_headers

        headers["User-Agent"] = "py_clob_client"
        headers["Accept"] = "*/*"
        headers["Content-Type"] = "application/json"
        headers["Connection"] = "keep-alive"

        url = f"{acct.client.host}/orders"
        try:
            resp = await self._http_client.post(
                url, headers=headers, content=serialized_body.encode("utf-8")
            )
        except httpx.RequestError as e:
            raise RuntimeError(
                f"Polymarket HTTP request failed: {type(e).__name__}: {e}"
            ) from e

        if resp.status_code != 200:
            raise RuntimeError(
                f"Polymarket bulk order POST failed: status={resp.status_code} body={resp.text}"
            )

        try:
            return resp.json()
        except ValueError:
            return {"raw_text": resp.text}

    async def _cancel_orders_async(
        self,
        acct: _PolymarketAccount,
        order_ids: list[str],
    ) -> dict | list:
        body = order_ids
        serialized_body = json.dumps(body, separators=(",", ":"), ensure_ascii=False)

        request_args = RequestArgs(
            method="DELETE",
            request_path="/orders",
            body=body,
            serialized_body=serialized_body,
        )
        headers = create_level_2_headers(
            acct.client.signer, acct.client.creds, request_args
        )

        headers["User-Agent"] = "py_clob_client"
        headers["Accept"] = "*/*"
        headers["Content-Type"] = "application/json"
        headers["Connection"] = "keep-alive"

        url = f"{acct.client.host}/orders"
        try:
            resp = await self._http_client.request(
                "DELETE", url, headers=headers, content=serialized_body.encode("utf-8")
            )
        except httpx.RequestError as e:
            raise RuntimeError(
                f"Polymarket cancel HTTP request failed: {type(e).__name__}: {e}"
            ) from e

        if resp.status_code != 200:
            raise RuntimeError(
                f"Polymarket cancel failed: status={resp.status_code} body={resp.text}"
            )

        try:
            return resp.json()
        except ValueError:
            return {"raw_text": resp.text}

    async def create_order(self, order: Order) -> dict:
        acct = self._get_account(order.account_id)
        if acct.client is None:
            raise RuntimeError("PolymarketWrapper not initialized")

        order_type = self._map_order_type(order.type)
        signed = await self._sign_order(acct, order)
        result = await self._post_order_async(acct, signed, order_type, order.post_only)

        if isinstance(result, dict) and result.get("success", False):
            return result
        else:
            error_msg = str(result) if result else "Unknown error"
            raise RuntimeError(f"Polymarket order creation failed: {error_msg}")

    async def create_order_raw(self, order: Order) -> dict:
        """Like create_order but returns the complete raw API response."""
        acct = self._get_account(order.account_id)
        if acct.client is None:
            raise RuntimeError("PolymarketWrapper not initialized")

        order_type = self._map_order_type(order.type)
        signed = await self._sign_order(acct, order)

        if order.server_sending_deadline_ms > 0:
            now_ms = int(time.time() * 1000)
            print(
                f"create_order_raw: difference={now_ms - order.server_sending_deadline_ms}"
            )
            if now_ms > order.server_sending_deadline_ms:
                return {
                    "success": False,
                    "errorMsg": "order expired server-side (post-sign)",
                    "orderID": "",
                }

        return await self._post_order_async(acct, signed, order_type, order.post_only)

    async def create_orders_bulk(self, orders: list[Order]) -> list[dict]:
        if not orders:
            return []
        acct = self._get_account(orders[0].account_id)
        if acct.client is None:
            raise RuntimeError("PolymarketWrapper not initialized")

        signed_orders: list[tuple] = []
        expired_post_sign: set[int] = set()
        for i, order in enumerate(orders):
            order_type = self._map_order_type(order.type)
            try:
                signed = await self._sign_order(acct, order)
                signed_orders.append((signed, order_type, order.post_only))
            except Exception as e:
                print(
                    f"[bulk_create] Sign FAILED [{i}] token={order.clob_id} "
                    f"price={order.price} qty={order.quantity}: ({type(e).__name__}) {e}"
                )
                raise

        now_ms = int(time.time() * 1000)
        live_signed: list[tuple] = []
        for i, order in enumerate(orders):
            if (
                order.server_sending_deadline_ms > 0
                and now_ms > order.server_sending_deadline_ms
            ):
                expired_post_sign.add(i)
            else:
                live_signed.append(signed_orders[i])

        if not live_signed:
            return [
                {
                    "success": False,
                    "orderID": "",
                    "errorMsg": "order expired server-side (post-sign)",
                    "status": "",
                    "takingAmount": "0",
                    "makingAmount": "0",
                }
                for _ in orders
            ]

        try:
            raw_response = await self._post_orders_async(acct, live_signed)
        except Exception as e:
            print(
                f"[bulk_create] post_orders FAILED ({type(e).__name__}): {e}, "
                f"order_count={len(signed_orders)}"
            )
            raise

        if isinstance(raw_response, dict):
            responses = raw_response.get("data", [raw_response])
        elif isinstance(raw_response, list):
            responses = raw_response
        else:
            print(
                f"[bulk_create] unexpected raw_response type: {type(raw_response).__name__}"
            )
            responses = []

        if isinstance(responses, list) and len(responses) != len(live_signed):
            print(
                f"[bulk_create] response count mismatch: "
                f"sent {len(live_signed)} orders, got {len(responses)} responses"
            )

        expired_result = {
            "success": False,
            "orderID": "",
            "errorMsg": "order expired server-side (post-sign)",
            "status": "",
            "takingAmount": "0",
            "makingAmount": "0",
        }
        results: list[dict] = [
            (
                expired_result
                if i in expired_post_sign
                else {
                    "success": False,
                    "orderID": "",
                    "errorMsg": "no response",
                    "status": "",
                    "takingAmount": "0",
                    "makingAmount": "0",
                }
            )
            for i in range(len(orders))
        ]

        if isinstance(responses, list):
            live_idx = 0
            for i in range(len(orders)):
                if i in expired_post_sign:
                    continue
                if live_idx >= len(responses):
                    break
                resp = responses[live_idx]
                live_idx += 1
                error_msg = resp.get("errorMsg", "")
                success = resp.get("success", False)
                if not success and error_msg:
                    print(f"[bulk_create] order [{i}] REJECTED: {error_msg}")
                results[i] = {
                    "success": success,
                    "orderID": resp.get("orderID", ""),
                    "errorMsg": error_msg,
                    "status": resp.get("status", ""),
                    "takingAmount": resp.get("takingAmount", "0"),
                    "makingAmount": resp.get("makingAmount", "0"),
                }

        return results

    _POLYMARKET_STATUS_MAP = {
        "LIVE": "open",
        "MATCHED": "closed",
        "CANCELED": "canceled",
        "DELAYED": "open",
        "UNMATCHED": "open",
    }

    @staticmethod
    def _normalize_order_to_ccxt(order: dict) -> dict:
        status_raw = order.get("status", "").upper()
        status = PolymarketWrapper._POLYMARKET_STATUS_MAP.get(status_raw, "closed")

        created_at = order.get("created_at")
        if isinstance(created_at, str):
            try:
                created_at = int(created_at)
            except (ValueError, TypeError):
                created_at = 0
        timestamp_ms = (created_at or 0) * 1000

        def to_float(val, default=0.0):
            if val is None:
                return default
            try:
                return float(val)
            except (ValueError, TypeError):
                return default

        return {
            "symbol": order.get("market", ""),
            "id": order.get("id", ""),
            "clientOrderId": "",
            "status": status,
            "price": to_float(order.get("price")),
            "average": None,
            "amount": to_float(order.get("original_size")),
            "filled": to_float(order.get("size_matched")),
            "timestamp": timestamp_ms,
            "lastTradeTimestamp": None,
            "lastUpdateTimestamp": None,
            "fee": None,
        }

    async def get_open_orders(self, market: str, account_id: int = 0) -> list[dict]:
        acct = self._get_account(account_id)
        if acct.client is None:
            raise RuntimeError("PolymarketWrapper not initialized")
        params = OpenOrderParams(market=market)
        raw_orders = await asyncio.to_thread(acct.client.get_orders, params)
        if not isinstance(raw_orders, list):
            return []
        return [self._normalize_order_to_ccxt(o) for o in raw_orders]

    async def get_all_open_orders(self, account_id: int = 0) -> list[dict]:
        acct = self._get_account(account_id)
        if acct.client is None:
            raise RuntimeError("PolymarketWrapper not initialized")
        raw_orders = await asyncio.to_thread(acct.client.get_orders)
        if not isinstance(raw_orders, list):
            return []
        # client.get_orders() returns ALL orders (LIVE, MATCHED, CANCELED).
        # Only return LIVE orders since this function is used for orphan
        # detection and cancel flows — attempting to cancel MATCHED/CANCELED
        # orders causes "order can't be found" errors from Polymarket.
        live_orders = [
            o for o in raw_orders
            if o.get("status", "").upper() == "LIVE"
        ]
        return [self._normalize_order_to_ccxt(o) for o in live_orders]

    async def query_order(self, order_id: str, account_id: int = 0) -> Optional[dict]:
        acct = self._get_account(account_id)
        if acct.client is None:
            raise RuntimeError("PolymarketWrapper not initialized")
        raw_order = await asyncio.to_thread(acct.client.get_order, order_id)
        if not raw_order or not isinstance(raw_order, dict):
            return None
        if not raw_order.get("id"):
            return None
        return self._normalize_order_to_ccxt(raw_order)

    async def cancel_orders_bulk(
        self, order_ids: list[str], account_id: int = 0
    ) -> list[dict]:
        acct = self._get_account(account_id)
        if acct.client is None:
            raise RuntimeError("PolymarketWrapper not initialized")

        results: list[dict] = [
            {"success": False, "orderID": oid, "errorMsg": "no response"}
            for oid in order_ids
        ]

        if not order_ids:
            return results

        raw_response = await self._cancel_orders_async(acct, order_ids)

        if isinstance(raw_response, dict):
            canceled = set(raw_response.get("canceled", []))
            not_canceled = raw_response.get("not_canceled", {})
            for i, oid in enumerate(order_ids):
                if oid in canceled:
                    results[i] = {
                        "success": True,
                        "orderID": oid,
                        "errorMsg": "",
                    }
                elif oid in not_canceled:
                    results[i] = {
                        "success": False,
                        "orderID": oid,
                        "errorMsg": str(not_canceled[oid]),
                    }
        elif isinstance(raw_response, list):
            canceled = set(raw_response)
            for i, oid in enumerate(order_ids):
                results[i] = {
                    "success": oid in canceled,
                    "orderID": oid,
                    "errorMsg": "" if oid in canceled else "not in response",
                }

        return results
