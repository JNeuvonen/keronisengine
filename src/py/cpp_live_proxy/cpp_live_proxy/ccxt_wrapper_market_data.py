import hashlib
import hmac
import time
from typing import List, Optional

from fastapi import HTTPException

from cpp_live_proxy.ccxt_constants import (
    EXCHANGE_NAME_BINANCE,
    EXCHANGE_NAME_BITGET,
    EXCHANGE_NAME_BYBIT,
    EXCHANGE_NAME_GATEIO,
    MARKET_TYPE_SPOT,
    MARKET_TYPE_SWAP,
)
from cpp_live_proxy.ccxt_helpers import (
    convert_interval_str_to_bitget_granularity,
    convert_interval_str_to_minutes,
    convert_interval_str_to_ms,
)
from cpp_live_proxy.audit_trace import maybe_trace_configs


class ExchangeRateLimitError(Exception):
    def __init__(self, exchange_name: str, status: int, retry_after_s: int = 60):
        self.exchange_name = exchange_name
        self.status = status
        self.retry_after_s = retry_after_s
        super().__init__(f"{exchange_name} rate limited (HTTP {status}), cooldown {retry_after_s}s")


class CcxtWrapperMarketDataMixin:
    async def fetch_tickers(self, exchange_name: str, market_type: str):
        client = self._round_robin_rotate_exchange_client(
            exchange_name, market_type, None, True
        )
        return await client.fetch_tickers()

    async def get_full_orderbook(
        self, symbol: str, exchange_name: str, market_type: str, limit: int = 1000
    ):
        client = self._round_robin_rotate_exchange_client(
            exchange_name, market_type, None, True
        )
        if exchange_name == EXCHANGE_NAME_GATEIO:
            symbol = symbol.replace("/", "_")
        return await client.fetch_order_book(symbol, limit=limit)

    async def get_historical_trades(
        self,
        symbol: str,
        exchange_name: str,
        market_type: str,
        from_timestamp: Optional[int] = None,
        trade_id: Optional[str] = None,
    ):
        import aiohttp

        rate_limit_key = f"{exchange_name}:{market_type}"
        cooldown_until = self._rate_limit_until.get(rate_limit_key, 0)
        if time.time() < cooldown_until:
            remaining = int(cooldown_until - time.time())
            raise ExchangeRateLimitError(exchange_name, 429, remaining)

        if exchange_name == EXCHANGE_NAME_BINANCE:
            # Always serve RAW individual trades from /historicalTrades — never
            # the /aggTrades aggregate endpoint, so strategies that consume
            # this path get tick-level data for order-flow features.
            #
            # Three call shapes:
            #   trade_id given        → /historicalTrades?fromId=<id>          (resume from cursor)
            #   from_timestamp given  → /aggTrades to resolve ts→id, then     (rare; bootstrap by time)
            #                            /historicalTrades?fromId=<id>
            #   neither given         → /historicalTrades (no fromId)          (bootstrap — latest 500)
            if market_type == MARKET_TYPE_SWAP:
                base_url = "https://fapi.binance.com/fapi/v1/historicalTrades"
                limit = 500
            elif market_type == MARKET_TYPE_SPOT:
                base_url = "https://api.binance.com/api/v3/historicalTrades"
                limit = 1000
            else:
                raise ValueError(
                    f"binance market_type must be spot or swap, got {market_type}"
                )

            # MARKET_DATA endpoint — requires a real X-MBX-APIKEY but no
            # signing. Public clients have api_key="" — fapi rejects them
            # with -2014; spot is lenient and accepts the empty header.
            # Prefer a private client (works for both), fall back to
            # public for spot deployments without a private spot account.
            try:
                client = self._pick_any_private_client(
                    exchange_name, market_type
                )
            except ValueError:
                client = self._round_robin_rotate_exchange_client(
                    exchange_name, market_type, None, True
                )
            headers = {"X-MBX-APIKEY": client.apiKey}
            params = {"symbol": symbol, "limit": limit}

            # Caller MUST supply either trade_id or a non-zero from_timestamp.
            # Returning trades without a cursor would silently return Binance's
            # "latest 500" which makes the C++ bar sync instant-match the WS
            # cutoff and skip the disk→WS gap. Fail loud instead.
            if trade_id is not None:
                params["fromId"] = trade_id
            elif from_timestamp is not None and from_timestamp > 0:
                # Translate ts → fromId via a single aggTrades probe, then
                # serve individual trades from that id. aggTrades is used
                # ONLY as a ts→id lookup, not as the returned data.
                if market_type == MARKET_TYPE_SWAP:
                    probe_url = "https://fapi.binance.com/fapi/v1/aggTrades"
                else:
                    probe_url = "https://api.binance.com/api/v3/aggTrades"
                async with aiohttp.ClientSession(trace_configs=maybe_trace_configs()) as probe_session:
                    async with probe_session.get(
                        probe_url,
                        params={
                            "symbol": symbol,
                            "startTime": from_timestamp,
                            "limit": 1,
                        },
                    ) as probe_resp:
                        if probe_resp.status == 200:
                            probe_data = await probe_resp.json()
                            if probe_data:
                                # 'f' = first individual trade id in this agg
                                params["fromId"] = str(probe_data[0]["f"])
                if "fromId" not in params:
                    raise HTTPException(
                        status_code=400,
                        detail=(
                            f"could not resolve from_timestamp={from_timestamp} "
                            f"to a trade_id for {symbol} on {exchange_name} {market_type}"
                        ),
                    )
            else:
                raise HTTPException(
                    status_code=400,
                    detail=(
                        "historical-data/trades requires either trade_id or "
                        "from_timestamp>0; silently returning 'latest 500' "
                        "trades was a footgun that broke bar gap-fill sync."
                    ),
                )

            print(f"Making request to {base_url} with params: {params}")
            async with aiohttp.ClientSession(trace_configs=maybe_trace_configs()) as session:
                async with session.get(
                    base_url, params=params, headers=headers
                ) as response:
                    if response.status == 200:
                        trades = await response.json()
                        print(f"Received {len(trades)} trades")
                        if trades:
                            print(f"First trade: {trades[0]}")
                        return trades
                    else:
                        error_text = await response.text()
                        print(f"Error response: {error_text}")
                        if response.status in (429, 418):
                            rate_limit_key = f"{exchange_name}:{market_type}"
                            self._rate_limit_until[rate_limit_key] = time.time() + self.RATE_LIMIT_COOLDOWN_S
                            self._log_rate_limit_hit(
                                exchange_name=exchange_name,
                                market_type=market_type,
                                http_status=response.status,
                                cooldown_s=self.RATE_LIMIT_COOLDOWN_S,
                            )
                            print(f"{rate_limit_key} rate limited (HTTP {response.status}), blocking for {self.RATE_LIMIT_COOLDOWN_S}s")
                            raise ExchangeRateLimitError(exchange_name, response.status)
                        raise Exception(
                            f"API request failed with status {response.status}: {error_text}"
                        )

        elif exchange_name == EXCHANGE_NAME_GATEIO:
            if market_type == MARKET_TYPE_SPOT:
                base_url = "https://api.gateio.ws/api/v4/spot/trades"
                gateio_symbol = symbol
                if "_" not in symbol and "USDT" in symbol:
                    gateio_symbol = symbol.replace("USDT", "_USDT")

                if trade_id is not None:
                    params = {
                        "currency_pair": gateio_symbol,
                        "limit": 1000,
                        "last_id": trade_id,
                    }
                    async with aiohttp.ClientSession(trace_configs=maybe_trace_configs()) as session:
                        async with session.get(base_url, params=params) as response:
                            if response.status != 200:
                                error_text = await response.text()
                                raise Exception(f"API request failed: {error_text}")
                            trades = await response.json()

                    trades.sort(key=lambda x: float(x["create_time"]))
                    return trades

                elif from_timestamp is not None:
                    now_ms = int(time.time() * 1000)
                    if from_timestamp < now_ms - (25 * 24 * 60 * 60 * 1000):
                        return []

                    from_sec = from_timestamp // 1000
                    to_sec = from_sec + 900

                    async def fetch_trades_recursive(from_s: int, to_s: int) -> list:
                        params = {
                            "currency_pair": gateio_symbol,
                            "limit": 1000,
                            "from": from_s,
                            "to": to_s,
                        }
                        async with aiohttp.ClientSession(trace_configs=maybe_trace_configs()) as session:
                            async with session.get(base_url, params=params) as response:
                                if response.status != 200:
                                    error_text = await response.text()
                                    raise Exception(f"API request failed: {error_text}")
                                trades = await response.json()

                        if len(trades) < 1000:
                            return trades

                        mid = (from_s + to_s) // 2
                        first_half = await fetch_trades_recursive(from_s, mid)
                        second_half = await fetch_trades_recursive(mid, to_s)
                        return first_half + second_half

                    trades = await fetch_trades_recursive(from_sec, to_sec)
                    trades.sort(key=lambda x: float(x["create_time"]))
                    return trades

    async def get_historical_ohlcv(
        self,
        symbol: str,
        exchange_name: str,
        market_type: str,
        interval: str,
        from_timestamp: Optional[int] = None,
        limit: Optional[int] = None,
    ):
        exchange_client = self._round_robin_rotate_exchange_client(
            exchange_name, market_type, None, True
        )

        effective_limit = limit if limit is not None else 1000

        if exchange_name == EXCHANGE_NAME_BINANCE:
            if market_type == MARKET_TYPE_SWAP:
                base_url = "https://fapi.binance.com/fapi/v1/klines"
            elif market_type == MARKET_TYPE_SPOT:
                base_url = "https://api.binance.com/api/v3/klines"

            params = {
                "symbol": symbol,
                "interval": interval,
                "limit": effective_limit,
            }
            if from_timestamp is not None:
                params["startTime"] = from_timestamp

            query_string = "&".join([f"{k}={v}" for k, v in params.items()])
            full_url = f"{base_url}?{query_string}"

            return await exchange_client.fetch(full_url)
        elif exchange_name == EXCHANGE_NAME_BYBIT:
            base_url = "https://api.bybit.com/v5/market/kline"

            params = {
                "category": "linear" if market_type == MARKET_TYPE_SWAP else "spot",
                "symbol": symbol,
                "interval": convert_interval_str_to_minutes(interval),
                "limit": 1000,
            }
            if from_timestamp is not None:
                params["start"] = from_timestamp

            query_string = "&".join([f"{k}={v}" for k, v in params.items()])
            full_url = f"{base_url}?{query_string}"

            response = await exchange_client.fetch(full_url)

            if "result" in response and "list" in response["result"]:
                transformed_data = []
                interval_ms = convert_interval_str_to_ms(interval)

                candles = list(reversed(response["result"]["list"]))

                for candle in candles:
                    quote_volume = float(candle[6]) if len(candle) > 6 else 0.0

                    transformed_candle = [
                        candle[0],
                        candle[1],
                        candle[2],
                        candle[3],
                        candle[4],
                        candle[5],
                        str(int(candle[0]) + interval_ms),
                        str(quote_volume),
                        "0",
                        "0",
                        "0",
                    ]
                    transformed_data.append(transformed_candle)

                return transformed_data
            else:
                return []
        elif exchange_name == EXCHANGE_NAME_BITGET:
            if market_type == MARKET_TYPE_SWAP:
                base_url = "https://api.bitget.com/api/v2/mix/market/candles"
            elif market_type == MARKET_TYPE_SPOT:
                base_url = "https://api.bitget.com/api/v2/spot/market/candles"

            params = {
                "symbol": symbol,
                "granularity": (
                    interval
                    if market_type == MARKET_TYPE_SPOT
                    else convert_interval_str_to_bitget_granularity(interval)
                ),
                "limit": 1000,
            }
            if from_timestamp is not None:
                params["startTime"] = from_timestamp

            if market_type == MARKET_TYPE_SWAP:
                params["productType"] = "USDT-FUTURES"

            query_string = "&".join([f"{k}={v}" for k, v in params.items()])
            full_url = f"{base_url}?{query_string}"

            response = await exchange_client.fetch(full_url)

            if "data" in response:
                transformed_data = []
                interval_ms = convert_interval_str_to_ms(interval)

                for candle in response["data"]:
                    timestamp = int(candle[0])
                    open_price = candle[1]
                    high_price = candle[2]
                    low_price = candle[3]
                    close_price = candle[4]
                    volume = candle[5]
                    quote_volume = candle[6] if len(candle) > 6 else "0"
                    trade_count = candle[7] if len(candle) > 7 else "0"

                    transformed_candle = [
                        str(timestamp),
                        open_price,
                        high_price,
                        low_price,
                        close_price,
                        volume,
                        str(timestamp + interval_ms),
                        quote_volume,
                        trade_count,
                        "0",
                        "0",
                    ]
                    transformed_data.append(transformed_candle)

                return transformed_data
            else:
                return []
        elif exchange_name == EXCHANGE_NAME_GATEIO:
            base_url = "https://api.gateio.ws/api/v4/spot/candlesticks"

            gateio_symbol = symbol
            if "_" not in symbol and "USDT" in symbol:
                gateio_symbol = symbol.replace("USDT", "_USDT")

            params = {
                "currency_pair": gateio_symbol,
                "interval": interval,
                "limit": 1000,
            }
            if from_timestamp is not None:
                params["from"] = from_timestamp // 1000

            query_string = "&".join([f"{k}={v}" for k, v in params.items()])
            full_url = f"{base_url}?{query_string}"

            response = await exchange_client.fetch(full_url)

            if response and isinstance(response, list):
                transformed_data = []
                interval_ms = convert_interval_str_to_ms(interval)

                for candle in response:
                    timestamp_s = int(candle[0])
                    timestamp_ms = timestamp_s * 1000
                    volume = candle[1]
                    close_price = candle[2]
                    high_price = candle[3]
                    low_price = candle[4]
                    open_price = candle[5]
                    quote_volume = candle[6] if len(candle) > 6 else "0"

                    transformed_candle = [
                        str(timestamp_ms),
                        open_price,
                        high_price,
                        low_price,
                        close_price,
                        volume,
                        str(timestamp_ms + interval_ms),
                        quote_volume,
                        "0",
                        "0",
                        "0",
                    ]
                    transformed_data.append(transformed_candle)

                return transformed_data
            else:
                return []

    async def query_swap_symbol_leverage_level(
        self,
        exchange_name: str,
        market_type: str,
        account_id: int,
    ):
        try:
            client = self._round_robin_rotate_exchange_client(
                exchange_name, market_type, account_id, False
            )
            res = await client.fetch_leverages()
            return res
        except Exception:
            return []

    async def fetch_borrowable_inventories(
        self,
        exchange_name: str,
        market_type: str,
        account_id: int,
        symbols: List[str],
    ):
        import aiohttp

        if not symbols:
            return []

        client = self._round_robin_rotate_exchange_client(
            exchange_name, market_type, account_id, False
        )

        def extract_base_currency(symbol: str) -> str:
            quote_assets = ["USDT", "USDC", "BTC", "ETH", "BNB", "BUSD", "SOL"]
            symbol_upper = symbol.upper()
            for quote in quote_assets:
                if symbol_upper.endswith(quote):
                    return symbol_upper[: -len(quote)]
            return symbol_upper

        currencies = [extract_base_currency(sym) for sym in symbols]
        currencies = list(set(currencies))

        results = []

        if exchange_name == EXCHANGE_NAME_GATEIO and market_type == MARKET_TYPE_SPOT:
            api_key = client.apiKey
            api_secret = client.secret

            if not api_key or not api_secret:
                raise ValueError("Gate.io API credentials not found")

            host = "https://api.gateio.ws"
            prefix = "/api/v4"
            url = "/unified/batch_borrowable"
            query_param = "currencies=" + ",".join(currencies)

            t = time.time()
            m = hashlib.sha512()
            m.update("".encode("utf-8"))
            hashed_payload = m.hexdigest()
            s = "%s\n%s\n%s\n%s\n%s" % (
                "GET",
                prefix + url,
                query_param,
                hashed_payload,
                t,
            )
            sign = hmac.new(
                api_secret.encode("utf-8"), s.encode("utf-8"), hashlib.sha512
            ).hexdigest()

            headers = {
                "Accept": "application/json",
                "Content-Type": "application/json",
                "KEY": api_key,
                "Timestamp": str(t),
                "SIGN": sign,
            }

            full_url = host + prefix + url + "?" + query_param

            session = getattr(client, "session", None)
            if session:
                async with session.get(full_url, headers=headers) as response:
                    response.raise_for_status()
                    batch_results = await response.json()
            else:
                async with aiohttp.ClientSession(trace_configs=maybe_trace_configs()) as session:
                    async with session.get(full_url, headers=headers) as response:
                        response.raise_for_status()
                        batch_results = await response.json()

            for item in batch_results:
                currency = item.get("currency", "")
                if currency in currencies:
                    results.append(
                        {
                            "currency": currency,
                            "available": item.get("amount", "0"),
                        }
                    )

        return results
