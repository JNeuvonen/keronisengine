import ccxt
import asyncio

from fastapi import APIRouter, HTTPException, Query, Response, status
from fastapi.responses import JSONResponse
from typing import List

from cpp_live_proxy.api.v1.ccxt._shared import (
    BACKEND,
    PREVENT_ORDER_CREATION,
    Routes,
    _is_order_expired,
    ccxt_wrapper,
    polymarket_wrapper,
)
from cpp_live_proxy.aster_wrapper import AsterWrapper
from cpp_live_proxy.gcp_log import log
from cpp_live_proxy.type_models import Order

router = APIRouter()


@router.get("/ping")
async def ping():
    return Response(status_code=status.HTTP_200_OK, content="OK")


@router.post(Routes.CREATE_ORDER)
async def create_order(body: Order):
    try:
        if PREVENT_ORDER_CREATION:
            return Response(status_code=status.HTTP_200_OK, content="OK")

        if BACKEND is not None and not body.is_prediction_market_order:
            result = await BACKEND.create_order(body)
            return JSONResponse(content=result.model_dump())

        if body.is_prediction_market_order:
            result = await polymarket_wrapper.create_order_raw(body)
            return JSONResponse(content=result)
        else:
            result = await ccxt_wrapper.create_order(body)
            return JSONResponse(content=result.model_dump())
    except Exception as e:
        print(str(e))
        log(
            f"Error in {Routes.CREATE_ORDER}: {str(e)}",
            {"body": body.model_dump()},
            "ERROR",
        )
        raise HTTPException(status_code=500, detail=str(e))


@router.post(Routes.CREATE_ORDERS_BULK)
async def create_orders_bulk(body: List[Order]):
    try:
        if PREVENT_ORDER_CREATION:
            return JSONResponse(content=[])

        valid_orders = []
        expired_indices = []
        for i, order in enumerate(body):
            if _is_order_expired(order):
                expired_indices.append(i)
            else:
                valid_orders.append(order)

        expired_result = {
            "success": False,
            "errorMsg": "order expired server-side",
            "orderID": "",
        }

        if not valid_orders:
            return JSONResponse(content=[expired_result for _ in expired_indices])

        valid_results = await polymarket_wrapper.create_orders_bulk(valid_orders)

        results = []
        valid_idx = 0
        for i in range(len(body)):
            if i in expired_indices:
                results.append(expired_result)
            else:
                results.append(valid_results[valid_idx])
                valid_idx += 1

        return JSONResponse(content=results)
    except Exception as e:
        import traceback

        print(f"[bulk_create] EXCEPTION ({type(e).__name__}): {e}")
        for i, o in enumerate(body):
            print(
                f"  [{i}] clob_id={o.clob_id} side={o.side} price={o.price} qty={o.quantity} type={o.type}"
            )
        traceback.print_exc()
        log(
            f"Error in {Routes.CREATE_ORDERS_BULK}: {str(e)}",
            {"order_count": len(body)},
            "ERROR",
        )
        raise HTTPException(status_code=500, detail=str(e))


@router.post(Routes.CANCEL_ORDERS_BULK)
async def cancel_orders_bulk(body: List[str], account_id: int = Query(0)):
    try:
        results = await polymarket_wrapper.cancel_orders_bulk(body, account_id)
        return JSONResponse(content=results)
    except Exception as e:
        print(str(e))
        log(
            f"Error in {Routes.CANCEL_ORDERS_BULK}: {str(e)}",
            {"order_count": len(body)},
            "ERROR",
        )
        raise HTTPException(status_code=500, detail=str(e))


@router.post(Routes.CANCEL_ORDER)
async def cancel_order(body: Order):
    try:
        if BACKEND is not None and not body.is_prediction_market_order:
            return await BACKEND.cancel_order(body)

        if body.is_prediction_market_order:
            asyncio.create_task(
                polymarket_wrapper.cancel_orders_bulk(
                    [body.exchange_order_id], body.account_id
                )
            )
            return Response(status_code=status.HTTP_200_OK, content="OK")

        # Binance/ccxt: synchronous — return the cancel round-trip result
        # ({"status": "cancelled"|"gone"|"error"}) so the C++ caller can resolve
        # engine state without waiting on the user-data WS.
        return await ccxt_wrapper.cancel_order(body)
    except Exception as e:
        print(str(e))
        log(
            f"Error in {Routes.CANCEL_ORDER}: {str(e)}",
            {"body": body.model_dump()},
            "ERROR",
        )
        raise HTTPException(status_code=500, detail=str(e))


@router.post(Routes.CANCEL_ORDER_V2)
async def cancel_order_v2(
    client_order_id: str,
    exchange_name: str,
    market_type: str,
    symbol: str,
    account_id: int,
):
    try:
        if BACKEND is not None:
            asyncio.create_task(
                BACKEND.cancel_order_v2(
                    client_order_id, exchange_name, market_type, symbol,
                    account_id,
                )
            )
            return Response(status_code=status.HTTP_200_OK, content="OK")

        asyncio.create_task(
            ccxt_wrapper.cancel_order_v2(
                client_order_id, exchange_name, market_type, symbol, account_id
            )
        )
        return Response(status_code=status.HTTP_200_OK, content="OK")
    except Exception as e:
        log(
            f"Error in {Routes.CANCEL_ORDER_V2}: {str(e)}",
            {
                "client_order_id": client_order_id,
                "exchange_name": exchange_name,
                "market_type": market_type,
                "symbol": symbol,
                "account_id": account_id,
            },
            "ERROR",
        )
        raise HTTPException(status_code=500, detail=str(e))


@router.get(Routes.GET_ORDERS_ON_SYMBOL)
async def get_orders_on_symbol(
    exchange_name: str, market_type: str, account_id: int, symbol: str
):
    try:
        if BACKEND is not None and exchange_name != "polymarket":
            res = await BACKEND.get_orders_on_symbol(
                exchange_name, market_type, account_id, symbol
            )
            return {"data": res}

        if exchange_name == "polymarket":
            res = await polymarket_wrapper.get_open_orders(
                market=symbol, account_id=account_id
            )
            return {"data": res}
        if exchange_name == "aster":
            return {"data": await AsterWrapper().get_open_orders(account_id, symbol)}
        res = await ccxt_wrapper.get_orders_on_symbol(
            exchange_name, market_type, account_id, symbol
        )
        return {
            "data": res,
        }
    except Exception as e:
        log(
            f"Error in {Routes.GET_ORDERS_ON_SYMBOL}: {str(e)}",
            {
                "exchange_name": exchange_name,
                "market_type": market_type,
                "account_id": account_id,
                "symbol": symbol,
            },
            "ERROR",
        )
        raise HTTPException(status_code=500, detail=str(e))


@router.get(Routes.OPEN_ORDERS)
async def get_open_orders(
    exchange_name: str, market_type: str, account_id: int, symbol: str = None
):
    try:
        if BACKEND is not None:
            res = await BACKEND.get_all_open_orders(
                symbol, exchange_name, market_type, account_id
            )
            return {"data": res}

        if exchange_name == "aster":
            return {"data": await AsterWrapper().get_open_orders(account_id, symbol)}
        res = await ccxt_wrapper.get_all_open_orders(
            symbol, exchange_name, market_type, account_id
        )
        return {
            "data": res,
        }
    except Exception as e:
        log(
            f"Error in {Routes.OPEN_ORDERS}: {str(e)}",
            {
                "exchange_name": exchange_name,
                "market_type": market_type,
                "account_id": account_id,
                "symbol": symbol,
            },
            "ERROR",
        )
        raise HTTPException(status_code=500, detail=str(e))


@router.get(Routes.QUERY_ORDER)
async def query_order(
    symbol: str,
    exchange_name: str,
    market_type: str,
    account_id: int,
    client_order_id: str,
):
    try:
        if BACKEND is not None and exchange_name != "polymarket":
            res = await BACKEND.query_order(
                symbol, exchange_name, market_type, account_id, client_order_id
            )
            if res is None:
                raise HTTPException(status_code=404, detail="Order not found")
            return {"data": res}

        if exchange_name == "polymarket":
            if not client_order_id:
                raise HTTPException(status_code=404, detail="Order not found")
            res = await polymarket_wrapper.query_order(
                order_id=client_order_id, account_id=account_id
            )
            if res is None:
                raise HTTPException(status_code=404, detail="Order not found")
            return {"data": res}
        if exchange_name == "aster":
            res = await AsterWrapper().query_order(account_id, symbol, client_order_id)
            if res is None:
                raise HTTPException(status_code=404, detail="Order not found")
            return {"data": res}
        res = await ccxt_wrapper.query_order(
            symbol, exchange_name, market_type, account_id, client_order_id
        )
        if res is None:
            raise HTTPException(status_code=404, detail="Order not found")
        return {
            "data": res,
        }
    except HTTPException:
        raise
    except ccxt.OrderNotFound as e:
        raise HTTPException(status_code=404, detail=str(e))
    except Exception as e:
        # Not 404: the engine removes an order only on a venue "not found".
        log(
            f"Error in {Routes.QUERY_ORDER}: {type(e).__name__}: {e}",
            {"exchange_name": exchange_name, "client_order_id": client_order_id},
            "ERROR",
        )
        raise HTTPException(status_code=502, detail=str(e))
