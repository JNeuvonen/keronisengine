from fastapi import APIRouter

from cpp_live_proxy.api.v1.ccxt._shared import set_polymarket_streamer
from cpp_live_proxy.api.v1.ccxt.credentials import router as credentials_router
from cpp_live_proxy.api.v1.ccxt.margin import router as margin_router
from cpp_live_proxy.api.v1.ccxt.metadata import router as metadata_router
from cpp_live_proxy.api.v1.ccxt.orders import router as orders_router
from cpp_live_proxy.api.v1.ccxt.polymarket import router as polymarket_router

router = APIRouter()
router.include_router(orders_router)
router.include_router(metadata_router)
router.include_router(margin_router)
router.include_router(polymarket_router)
router.include_router(credentials_router)

__all__ = ["router", "set_polymarket_streamer"]
