import asyncio
import time
from typing import List

from fastapi import APIRouter, HTTPException, Query

from cpp_live_proxy.api.v1.ccxt._shared import (
    ASTER_URL,
    BACKEND,
    Routes,
    ccxt_wrapper,
    polymarket_wrapper,
)
from cpp_live_proxy.aster_wrapper import AsterWrapper
from cpp_live_proxy.gcp_log import log
from cpp_live_proxy.us_equity_catalog import (
    US_EQUITY_EXCHANGES,
    US_EQUITY_MARKET_TYPES,
    USEquityCatalog,
)
from cpp_live_proxy._compat import GateioUtils

router = APIRouter()


def _us_equity_catalog(exchange_name: str, market_type: str):
    if exchange_name not in US_EQUITY_EXCHANGES:
        return None
    if market_type not in US_EQUITY_MARKET_TYPES:
        raise HTTPException(
            status_code=400,
            detail=f"Unsupported market_type for US equity: {market_type}",
        )
    return USEquityCatalog.get_instance()


@router.get(Routes.EXCHANGE_INFO)
async def get_exchange_info(exchange_name: str, market_type: str):
    try:
        catalog = _us_equity_catalog(exchange_name, market_type)
        if catalog is not None:
            return catalog.exchange_info_rows(market_type)

        if BACKEND is not None and exchange_name not in ("polymarket", "aster"):
            return await BACKEND.get_exchange_info(exchange_name, market_type)

        if exchange_name == "aster":
            import httpx
            async with httpx.AsyncClient(timeout=15) as client:
                r = await client.get(f"{ASTER_URL}/fapi/v3/exchangeInfo")
                data = r.json()
            results = []
            for s in data.get("symbols", []):
                if s.get("status") != "TRADING":
                    continue
                results.append({
                    "symbol": s["symbol"],
                    "base": s["baseAsset"],
                    "quote": s["quoteAsset"],
                    "is_active": True,
                    "type": "swap",
                    "limits": {
                        "amount": {"min": float(next((f["minQty"] for f in s.get("filters", []) if f["filterType"] == "LOT_SIZE"), 0)),
                                   "max": float(next((f["maxQty"] for f in s.get("filters", []) if f["filterType"] == "LOT_SIZE"), 1e9))},
                        "price": {"min": float(next((f["minPrice"] for f in s.get("filters", []) if f["filterType"] == "PRICE_FILTER"), 0)),
                                  "max": float(next((f["maxPrice"] for f in s.get("filters", []) if f["filterType"] == "PRICE_FILTER"), 1e9))},
                        "cost": {"min": float(next((f["notional"] for f in s.get("filters", []) if f["filterType"] == "MIN_NOTIONAL"), 0)),
                                 "max": None},
                    },
                    "precision": {
                        "price": float(next((f["tickSize"] for f in s.get("filters", []) if f["filterType"] == "PRICE_FILTER"), 0.0001)),
                        "amount": float(next((f["stepSize"] for f in s.get("filters", []) if f["filterType"] == "LOT_SIZE"), 0.1)),
                        "base": None,
                        "quote": None,
                    },
                })
            return results
        markets = ccxt_wrapper.get_exchange_symbols_info(exchange_name, market_type)
        return markets
    except HTTPException:
        raise
    except Exception as e:
        log(
            f"Error in {Routes.EXCHANGE_INFO}: {str(e)}",
            {"exchange_name": exchange_name, "market_type": market_type},
            "ERROR",
        )
        raise HTTPException(status_code=500, detail=str(e))


@router.get(Routes.GET_INVENTORY_STATE)
async def get_inventory_state(exchange_name: str, market_type: str, account_id: int):
    try:
        if BACKEND is not None and exchange_name not in ("polymarket", "aster"):
            return await BACKEND.get_inventory_state(
                exchange_name, market_type, account_id)

        if exchange_name == "polymarket":
            balances = await asyncio.to_thread(
                polymarket_wrapper.get_eoa_balances, account_id
            )
            # Held inventory from on-chain CTF balances. The data-api /positions
            # nets balanced complete sets to invisible, so minted-but-unsold
            # inventory would not appear; on-chain balanceOf is the truth.
            positions = await asyncio.to_thread(
                polymarket_wrapper.get_onchain_inventory, account_id
            )

            inventory_items = []
            for pos in positions:
                size = float(pos.get("size", 0))
                if size == 0:
                    continue
                condition_id = pos.get("asset", "")
                outcome = pos.get("outcome", "")
                inventory_items.append(
                    {
                        "asset": condition_id,
                        "total": str(size),
                        "used": "0",
                        "free": str(size),
                        "debt": "0",
                        "used_margin": "0",
                        "maint_margin": "0",
                        "quote_conversion_symbol": outcome,
                        "exchange_name": "polymarket",
                        "market_type": "spot",
                    }
                )

            # v2 uses both: pUSD (order settlement) + USDC.e (minting). Both are
            # 1:1 USD, so report the combined balance as the single "USDC" quote
            # the engine keys on.
            collateral_balance = float(balances.get("pusd_balance", 0)) + float(
                balances.get("usdc_balance", 0)
            )
            return {
                "inventory_state": {
                    "quote_assets": [
                        {
                            "asset": "USDC",
                            "quantity": str(collateral_balance),
                            "free": str(collateral_balance),
                            "used": "0",
                            "price": 1,
                        }
                    ],
                    "inventory": inventory_items,
                }
            }

        if exchange_name == "aster":
            aster = AsterWrapper()
            balances = await aster.get_balance(account_id)
            positions = await aster.get_positions(account_id)

            quote_assets = []
            for b in balances:
                bal = float(b.get("balance", 0))
                if bal == 0:
                    continue
                quote_assets.append({
                    "asset": b["asset"],
                    "quantity": b["balance"],
                    "free": b.get("availableBalance", b["balance"]),
                    "used": str(bal - float(b.get("availableBalance", bal))),
                    "price": 1 if b["asset"] == "USDT" else 0,
                })

            inventory_items = []
            for pos in positions:
                amt = float(pos.get("positionAmt", 0))
                if amt == 0:
                    continue
                inventory_items.append({
                    "asset": pos["symbol"],
                    "total": pos["positionAmt"],
                    "used": "0",
                    "free": pos["positionAmt"],
                    "debt": "0",
                    "used_margin": pos.get("initialMargin", "0"),
                    "maint_margin": pos.get("maintMargin", "0"),
                    "quote_conversion_symbol": pos["symbol"],
                    "exchange_name": "aster",
                    "market_type": "swap",
                })

            return {
                "inventory_state": {
                    "quote_assets": quote_assets,
                    "inventory": inventory_items,
                }
            }

        try:
            res = await ccxt_wrapper.get_inventory_state(
                exchange_name, market_type, account_id
            )
        except ValueError as e:
            # "No client found for key ..." — venue init never registered a
            # client (e.g. IBKR Gateway not running locally). Return an
            # empty inventory instead of 500 so the engine treats the venue
            # as a zero-balance shell rather than a hard failure.
            if "No client found for key" in str(e):
                return {
                    "inventory_state": {
                        "quote_assets": [],
                        "inventory": [],
                    },
                    "client_unavailable": True,
                }
            raise
        return {
            "inventory_state": res,
        }
    except Exception as e:
        log(
            f"Error in {Routes.GET_INVENTORY_STATE}: {str(e)}",
            {
                "exchange_name": exchange_name,
                "market_type": market_type,
                "account_id": account_id,
            },
            "ERROR",
        )
        raise HTTPException(status_code=500, detail=str(e))


@router.get(Routes.GET_SYMBOLS)
async def get_symbols(exchange_name: str, market_type: str):
    try:
        catalog = _us_equity_catalog(exchange_name, market_type)
        if catalog is not None:
            return {"data": catalog.symbol_names(active_only=False)}

        if BACKEND is not None and exchange_name not in ("polymarket", "aster"):
            return {"data": await BACKEND.get_symbols(exchange_name, market_type)}

        if exchange_name == "gateio" and market_type == "spot":
            symbols = GateioUtils.get_cross_margin_usdt_pairs(
                use_gateio_format=False
            )
        else:
            symbols = ccxt_wrapper.get_symbols(
                exchange_name, market_type, active=True, format_to_std=True
            )
        return {
            "data": symbols,
        }
    except HTTPException:
        raise
    except Exception as e:
        log(
            f"Error in {Routes.GET_SYMBOLS}: {str(e)}",
            {"exchange_name": exchange_name, "market_type": market_type},
            "ERROR",
        )
        raise HTTPException(status_code=500, detail=str(e))


@router.get(Routes.GET_ACTIVE_SYMBOLS)
async def get_active_symbols(exchange_name: str, market_type: str):
    try:
        catalog = _us_equity_catalog(exchange_name, market_type)
        if catalog is not None:
            return {"data": catalog.symbol_names(active_only=True)}

        if BACKEND is not None and exchange_name not in ("polymarket", "aster"):
            return {"data": await BACKEND.get_active_symbols(
                exchange_name, market_type)}

        if exchange_name == "gateio" and market_type == "spot":
            symbols = GateioUtils.get_cross_margin_usdt_pairs(
                use_gateio_format=False
            )
        else:
            symbols = ccxt_wrapper.get_symbols(
                exchange_name, market_type, active=True, format_to_std=True
            )
        return {
            "data": symbols,
        }
    except HTTPException:
        raise
    except Exception as e:
        print(str(e))
        log(
            f"Error in {Routes.GET_ACTIVE_SYMBOLS}: {str(e)}",
            {"exchange_name": exchange_name, "market_type": market_type},
            "ERROR",
        )
        raise HTTPException(status_code=500, detail=str(e))


@router.get(Routes.FETCH_PRICES)
async def fetch_prices(exchange_name: str, market_type: str):
    try:
        if BACKEND is not None and exchange_name not in ("polymarket", "aster"):
            return {"data": await BACKEND.fetch_prices(
                exchange_name, market_type)}

        tickers = await ccxt_wrapper.fetch_tickers(exchange_name, market_type)

        prices = []
        for symbol, ticker_data in tickers.items():
            if (
                ticker_data
                and "last" in ticker_data
                and ticker_data["last"] is not None
            ):
                prices.append({"symbol": symbol, "price": ticker_data["last"]})

        return {
            "data": prices,
        }
    except Exception as e:
        print(str(e))
        log(
            f"Error in {Routes.FETCH_PRICES}: {str(e)}",
            {"exchange_name": exchange_name, "market_type": market_type},
            "ERROR",
        )
        raise HTTPException(status_code=500, detail=str(e))


@router.get(Routes.GET_SERVER_TIME)
async def get_server_time(exchange_name: str, market_type: str):
    try:
        if BACKEND is not None and exchange_name not in ("polymarket", "aster"):
            return await BACKEND.get_server_time(exchange_name, market_type)

        client = ccxt_wrapper._round_robin_rotate_exchange_client(
            exchange_name, market_type, None, True
        )

        server_time = await client.fetch_time()
        local_time = int(time.time() * 1000)

        start_time = time.time()
        server_time = await client.fetch_time()
        end_time = time.time()
        latency_ms = int((end_time - start_time) * 1000)

        local_time = int(time.time() * 1000)

        return {
            "server_time": server_time,
            "local_time": local_time,
            "time_diff_ms": server_time - local_time,
            "request_latency_ms": latency_ms,
        }
    except Exception as e:
        print(str(e))
        log(
            f"Error in {Routes.GET_SERVER_TIME}: {str(e)}",
            {"exchange_name": exchange_name, "market_type": market_type},
            "ERROR",
        )
        raise HTTPException(status_code=500, detail=str(e))


@router.get(Routes.TEST_IP_ROTATION)
async def test_ip_rotation(
    exchange_name: str,
    market_type: str,
    account_id: int,
    is_public: bool,
):
    try:
        if BACKEND is not None and exchange_name not in ("polymarket", "aster"):
            res = await BACKEND.test_ip_rotation(
                exchange_name, market_type, account_id, is_public)
            return {"data": res}

        res = await ccxt_wrapper.test_ip_rotation(
            exchange_name,
            market_type,
            None if account_id == -1 else account_id,
            is_public,
        )
        return {
            "data": res,
        }
    except Exception as e:
        log(
            f"Error in {Routes.TEST_IP_ROTATION}: {str(e)}",
            {
                "exchange_name": exchange_name,
                "market_type": market_type,
                "account_id": account_id,
                "is_public": is_public,
            },
            "ERROR",
        )
        raise HTTPException(status_code=500, detail=str(e))


@router.get(Routes.GET_FULL_ORDERBOOK)
async def get_full_orderbook(
    symbol: str, exchange_name: str, market_type: str, limit: int = 1000
):
    try:
        if BACKEND is not None and exchange_name not in ("polymarket", "aster"):
            res = await BACKEND.get_full_orderbook(
                symbol, exchange_name, market_type, limit)
            return {"data": res}

        res = await ccxt_wrapper.get_full_orderbook(
            symbol, exchange_name, market_type, limit
        )
        return {
            "data": res,
        }
    except Exception as e:
        log(
            f"Error in {Routes.GET_FULL_ORDERBOOK}: {str(e)}",
            {
                "symbol": symbol,
                "exchange_name": exchange_name,
                "market_type": market_type,
            },
            "ERROR",
        )
        raise HTTPException(status_code=500, detail=str(e))


@router.get(Routes.FETCH_BORROWABLE_INVENTORIES)
async def fetch_borrowable_inventories(
    exchange_name: str = Query(...),
    market_type: str = Query(...),
    account_id: int = Query(...),
    symbols: List[str] = Query(...),
):
    try:
        if BACKEND is not None and exchange_name not in ("polymarket", "aster"):
            res = await BACKEND.fetch_borrowable_inventories(
                exchange_name, market_type, account_id, symbols)
            return {"data": res}

        res = await ccxt_wrapper.fetch_borrowable_inventories(
            exchange_name, market_type, account_id, symbols
        )
        return {"data": res}
    except Exception as e:
        log(
            f"Error in {Routes.FETCH_BORROWABLE_INVENTORIES}: {str(e)}",
            {
                "exchange_name": exchange_name,
                "market_type": market_type,
                "account_id": account_id,
                "symbols": symbols,
            },
            "ERROR",
        )
        raise HTTPException(status_code=500, detail=str(e))
