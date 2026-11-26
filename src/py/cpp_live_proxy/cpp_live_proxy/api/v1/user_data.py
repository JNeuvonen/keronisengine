from fastapi import APIRouter, WebSocket, WebSocketDisconnect
import asyncio
import json
from typing import Any
from cpp_live_proxy.ccxt_wrapper import CcxtWrapper
from cpp_live_proxy.gcp_log import log

router = APIRouter()

# The engine treats 45s without any message as a dead socket.
HEARTBEAT_INTERVAL_S = 15


@router.websocket("/stream")
async def websocket_orders(websocket: WebSocket):
    queue: asyncio.Queue[Any] = asyncio.Queue()
    ccxt_wrapper = CcxtWrapper()
    try:
        await websocket.accept()
        ccxt_wrapper.register_listener(queue)
        while True:
            try:
                data = await asyncio.wait_for(queue.get(), HEARTBEAT_INTERVAL_S)
            except asyncio.TimeoutError:
                await websocket.send_text(json.dumps({"event_type": "heartbeat"}))
                continue
            # ``None`` is the close-sentinel pushed by
            # CcxtWrapper.disconnect_all_user_data_listeners() — used by
            # test scenarios to simulate a WS gap. Break cleanly so the
            # finally block unregisters and closes the socket.
            if data is None:
                break
            await websocket.send_text(json.dumps(data))
    except WebSocketDisconnect:
        pass
    except Exception as e:
        log(f"WebSocket stream error: {type(e).__name__}: {e}", {}, "ERROR")
    finally:
        ccxt_wrapper.unregister_listener(queue)
        try:
            await websocket.close()
        except Exception:
            pass


@router.get("/status")
async def stream_status():
    return CcxtWrapper()._private_stream_status


@router.post("/restart-streams")
async def restart_streams(exchange_name: str, market_type: str, account_id: int):
    restarted = await CcxtWrapper().restart_private_streams_for_account(
        exchange_name, market_type, account_id
    )
    return {"restarted": restarted}
