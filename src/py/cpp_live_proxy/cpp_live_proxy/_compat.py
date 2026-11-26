"""Standalone compatibility helpers for open-source cpp_live_proxy."""

from __future__ import annotations

import contextlib
import logging
import os
import time
from typing import Any, Dict, Optional

_logger = logging.getLogger("cpp_live_proxy")

PY_API_STARTUP = "PY_API_STARTUP"
PY_API_SHUTDOWN = "PY_API_SHUTDOWN"
PY_PROXY_VENUE_ERROR = "PY_PROXY_VENUE_ERROR"


def get_current_time_in_ms() -> int:
    return int(time.time() * 1000)


def emit_trading_log(
    event_type: str,
    level: str = "INFO",
    message: str = "",
    metadata: Optional[Dict[str, Any]] = None,
) -> None:
    lvl = getattr(logging, level.upper(), logging.INFO)
    _logger.log(lvl, "[%s] %s %s", event_type, message, metadata or "")


@contextlib.contextmanager
def with_audit_context(**_kwargs: Any):
    yield


def resolve_optional_gcp_secret(secret_ref: str) -> str:
    """Resolve an external secret reference from environment variable SECRET_<REF>."""
    env_key = f"SECRET_{secret_ref.upper().replace('-', '_')}"
    val = os.getenv(env_key, "")
    if val:
        return val.strip()
    raise ValueError(
        f"Secret reference '{secret_ref}' requires either plaintext credentials "
        f"in accounts.json or environment variable {env_key}"
    )


class GateioUtils:
    @staticmethod
    def get_cross_margin_usdt_pairs(api_key: str = "", api_secret: str = ""):
        import ccxt

        ex = ccxt.gateio({"apiKey": api_key, "secret": api_secret})
        markets = ex.load_markets()
        return [
            m["id"].replace("_", "")
            for m in markets.values()
            if m.get("spot") and m.get("quote") == "USDT" and m.get("margin")
        ]
