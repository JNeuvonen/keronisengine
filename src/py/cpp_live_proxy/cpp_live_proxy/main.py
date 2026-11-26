import os
import sys
from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware
from cpp_live_proxy._compat import (
    PY_API_SHUTDOWN,
    PY_API_STARTUP,
    emit_trading_log,
    with_audit_context,
)
from cpp_live_proxy.ccxt_wrapper import CcxtWrapper
from cpp_live_proxy.aster_wrapper import AsterWrapper
from cpp_live_proxy.polymarket_wrapper import PolymarketWrapper
from cpp_live_proxy.polymarket_streamer import PolymarketStreamer
from cpp_live_proxy.api.v1.ccxt import router as ccxt_router, set_polymarket_streamer
from cpp_live_proxy.api.v1.user_data import router as user_data_router
from cpp_live_proxy.gcp_log import EMIT_GCP_LOGS, log
from contextlib import asynccontextmanager
import asyncio
import time


async def collateral_rebalancer(polymarket: PolymarketWrapper):
    """Rotate collateral so both pUSD (trading) and USDC.e (mint/redeem) stay
    funded. Two floors on the combined (USDC.e+pUSD) balance: when USDC.e dips
    below COLLATERAL_MIN_USDCE_RATIO, unwrap the abundant pUSD -> USDC.e; when
    pUSD dips below COLLATERAL_MIN_PUSD_RATIO, wrap the abundant USDC.e -> pUSD.
    At most one rotation per account per loop. With both floors at 15% and
    moving 80% of the abundant side, a rotation only fires when the other side
    is depleted, leaving both sides well inside the band — no oscillation.

    Runs ONLY in the singleton SPAWN_PRIVATE_STREAMS worker (callers gate this)
    so multiple uvicorn workers can't fire concurrent wrap/unwrap txs (nonce
    races). All thresholds are env-configurable. On-chain tx runs off the event
    loop via to_thread. Every action is logged to trading_activity_log."""
    interval_s = int(os.getenv("COLLATERAL_REBALANCE_INTERVAL_S", "60"))
    min_usdce_ratio = float(os.getenv("COLLATERAL_MIN_USDCE_RATIO", "0.15"))
    min_pusd_ratio = float(os.getenv("COLLATERAL_MIN_PUSD_RATIO", "0.15"))
    move_pusd_fraction = float(os.getenv("COLLATERAL_MOVE_PUSD_FRACTION", "0.80"))
    move_usdce_fraction = float(os.getenv("COLLATERAL_MOVE_USDCE_FRACTION", "0.80"))
    min_move_usd = float(os.getenv("COLLATERAL_MIN_MOVE_USD", "5"))
    cooldown_s = int(os.getenv("COLLATERAL_REBALANCE_COOLDOWN_S", "120"))
    last_rebalance: dict[int, float] = {}
    log(f"[collateral_rebalancer] active: usdce_floor={min_usdce_ratio} "
        f"pusd_floor={min_pusd_ratio} interval={interval_s}s", {}, "INFO")
    while True:
        try:
            for account_id in list(polymarket.get_all_accounts().keys()):
                try:
                    bals = await asyncio.to_thread(
                        polymarket.get_eoa_balances, account_id)
                except Exception as e:
                    log(f"[collateral_rebalancer] balance read failed "
                        f"acct={account_id}: {e!r}", {}, "WARNING")
                    continue
                usdce = bals["usdc_balance"]
                pusd = bals["pusd_balance"]
                combined = usdce + pusd
                if combined <= 0:
                    continue
                usdce_ratio = usdce / combined
                pusd_ratio = pusd / combined
                if usdce_ratio < min_usdce_ratio:
                    direction = "unwrap"
                    move = pusd * move_pusd_fraction
                elif pusd_ratio < min_pusd_ratio:
                    direction = "wrap"
                    move = usdce * move_usdce_fraction
                else:
                    continue
                if move < min_move_usd:
                    continue
                if time.time() - last_rebalance.get(account_id, 0.0) < cooldown_s:
                    continue
                last_rebalance[account_id] = time.time()
                label = "pUSD->USDC.e" if direction == "unwrap" else "USDC.e->pUSD"
                emit_trading_log(
                    event_type="COLLATERAL_REBALANCE", level="INFO",
                    message=(f"{label}: usdce={usdce:.2f} pusd={pusd:.2f} "
                             f"usdce_ratio={usdce_ratio:.3f} "
                             f"pusd_ratio={pusd_ratio:.3f} move={move:.2f}"),
                    metadata={"account_id": account_id, "usdce": usdce,
                              "pusd": pusd, "combined": combined,
                              "usdce_ratio": usdce_ratio,
                              "pusd_ratio": pusd_ratio, "move_usd": move,
                              "direction": direction,
                              "exchange_name": "polymarket"})
                try:
                    if direction == "unwrap":
                        res = await asyncio.to_thread(
                            polymarket.unwrap_pusd_to_usdce, move, account_id)
                        moved = res.get("unwrapped_usd", 0)
                    else:
                        res = await asyncio.to_thread(
                            polymarket.wrap_usdce_to_pusd, move, account_id)
                        moved = res.get("wrapped_usd", 0)
                    emit_trading_log(
                        event_type="COLLATERAL_REBALANCE", level="INFO",
                        message=f"rebalance done ({direction}): {moved:.2f}",
                        metadata={"account_id": account_id, "moved_usd": moved,
                                  "direction": direction,
                                  "tx_hash": res.get("tx_hash", ""),
                                  "exchange_name": "polymarket"})
                except Exception as e:
                    emit_trading_log(
                        event_type="COLLATERAL_REBALANCE_FAILED", level="ERROR",
                        message=f"rebalance {direction} failed acct={account_id}: {e!r}",
                        metadata={"account_id": account_id, "error": repr(e),
                                  "direction": direction,
                                  "exchange_name": "polymarket"})
        except Exception as e:
            log(f"[collateral_rebalancer] loop error: {e!r}", {}, "ERROR")
        await asyncio.sleep(interval_s)

print("PYTHON:", sys.executable)


USE_TESTNET = os.getenv("USE_TESTNET", "1") == "1"
SPAWN_PRIVATE_STREAMS = os.getenv("SPAWN_PRIVATE_STREAMS", "0") == "1"
TEST_MODE = os.getenv("CPP_LIVE_PROXY_TEST_MODE", "0") == "1"


class Routes:
    CCXT = "/api/v1/ccxt"
    USER_DATA_STREAM = "/api/v1/user-data"
    TEST_CONTROL = "/api/v1/test"


@asynccontextmanager
async def lifespan(app: FastAPI):
    if TEST_MODE:
        print("CPP_LIVE_PROXY_TEST_MODE=1 — skipping CCXT/Polymarket/Aster wrapper init")

        streamer_task = None
        streamer = None
        if os.getenv("CPP_LIVE_PROXY_BACKEND", "") == "simulator":
            from cpp_live_proxy.backends.simulator_event_streamer import (
                SimulatorEventStreamer,
            )

            sim_url = os.getenv("SIMULATOR_CONTROL_URL",
                                "http://127.0.0.1:19000")
            broadcaster = CcxtWrapper()  # singleton, cheap __init__
            streamer = SimulatorEventStreamer(sim_url, broadcaster.broadcast)
            streamer_task = asyncio.create_task(streamer.run())
            print(f"[simulator_event_streamer] started (source={sim_url})")

        try:
            yield
        finally:
            if streamer is not None:
                await streamer.aclose()
            if streamer_task is not None:
                streamer_task.cancel()
                try:
                    await streamer_task
                except (asyncio.CancelledError, Exception):
                    pass
        return

    asyncio.create_task(CcxtWrapper().init_accounts(use_testnet=USE_TESTNET))

    polymarket = PolymarketWrapper()
    try:
        await polymarket.init()
        if SPAWN_PRIVATE_STREAMS:
            for account_id, acct in polymarket.get_all_accounts().items():
                creds = acct.client.creds
                streamer = PolymarketStreamer(
                    api_key=creds.api_key,
                    secret=creds.api_secret,
                    passphrase=creds.api_passphrase,
                    account_id=account_id,
                )
                set_polymarket_streamer(streamer, account_id)
                asyncio.create_task(streamer.start())

            # Collateral rebalancer: always on whenever polymarket accounts are
            # configured. Runs only in this singleton private-streams worker so
            # prod's multiple public workers can't fire concurrent wrap txs
            # (nonce races) — that gate is for correctness, not opt-in.
            if polymarket.get_all_accounts():
                asyncio.create_task(collateral_rebalancer(polymarket))
                print("[collateral_rebalancer] started")
    except Exception as e:
        print(f"Warning: Polymarket init failed: {e}")

    try:
        aster = AsterWrapper()
        await aster.init()
    except Exception as e:
        print(f"Warning: Aster init failed: {e}")

    emit_trading_log(
        event_type=PY_API_STARTUP,
        level="INFO",
        message="cpp_live_proxy started",
        metadata={
            "service": "cpp_live_proxy",
            "pid": os.getpid(),
            "use_testnet": USE_TESTNET,
            "spawn_private_streams": SPAWN_PRIVATE_STREAMS,
        },
    )

    yield

    emit_trading_log(
        event_type=PY_API_SHUTDOWN,
        level="INFO",
        message="cpp_live_proxy shutdown",
        metadata={"service": "cpp_live_proxy", "pid": os.getpid()},
    )


GOOGLE_CREDENTIALS_PATH = os.getenv("GOOGLE_APPLICATION_CREDENTIALS")

if EMIT_GCP_LOGS and (not GOOGLE_CREDENTIALS_PATH or not os.path.isfile(GOOGLE_CREDENTIALS_PATH)):
    print(
        f"ERROR: Invalid or missing GOOGLE_APPLICATION_CREDENTIALS: {GOOGLE_CREDENTIALS_PATH}"
    )
    sys.exit(1)


app = FastAPI(lifespan=lifespan)
app.add_middleware(
    CORSMiddleware,
    allow_origins=["http://127.0.0.1", "http://localhost"],
    allow_credentials=True,
    allow_methods=["POST", "GET"],
    allow_headers=["content-type"],
)


@app.middleware("http")
async def audit_attribution(request, call_next):
    """Propagate (exchange, market_type, account_id, symbol) from the
    inbound query string into the audit contextvar for the dynamic
    extent of this request. The aiohttp TraceConfig reads the context
    when any outbound ccxt call fires, attributing the row in
    outbound_request_audit. Cost: ~µs per inbound request."""
    qp = request.query_params
    account_id_raw = qp.get("account_id")
    try:
        account_id = int(account_id_raw) if account_id_raw is not None else None
    except ValueError:
        account_id = None
    with with_audit_context(
        exchange=qp.get("exchange_name"),
        market_type=qp.get("market_type"),
        account_id=account_id,
        symbol=qp.get("symbol"),
    ):
        return await call_next(request)


# IBKR-forwarding middleware: only the SPAWN_PRIVATE_STREAMS=1 worker
# owns the IBKR session (single TCP connection per clientId, single
# order-id allocator, single subscription stream). Public-streams
# workers see IBKR-bearing requests and forward them verbatim to the
# private worker. Non-IBKR requests pass through unchanged.
IBKR_FORWARD_URL = os.getenv("IBKR_FORWARD_URL", "")

if not SPAWN_PRIVATE_STREAMS and IBKR_FORWARD_URL:
    import httpx
    from fastapi import Request
    from fastapi.responses import Response

    _ibkr_forward_client = httpx.AsyncClient(
        base_url=IBKR_FORWARD_URL, timeout=30.0
    )

    def _request_targets_ibkr(query_string: str, body: bytes) -> bool:
        # Cheap query-string check covers all GET endpoints.
        q = query_string.lower()
        if "exchange_name=ibkr" in q:
            return True
        # POST bodies (create-order / cancel-order) carry exchange_name in
        # JSON. Substring match is intentionally permissive — any JSON
        # shape with this field hits IBKR routing.
        if not body:
            return False
        b = body.lower().replace(b" ", b"")
        return b'"exchange_name":"ibkr"' in b

    @app.middleware("http")
    async def forward_ibkr_to_private_streams(request: Request, call_next):
        # Only POST may carry a body; reading body in middleware requires
        # us to replay it for the downstream handler if we don't forward.
        body = b""
        if request.method == "POST":
            body = await request.body()

        if not _request_targets_ibkr(request.url.query, body):
            if body:
                # Replay the body for the downstream handler.
                async def _receive():
                    return {"type": "http.request", "body": body, "more_body": False}
                request._receive = _receive
            return await call_next(request)

        # Forward verbatim to the private-streams worker.
        target_path = request.url.path
        if request.url.query:
            target_path = f"{target_path}?{request.url.query}"
        headers = {
            k: v for k, v in request.headers.items()
            if k.lower() not in ("host", "content-length")
        }
        try:
            upstream = await _ibkr_forward_client.request(
                method=request.method,
                url=target_path,
                content=body if body else None,
                headers=headers,
            )
        except httpx.RequestError as e:
            log(f"IBKR forward failed: {e}", {"path": target_path}, "ERROR")
            return Response(
                content=f'{{"detail":"IBKR forward to {IBKR_FORWARD_URL} failed: {e}"}}',
                status_code=502,
                media_type="application/json",
            )
        # Strip hop-by-hop response headers; let Starlette set its own.
        excluded = {"content-encoding", "transfer-encoding", "content-length"}
        return Response(
            content=upstream.content,
            status_code=upstream.status_code,
            headers={
                k: v for k, v in upstream.headers.items()
                if k.lower() not in excluded
            },
            media_type=upstream.headers.get("content-type"),
        )
app.include_router(ccxt_router, prefix=Routes.CCXT)
app.include_router(user_data_router, prefix=Routes.USER_DATA_STREAM)

# Test-only control endpoints — gated on CPP_LIVE_PROXY_TEST_MODE so
# production proxies never expose them. Used by infra_e2e_testing
# scenarios to simulate failure modes (WS gap, etc.).
if TEST_MODE:
    from cpp_live_proxy.api.v1.test_control import router as test_control_router
    app.include_router(test_control_router, prefix=Routes.TEST_CONTROL)


if __name__ == "__main__":
    import uvicorn

    log("Starting server on port 8000", {}, "INFO")
    uvicorn.run(app, host="127.0.0.1", port=8000)
