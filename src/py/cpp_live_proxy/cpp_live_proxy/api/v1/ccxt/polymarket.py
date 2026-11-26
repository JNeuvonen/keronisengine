import asyncio
from typing import List

from fastapi import APIRouter, HTTPException, Query, Response, status
from fastapi.responses import JSONResponse
from pydantic import BaseModel

from cpp_live_proxy.api.v1.ccxt._shared import (
    Routes,
    polymarket_streamers,
    polymarket_wrapper,
)
from cpp_live_proxy.gcp_log import log
from cpp_live_proxy.type_models import (
    MergeContractsRequest,
    MintContractsRequest,
    RedeemWinningsRequest,
)

router = APIRouter()


async def _resolve_condition(body) -> tuple:
    if body.condition_id:
        return body.condition_id, body.neg_risk, None
    if not body.slug:
        raise ValueError("Either condition_id or slug is required")
    market_info = await asyncio.to_thread(polymarket_wrapper.resolve_market, body.slug)
    return (
        market_info["condition_id"],
        market_info["neg_risk"],
        market_info.get("tokens"),
    )


@router.post(Routes.MINT_CONTRACTS)
async def mint_contracts(body: MintContractsRequest):
    try:
        condition_id, neg_risk, _ = await _resolve_condition(body)
        result = await asyncio.to_thread(
            polymarket_wrapper.mint_complete_sets,
            condition_id,
            neg_risk,
            body.num_sets,
            body.account_id,
        )
        return JSONResponse(content=result)
    except Exception as e:
        log(
            f"Error in {Routes.MINT_CONTRACTS}: {str(e)}",
            {
                "condition_id": body.condition_id,
                "slug": body.slug,
                "num_sets": body.num_sets,
            },
            "ERROR",
        )
        raise HTTPException(status_code=500, detail=str(e))


@router.post(Routes.MERGE_CONTRACTS)
async def merge_contracts(body: MergeContractsRequest):
    try:
        condition_id, neg_risk, _ = await _resolve_condition(body)
        result = await asyncio.to_thread(
            polymarket_wrapper.merge_complete_sets,
            condition_id,
            neg_risk,
            body.num_sets,
            body.account_id,
        )
        return JSONResponse(content=result)
    except Exception as e:
        log(
            f"Error in {Routes.MERGE_CONTRACTS}: {str(e)}",
            {
                "condition_id": body.condition_id,
                "slug": body.slug,
                "num_sets": body.num_sets,
            },
            "ERROR",
        )
        raise HTTPException(status_code=500, detail=str(e))


@router.post(Routes.REDEEM_WINNINGS)
async def redeem_winnings(body: RedeemWinningsRequest):
    try:
        condition_id, neg_risk, tokens = await _resolve_condition(body)
        if tokens is None:
            market_info = await asyncio.to_thread(
                polymarket_wrapper.resolve_market, body.slug
            )
            tokens = market_info["tokens"]
        result = await asyncio.to_thread(
            polymarket_wrapper.redeem_winnings,
            condition_id,
            neg_risk,
            tokens,
            body.account_id,
        )
        return JSONResponse(content=result)
    except Exception as e:
        log(
            f"Error in {Routes.REDEEM_WINNINGS}: {str(e)}",
            {"condition_id": body.condition_id, "slug": body.slug},
            "ERROR",
        )
        raise HTTPException(status_code=500, detail=str(e))


@router.get(Routes.POLYMARKET_POSITIONS)
async def get_polymarket_positions(account_id: int = Query(0)):
    try:
        address = polymarket_wrapper.get_eoa_address(account_id)
        positions = await asyncio.to_thread(polymarket_wrapper.get_positions, address)
        return JSONResponse(content={"address": address, "positions": positions})
    except Exception as e:
        log(f"Error in {Routes.POLYMARKET_POSITIONS}: {str(e)}", {}, "ERROR")
        raise HTTPException(status_code=500, detail=str(e))


@router.get(Routes.EOA_BALANCES)
async def get_eoa_balances(account_id: int = Query(0)):
    try:
        balances = await asyncio.to_thread(
            polymarket_wrapper.get_eoa_balances, account_id
        )
        return JSONResponse(content=balances)
    except Exception as e:
        log(f"Error in {Routes.EOA_BALANCES}: {str(e)}", {}, "ERROR")
        raise HTTPException(status_code=500, detail=str(e))


@router.get(Routes.RESOLVE_MARKET)
async def resolve_market(slug: str = Query(...)):
    try:
        result = await asyncio.to_thread(polymarket_wrapper.resolve_market, slug)
        result.pop("market", None)
        return JSONResponse(content=result)
    except Exception as e:
        log(f"Error in {Routes.RESOLVE_MARKET}: {str(e)}", {"slug": slug}, "ERROR")
        raise HTTPException(status_code=500, detail=str(e))


class PolymarketStreamSubscribeBody(BaseModel):
    condition_ids: List[str]


@router.post(Routes.POLYMARKET_STREAM_SUBSCRIBE)
async def polymarket_stream_subscribe(body: PolymarketStreamSubscribeBody):
    if not polymarket_streamers:
        raise HTTPException(
            status_code=503, detail="Polymarket streamer not initialized"
        )
    try:
        for streamer in polymarket_streamers.values():
            await streamer.set_subscriptions(set(body.condition_ids))
        return Response(status_code=status.HTTP_200_OK, content="OK")
    except Exception as e:
        log(f"Error in {Routes.POLYMARKET_STREAM_SUBSCRIBE}: {str(e)}", {}, "ERROR")
        raise HTTPException(status_code=500, detail=str(e))


@router.get(Routes.POLYMARKET_ALL_OPEN_ORDERS)
async def get_polymarket_all_open_orders(account_id: int = Query(0)):
    try:
        res = await polymarket_wrapper.get_all_open_orders(account_id)
        return {"data": res}
    except Exception as e:
        log(f"Error in {Routes.POLYMARKET_ALL_OPEN_ORDERS}: {str(e)}", {}, "ERROR")
        raise HTTPException(status_code=500, detail=str(e))
