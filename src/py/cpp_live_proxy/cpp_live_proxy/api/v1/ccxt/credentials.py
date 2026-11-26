import os

from fastapi import APIRouter, Depends, HTTPException
from fastapi.responses import JSONResponse
from fastapi.security import HTTPAuthorizationCredentials, HTTPBearer

from cpp_live_proxy.api.v1.ccxt._shared import (
    ASTER_MOCK_CREDENTIALS,
    HYPERLIQUID_MOCK_CREDENTIALS,
    POLYMARKET_MOCK_CREDENTIALS,
    Routes,
    _MOCK_ASTER_CREDENTIALS,
    _MOCK_HYPERLIQUID_CREDENTIALS,
    _MOCK_POLYMARKET_CREDENTIALS,
    ccxt_wrapper,
    polymarket_wrapper,
)
from cpp_live_proxy.gcp_log import log

PROXY_AUTH_TOKEN = os.getenv("PROXY_AUTH_TOKEN", "")
proxy_auth_scheme = HTTPBearer(auto_error=False)


async def verify_proxy_auth_token(
    credentials: HTTPAuthorizationCredentials | None = Depends(proxy_auth_scheme),
):
    if not PROXY_AUTH_TOKEN:
        return
    if credentials is None or credentials.credentials != PROXY_AUTH_TOKEN:
        raise HTTPException(status_code=403, detail="Forbidden")


router = APIRouter()


@router.get(Routes.GET_CREDENTIALS)
async def get_credentials(_=Depends(verify_proxy_auth_token)):
    if POLYMARKET_MOCK_CREDENTIALS:
        return JSONResponse(content=_MOCK_POLYMARKET_CREDENTIALS)
    try:
        accounts = {}
        for account_id, acct in polymarket_wrapper.get_all_accounts().items():
            client = acct.client
            accounts[str(account_id)] = {
                "private_key": acct.private_key,
                "api_key": client.creds.api_key,
                "api_secret": client.creds.api_secret,
                "api_passphrase": client.creds.api_passphrase,
                "maker_address": client.builder.funder,
                "signer_address": client.signer.address(),
                "builder_api_key": acct.builder_api_key,
                "builder_secret": acct.builder_secret,
                "builder_passphrase": acct.builder_passphrase,
            }
        return JSONResponse(content={"accounts": accounts})
    except Exception as e:
        log(f"Error in {Routes.GET_CREDENTIALS}: {str(e)}", {}, "ERROR")
        raise HTTPException(status_code=500, detail=str(e))


@router.get(Routes.GET_ASTER_CREDENTIALS)
async def get_aster_credentials(_=Depends(verify_proxy_auth_token)):
    if ASTER_MOCK_CREDENTIALS:
        return JSONResponse(content=_MOCK_ASTER_CREDENTIALS)
    try:
        accounts_json_path = os.getenv("ACCOUNTS_JSON_PATH")
        if not accounts_json_path:
            return JSONResponse(content={"accounts": {}})

        import json as json_module

        with open(accounts_json_path) as f:
            data = json_module.load(f)

        aster_raw = data.get("aster_futures_accounts", [])
        if not aster_raw:
            return JSONResponse(content={"accounts": {}})

        from cpp_live_proxy._compat import resolve_optional_gcp_secret

        def resolve_secret(item, key, gcp_key):
            gcp_ref = item.get(gcp_key, "")
            if gcp_ref and os.getenv("GCP_PROJECT_ID", ""):
                return resolve_optional_gcp_secret(gcp_ref)
            value = item.get(key, "")
            if value:
                return value
            raise ValueError(f"aster account must have either '{gcp_key}' or '{key}'")

        accounts = {}
        for item in aster_raw:
            account_id = str(item["account_id"])
            private_key = resolve_secret(item, "private_key", "private_key_gcp_secret")
            accounts[account_id] = {
                "private_key": private_key,
                "user_address": item["user_address"],
                "signer_address": item["signer_address"],
            }
        return JSONResponse(content={"accounts": accounts})
    except Exception as e:
        log(f"Error in {Routes.GET_ASTER_CREDENTIALS}: {str(e)}", {}, "ERROR")
        raise HTTPException(status_code=500, detail=str(e))


@router.get(Routes.GET_HYPERLIQUID_CREDENTIALS)
async def get_hyperliquid_credentials(_=Depends(verify_proxy_auth_token)):
    # Polymarket-style: creds are GCP-resolved once at startup by the ccxt layer
    # (_init_hyperliquid_accounts) and read back from the singleton here, not
    # re-read from accounts.json + GCP per request.
    if HYPERLIQUID_MOCK_CREDENTIALS:
        return JSONResponse(content=_MOCK_HYPERLIQUID_CREDENTIALS)
    try:
        creds = getattr(ccxt_wrapper, "_hyperliquid_direct_creds", {})
        return JSONResponse(content={"accounts": dict(creds)})
    except Exception as e:
        log(f"Error in {Routes.GET_HYPERLIQUID_CREDENTIALS}: {str(e)}", {}, "ERROR")
        raise HTTPException(status_code=500, detail=str(e))
