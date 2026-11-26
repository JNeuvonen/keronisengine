from typing import Dict, Set

from cpp_live_proxy.ccxt_constants import EXCHANGE_NAME_IBKR, MARKET_TYPE_SPOT, MARKET_TYPE_SWAP


def _allowed_quote_assets_for(exchange_name: str) -> Set[str]:
    """Per-venue allow-list of quote assets that get_exchange_symbols_info
    surfaces. Crypto venues are stablecoin-quoted; IBKR is USD-quoted for
    US equities. Without this gate, IBKR symbols seeded into the markets
    dict were silently filtered out, leaving Account.get_active_symbols
    empty and is_using_all_symbols strategies unable to enumerate them.
    """
    if exchange_name == EXCHANGE_NAME_IBKR:
        return {"USD"}
    return {"USDT", "USDC"}


class CcxtWrapperMetadataMixin:
    def _parse_binance_exchange_info(self, exchange_info: Dict):

        ret = {
            "symbol": exchange_info["base"] + exchange_info["quote"],
            "base": exchange_info["base"],
            "quote": exchange_info["quote"],
            "is_active": exchange_info["active"],
            "price_filter": {},
            "lot_size_filter": {},
            "min_notional": 0,
        }
        info = exchange_info["info"]

        for item in info["filters"]:

            if item["filterType"] == "PRICE_FILTER":
                ret["price_filter"] = {
                    "min_price": item["minPrice"],
                    "max_price": item["maxPrice"],
                    "tick_size": item["tickSize"],
                }
            elif item["filterType"] == "LOT_SIZE":
                ret["lot_size_filter"] = {
                    "min_quantity": item["minQty"],
                    "max_quantity": item["maxQty"],
                    "tick_size": item["stepSize"],
                }
            elif item["filterType"] == "NOTIONAL":
                ret["min_notional"] = item["minNotional"]
            elif item["filterType"] == "MAX_NUM_ORDERS":
                if "maxNumOrders" in item:
                    ret["max_num_orders"] = int(item["maxNumOrders"])
        return ret

    def _get_metadata_market_type(self, exchange_name: str, info: Dict) -> str:
        info_type = info.get("type")
        if exchange_name == EXCHANGE_NAME_IBKR and info_type in {"spot", "stock"}:
            return MARKET_TYPE_SPOT
        return info_type

    def get_exchange_symbols_info(self, exchange_name: str, market_type: str):
        exchange_info = self.get_exchange_info(exchange_name, market_type)
        allowed_quotes = _allowed_quote_assets_for(exchange_name)

        ret = []
        for symbol, info in exchange_info.items():
            metadata_market_type = self._get_metadata_market_type(exchange_name, info)

            if market_type == MARKET_TYPE_SWAP:
                if info["quote"] not in allowed_quotes:
                    continue

                if metadata_market_type != MARKET_TYPE_SWAP:
                    continue
                if info["linear"] is False:
                    continue

                if info["expiry"] is not None:
                    continue

            if market_type == MARKET_TYPE_SPOT:
                if metadata_market_type != MARKET_TYPE_SPOT:
                    continue
                if info["quote"] not in allowed_quotes:
                    continue

            allows_margin = info.get("margin", False)
            native_symbol = (
                info["base"]
                if exchange_name == EXCHANGE_NAME_IBKR
                else info["base"] + info["quote"]
            )

            ret.append(
                {
                    "symbol": native_symbol,
                    "base": info["base"],
                    "quote": info["quote"],
                    "is_active": info["active"],
                    "type": metadata_market_type,
                    "allows_margin": allows_margin,
                    "precision": {
                        "price": info["precision"]["price"],
                        "amount": info["precision"]["amount"],
                        "base": info["precision"].get("base"),
                        "quote": info["precision"].get("quote"),
                    },
                    "limits": {
                        "amount": {
                            "min": info["limits"]["amount"]["min"],
                            "max": info["limits"]["amount"]["max"],
                        },
                        "price": {
                            "min": info["limits"]["price"]["min"],
                            "max": info["limits"]["price"]["max"],
                        },
                        "cost": {
                            "min": info["limits"]["cost"]["min"],
                            "max": info["limits"]["cost"]["max"],
                        },
                    },
                }
            )

        return ret

    def get_exchange_info(self, exchange_name: str, market_type: str):
        for key, clients in self.exchange_clients.items():
            for exchange_client in clients:
                if (
                    exchange_name == exchange_client.exchange_name
                    and market_type == exchange_client.market_type
                    and exchange_client.is_ready
                ):
                    return exchange_client.client.markets

        raise ValueError(
            f"Invalid exchange name and market type: {exchange_name} {market_type}"
        )

    def convert_ccxt_symbol_to_std_format(self, symbol: str):
        return symbol.replace("/", "").split(":")[0]

    def get_symbols(
        self,
        exchange_name: str,
        market_type: str,
        active: bool = False,
        format_to_std: bool = True,
    ):
        # Live endpoint: GET /api/v1/ccxt/get-symbols and /get-active-symbols.
        # The C++ trading engine (src/cpp/.../ccxt_proxy.cpp) hits these at
        # startup and on symbol-list refresh; route handlers in
        # api/v1/ccxt/metadata.py call ccxt_wrapper.get_symbols() here.
        # Removing this method (PR #1228 mixin refactor regression) silently
        # broke every symbols query — restored from the pre-refactor
        # implementation byte-for-byte.
        exchange_info = self.get_exchange_info(exchange_name, market_type)

        filtered_markets = {}
        for symbol, market in exchange_info.items():
            if market_type == MARKET_TYPE_SWAP:
                if (
                    not market["swap"]
                    or not market["contract"]
                    or market.get("expiry") is not None
                ):
                    continue
            elif market_type == MARKET_TYPE_SPOT:
                if market["swap"] or market["contract"]:
                    continue

            if active and not market["active"]:
                continue

            filtered_markets[symbol] = market

        if format_to_std:
            return [
                self.convert_ccxt_symbol_to_std_format(symbol)
                for symbol in filtered_markets.keys()
            ]
        return list(filtered_markets.keys())
