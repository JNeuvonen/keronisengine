from typing import Dict

from cpp_live_proxy.ccxt_constants import (
    EXCHANGE_NAME_BINANCE,
    EXCHANGE_NAME_BITGET,
    EXCHANGE_NAME_BYBIT,
    EXCHANGE_NAME_GATEIO,
    EXCHANGE_NAME_HYPERLIQUID,
    EXCHANGE_NAME_IBKR,
    MARKET_TYPE_SPOT,
    MARKET_TYPE_SWAP,
)
from cpp_live_proxy.gcp_log import log


class CcxtWrapperInventoryMixin:
    def _parse_binance_futures_inventory_state(self, inventory_state: Dict):
        quote_assets = []
        inventory = []

        for item in inventory_state["info"]["assets"]:
            if item["asset"] == "USDC":
                quote_asset = {
                    "asset": item["asset"],
                    "quantity": inventory_state["info"]["totalMarginBalance"],
                    "free": inventory_state["info"]["totalMarginBalance"],
                    "used": "0",
                    "price": 1,
                }
                quote_assets.append(quote_asset)

        for item in inventory_state["info"]["positions"]:
            # Binance USDS-M Futures emits one position row PER side in hedge
            # mode (positionSide="LONG" / "SHORT") and one combined row with
            # positionSide="BOTH" in one-way mode. Preserve the side here so
            # the C++ inventory validator can compare per-side against the
            # corresponding long/short open trade quantity. Without this, the
            # downstream net-vs-net check is blind to gross drift where both
            # legs grow in parallel (e.g. cpp thinks 100L+50S net=+50, exchange
            # has 200L+150S also net=+50 — no mismatch raised, 100 of hidden
            # long and 100 of hidden short sit unbooked).
            inventory.append(
                {
                    "total": item["positionAmt"],
                    "used": "0",
                    "free": "0",
                    "debt": "0",
                    "used_margin": item["initialMargin"],
                    "maint_margin": item["maintMargin"],
                    "asset": item["symbol"],
                    "quote_conversion_symbol": item["symbol"],
                    "exchange_name": EXCHANGE_NAME_BINANCE,
                    "market_type": MARKET_TYPE_SWAP,
                    "position_side": item.get("positionSide", "BOTH"),
                }
            )

        return {
            "quote_assets": quote_assets,
            "inventory": inventory,
        }

    def _parse_binance_spot_inventory_state(self, inventory_state: Dict):
        quote_assets = []
        inventory = []

        is_margin_account = "userAssets" in inventory_state.get("info", {})

        if is_margin_account:
            for item in inventory_state["info"]["userAssets"]:
                if item["asset"] == "USDC" or item["asset"] == "USDT":
                    total = float(item["netAsset"])
                    quote_asset = {
                        "asset": item["asset"],
                        "quantity": str(total),
                        "free": item["free"],
                        "used": item["locked"],
                        "price": 1,
                    }
                    quote_assets.append(quote_asset)

            for item in inventory_state["info"]["userAssets"]:
                if item["asset"] == "USDT" or item["asset"] == "USDC":
                    continue

                total = float(item["netAsset"])
                debt = float(item["borrowed"]) + float(item["interest"])

                inventory_item = {
                    "total": str(total),
                    "used": item["locked"],
                    "free": item["free"],
                    "debt": str(debt),
                    "used_margin": "0",
                    "maint_margin": "0",
                    "asset": item["asset"],
                    "quote_conversion_symbol": item["asset"] + "USDT",
                    "exchange_name": EXCHANGE_NAME_BINANCE,
                    "market_type": MARKET_TYPE_SPOT,
                }
                inventory.append(inventory_item)
        else:
            for item in inventory_state["info"]["balances"]:
                if item["asset"] == "USDC" or item["asset"] == "USDT":
                    total = float(item["free"]) + float(item["locked"])
                    quote_asset = {
                        "asset": item["asset"],
                        "quantity": str(total),
                        "free": item["free"],
                        "used": item["locked"],
                        "price": 1,
                    }
                    quote_assets.append(quote_asset)

            for item in inventory_state["info"]["balances"]:
                if item["asset"] == "USDT" or item["asset"] == "USDC":
                    continue

                total = float(item["free"]) + float(item["locked"])
                inventory_item = {
                    "total": str(total),
                    "used": item["locked"],
                    "free": item["free"],
                    "debt": "0",
                    "used_margin": "0",
                    "maint_margin": "0",
                    "asset": item["asset"],
                    "quote_conversion_symbol": item["asset"] + "USDT",
                    "exchange_name": EXCHANGE_NAME_BINANCE,
                    "market_type": MARKET_TYPE_SPOT,
                }
                inventory.append(inventory_item)

        return {
            "quote_assets": quote_assets,
            "inventory": inventory,
        }

    def _parse_bybit_unified(self, inventory_state: Dict):
        quote_assets = []
        inventory = []

        try:
            account_data = inventory_state["info"]["result"]["list"][0]

            for coin_data in account_data["coin"]:
                if coin_data["coin"] in ["USDT", "USDC"]:
                    quote_asset = {
                        "asset": coin_data["coin"],
                        "quantity": str(coin_data["equity"]),
                        "free": str(coin_data["walletBalance"]),
                        "used": str(coin_data["locked"]),
                        "price": 1,
                    }
                    quote_assets.append(quote_asset)

            for coin_data in account_data["coin"]:
                if coin_data["coin"] in ["USDT", "USDC"]:
                    continue

                quote_symbol = "USDT"
                inventory_item = {
                    "total": str(coin_data["equity"]),
                    "used": str(coin_data["locked"]),
                    "free": str(coin_data["walletBalance"]),
                    "debt": str(coin_data["borrowAmount"]),
                    "used_margin": str(coin_data["totalPositionIM"]),
                    "maint_margin": str(coin_data["totalPositionMM"]),
                    "asset": coin_data["coin"],
                    "quote_conversion_symbol": coin_data["coin"] + quote_symbol,
                    "exchange_name": EXCHANGE_NAME_BYBIT,
                    "market_type": MARKET_TYPE_SPOT,
                }
                inventory.append(inventory_item)

        except Exception as e:
            log(f"Error parsing Bybit unified inventory state: {e}", {}, "ERROR")
            quote_assets = []
            inventory = []

        return {
            "quote_assets": quote_assets,
            "inventory": inventory,
        }

    def _parse_bybit_spot_inventory_state(self, inventory_state: Dict):
        quote_assets = []
        inventory = []

        for item in inventory_state["info"].get("userAssets", []):
            if item["asset"] == "USDT":
                quote_asset = {
                    "asset": item["asset"],
                    "quantity": item.get("netAsset", "0"),
                    "free": item.get("free", "0"),
                    "used": item.get("locked", "0"),
                    "price": 1,
                }
                quote_assets.append(quote_asset)

        for item in inventory_state["info"].get("userAssets", []):
            inventory_item = {
                "total": item.get("netAsset", "0"),
                "used": item.get("locked", "0"),
                "free": item.get("free", "0"),
                "debt": str(
                    float(item.get("borrowed", "0")) + float(item.get("interest", "0"))
                ),
                "used_margin": "0",
                "maint_margin": "0",
                "asset": item["asset"],
                "quote_conversion_symbol": item["asset"] + "USDT",
                "exchange_name": EXCHANGE_NAME_BYBIT,
                "market_type": MARKET_TYPE_SPOT,
            }
            inventory.append(inventory_item)

        return {
            "quote_assets": quote_assets,
            "inventory": inventory,
        }

    def _parse_bitget_futures_inventory_state(self, inventory_state: Dict):
        quote_assets = []
        inventory = []

        for item in inventory_state["info"].get("assets", []):
            if item["asset"] == "USDT":
                quote_asset = {
                    "asset": item["asset"],
                    "quantity": inventory_state["info"].get("totalMarginBalance", "0"),
                    "free": inventory_state["info"].get("totalMarginBalance", "0"),
                    "used": "0",
                    "price": 1,
                }
                quote_assets.append(quote_asset)

        for item in inventory_state["info"].get("positions", []):
            inventory.append(
                {
                    "total": item.get("positionAmt", "0"),
                    "used": "0",
                    "free": "0",
                    "debt": "0",
                    "used_margin": item.get("initialMargin", "0"),
                    "maint_margin": item.get("maintMargin", "0"),
                    "asset": item.get("symbol", ""),
                    "quote_conversion_symbol": item.get("symbol", ""),
                    "exchange_name": EXCHANGE_NAME_BITGET,
                    "market_type": MARKET_TYPE_SWAP,
                }
            )

        return {
            "quote_assets": quote_assets,
            "inventory": inventory,
        }

    def _parse_bitget_spot_inventory_state(self, inventory_state: Dict):
        quote_assets = []
        inventory = []

        try:
            for coin_data in inventory_state["info"]:
                if coin_data["coin"] in ["USDT", "USDC"]:
                    quote_asset = {
                        "asset": coin_data["coin"],
                        "quantity": str(coin_data["net"]),
                        "free": str(coin_data["available"]),
                        "used": str(coin_data["frozen"]),
                        "price": 1,
                    }
                    quote_assets.append(quote_asset)

            for coin_data in inventory_state["info"]:
                if coin_data["coin"] in ["USDT", "USDC"]:
                    continue

                quote_symbol = "USDT"
                inventory_item = {
                    "total": str(coin_data["net"]),
                    "used": str(coin_data["frozen"]),
                    "free": str(coin_data["available"]),
                    "debt": str(
                        float(coin_data["borrow"]) + float(coin_data["interest"])
                    ),
                    "used_margin": "0",
                    "maint_margin": "0",
                    "asset": coin_data["coin"],
                    "quote_conversion_symbol": coin_data["coin"] + quote_symbol,
                    "exchange_name": EXCHANGE_NAME_BITGET,
                    "market_type": MARKET_TYPE_SPOT,
                }
                inventory.append(inventory_item)

        except Exception as e:
            log(f"Error parsing Bitget spot inventory state: {e}", {}, "ERROR")
            quote_assets = []
            inventory = []

        return {
            "quote_assets": quote_assets,
            "inventory": inventory,
        }

    def _parse_gateio_spot_inventory_state(self, inventory_state: Dict):
        quote_assets = []
        inventory = []

        try:
            balances = inventory_state["info"]["balances"]

            for asset, balance_data in balances.items():
                if asset in ["USDT", "USDC"]:
                    quote_asset = {
                        "asset": asset,
                        "quantity": str(balance_data["equity"]),
                        "free": str(balance_data["available"]),
                        "used": str(balance_data["freeze"]),
                        "price": 1,
                    }
                    quote_assets.append(quote_asset)

            for asset, balance_data in balances.items():
                if asset in ["USDT", "USDC"]:
                    continue

                inventory_item = {
                    "total": str(balance_data["equity"]),
                    "used": str(balance_data["freeze"]),
                    "free": str(balance_data["available"]),
                    "debt": str(balance_data["borrowed"]),
                    "used_margin": "0",
                    "maint_margin": "0",
                    "asset": asset,
                    "quote_conversion_symbol": asset + "USDT",
                    "exchange_name": EXCHANGE_NAME_GATEIO,
                    "market_type": MARKET_TYPE_SPOT,
                }
                inventory.append(inventory_item)

        except Exception as e:
            log(f"Error parsing GateIO spot inventory state: {e}", {}, "ERROR")
            quote_assets = []
            inventory = []

        return {
            "quote_assets": quote_assets,
            "inventory": inventory,
        }

    def _parse_ibkr_spot_inventory_state(self, inventory_state: Dict):
        quote_assets = []
        inventory = []

        balances = inventory_state.get("info")
        if not isinstance(balances, list):
            balances = inventory_state.get("balances")
        if not isinstance(balances, list):
            balances = []

        for item in balances:
            asset = item.get("currency") or item.get("asset") or item.get("code")
            if asset not in ["USD", "USDT", "USDC"]:
                continue

            free_value = item.get("cash")
            if free_value is None:
                free_value = item.get("free", 0)
            total_value = item.get("netAsset")
            if total_value is None:
                total_value = item.get("total", free_value)
            used_value = item.get("locked")
            if used_value is None:
                used_value = item.get("used")
            if used_value is None:
                used_value = str(max(float(total_value) - float(free_value), 0.0))

            quote_assets.append(
                {
                    "asset": asset,
                    "quantity": str(total_value),
                    "free": str(free_value),
                    "used": str(used_value),
                    "price": 1,
                }
            )

        positions = inventory_state.get("info", [])
        if not isinstance(positions, list):
            positions = inventory_state.get("positions")
        if not isinstance(positions, list):
            positions = []

        for item in positions:
            symbol = item.get("symbol") or item.get("asset")
            if not symbol:
                continue

            total_value = item.get("position")
            if total_value is None:
                total_value = item.get("total")
            if total_value is None:
                total_value = item.get("free", 0)

            free_value = item.get("free")
            if free_value is None:
                free_value = total_value

            inventory.append(
                {
                    "total": str(total_value),
                    "used": str(item.get("used", 0)),
                    "free": str(free_value),
                    "debt": str(item.get("debt", 0)),
                    "used_margin": str(item.get("used_margin", 0)),
                    "maint_margin": str(item.get("maint_margin", 0)),
                    "asset": symbol,
                    "quote_conversion_symbol": f"{symbol}USDT",
                    "exchange_name": EXCHANGE_NAME_IBKR,
                    "market_type": MARKET_TYPE_SPOT,
                }
            )

        return {
            "quote_assets": quote_assets,
            "inventory": inventory,
        }

    def _parse_hyperliquid_inventory_state(self, inventory_state: Dict):
        # HL fetch_balance().info: marginSummary{accountValue,totalMarginUsed} +
        # assetPositions[].position{coin, szi (signed), marginUsed}. One-way
        # account, so a single signed row per coin (position_side BOTH).
        quote_assets = []
        inventory = []
        info = inventory_state.get("info", {})
        margin = info.get("marginSummary", {})
        account_value = margin.get("accountValue", "0")
        total_margin_used = margin.get("totalMarginUsed", "0")
        try:
            free = str(float(account_value) - float(total_margin_used))
        except (TypeError, ValueError):
            free = account_value
        quote_assets.append(
            {
                "asset": "USDC",
                "quantity": account_value,
                "free": free,
                "used": total_margin_used,
                "price": 1,
            }
        )
        for entry in info.get("assetPositions", []):
            pos = entry.get("position", {})
            coin = pos.get("coin", "")
            if not coin:
                continue
            # HL is uniformly USDC-quoted; emit the canonical base+quote symbol
            # the engine keys on (matches trade_ticker / get_order_api_symbol).
            canonical = coin + "USDC"
            inventory.append(
                {
                    "total": pos.get("szi", "0"),
                    "used": "0",
                    "free": "0",
                    "debt": "0",
                    "used_margin": pos.get("marginUsed", "0"),
                    "maint_margin": "0",
                    "asset": canonical,
                    "quote_conversion_symbol": canonical,
                    "exchange_name": EXCHANGE_NAME_HYPERLIQUID,
                    "market_type": MARKET_TYPE_SWAP,
                    "position_side": "BOTH",
                }
            )
        return {
            "quote_assets": quote_assets,
            "inventory": inventory,
        }

    def _parse_inventory_state(
        self, exchange_name: str, market_type: str, inventory_state: Dict
    ):
        if exchange_name == EXCHANGE_NAME_BINANCE:
            if market_type == MARKET_TYPE_SWAP:
                return self._parse_binance_futures_inventory_state(inventory_state)
            elif market_type == MARKET_TYPE_SPOT:
                return self._parse_binance_spot_inventory_state(inventory_state)
        elif exchange_name == EXCHANGE_NAME_BYBIT:
            return self._parse_bybit_unified(inventory_state)
        elif exchange_name == EXCHANGE_NAME_BITGET:
            if market_type == MARKET_TYPE_SWAP:
                return self._parse_bitget_futures_inventory_state(inventory_state)
            elif market_type == MARKET_TYPE_SPOT:
                return self._parse_bitget_spot_inventory_state(inventory_state)
        elif exchange_name == EXCHANGE_NAME_GATEIO:
            if market_type == MARKET_TYPE_SPOT:
                return self._parse_gateio_spot_inventory_state(inventory_state)
        elif exchange_name == EXCHANGE_NAME_IBKR:
            if market_type == MARKET_TYPE_SPOT:
                return self._parse_ibkr_spot_inventory_state(inventory_state)
        elif exchange_name == EXCHANGE_NAME_HYPERLIQUID:
            return self._parse_hyperliquid_inventory_state(inventory_state)

    async def get_inventory_state(
        self, exchange_name: str, market_type: str, account_id: int
    ):
        client = self._round_robin_rotate_exchange_client(
            exchange_name, market_type, account_id, False
        )

        if exchange_name == EXCHANGE_NAME_BITGET and market_type == MARKET_TYPE_SPOT:
            res = await client.fetch_balance(
                params={
                    "type": "margin",
                    "marginMode": "cross",
                    "productType": "cross",
                }
            )
        elif exchange_name == EXCHANGE_NAME_BINANCE and market_type == MARKET_TYPE_SPOT:
            res = await client.fetch_balance(
                params={
                    "type": "margin",
                }
            )
        elif exchange_name == EXCHANGE_NAME_GATEIO and market_type == MARKET_TYPE_SPOT:
            res = await client.fetch_balance(params={"unifiedAccount": True})
        elif exchange_name == EXCHANGE_NAME_BINANCE and market_type == MARKET_TYPE_SWAP:
            res = await client.fetch_balance()
        else:
            res = await client.fetch_balance()
        parsed_res = self._parse_inventory_state(exchange_name, market_type, res)
        # Binance futures hedge mode emits one position row per side. We used
        # to dedup-sum them into a single net entry here, which made the C++
        # inventory validator blind to gross long/short drift. Now we keep
        # both rows and rely on the C++ side-aware Inventory merge to track
        # long_total + short_total separately. See bar.cpp / inventory.h.
        return parsed_res
