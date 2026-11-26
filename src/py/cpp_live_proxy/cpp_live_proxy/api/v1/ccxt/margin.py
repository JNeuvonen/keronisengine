from fastapi import APIRouter, HTTPException

from cpp_live_proxy.api.v1.ccxt._shared import BACKEND, Routes, ccxt_wrapper
from cpp_live_proxy.gcp_log import log

router = APIRouter()


@router.post(Routes.REPAY_LOAN)
async def repay_loan(
    exchange_name: str,
    market_type: str,
    account_id: int,
    symbol: str,
    amount: str,
):
    try:
        if BACKEND is not None and exchange_name not in ("polymarket", "aster"):
            res = await BACKEND.repay_loan(
                exchange_name, market_type, account_id, symbol, amount)
            return {"data": res}

        res = await ccxt_wrapper.repay_loan(
            exchange_name,
            market_type,
            account_id,
            symbol,
            amount,
        )
        return {
            "data": res,
        }
    except Exception as e:
        print("Error in repay loan")
        print(exchange_name)
        print(market_type)
        print(account_id)
        print(symbol)
        print(amount)
        print(str(e))
        log(
            f"Error in {Routes.REPAY_LOAN}: {str(e)}",
            {
                "exchange_name": exchange_name,
                "market_type": market_type,
                "account_id": account_id,
                "symbol": symbol,
                "amount": amount,
            },
            "ERROR",
        )
        raise HTTPException(status_code=500, detail=str(e))


@router.post(Routes.UPDATE_SWAPS_DEFAULT_LEVERAGE_LEVEL)
async def update_swaps_default_leverage_level(
    exchange_name: str,
    market_type: str,
    account_id: int,
    symbol: str,
    leverage_level: int,
):
    try:
        if BACKEND is not None and exchange_name not in ("polymarket", "aster"):
            res = await BACKEND.update_swaps_default_leverage_level(
                exchange_name, market_type, account_id, symbol,
                leverage_level)
            return {"data": res}

        res = await ccxt_wrapper.update_swaps_default_leverage_level(
            exchange_name, market_type, account_id, symbol, leverage_level
        )
        return {
            "data": res,
        }
    except Exception as e:
        log(
            f"Error in {Routes.UPDATE_SWAPS_DEFAULT_LEVERAGE_LEVEL}: {str(e)}",
            {
                "exchange_name": exchange_name,
                "market_type": market_type,
                "account_id": account_id,
                "symbol": symbol,
                "leverage_level": leverage_level,
            },
            "ERROR",
        )
        raise HTTPException(status_code=500, detail=str(e))


@router.get(Routes.QUERY_SWAP_SYMBOL_CONFIGURATION)
async def query_swap_symbol_configuration(
    exchange_name: str,
    market_type: str,
    account_id: int,
):
    try:
        if BACKEND is not None and exchange_name not in ("polymarket", "aster"):
            res = await BACKEND.query_swap_symbol_leverage_level(
                exchange_name, market_type, account_id)
            return {"data": res}

        res = await ccxt_wrapper.query_swap_symbol_leverage_level(
            exchange_name,
            market_type,
            account_id,
        )
        return {
            "data": res,
        }
    except Exception as e:
        log(
            f"Error in {Routes.QUERY_SWAP_SYMBOL_CONFIGURATION}: {str(e)}",
            {
                "exchange_name": exchange_name,
                "market_type": market_type,
                "account_id": account_id,
            },
            "ERROR",
        )
        raise HTTPException(status_code=500, detail=str(e))
