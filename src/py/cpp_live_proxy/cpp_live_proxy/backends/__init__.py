"""Pluggable backends for cpp_live_proxy route handlers.

Production paths route through the existing wrapper singletons (CcxtWrapper,
PolymarketWrapper, AsterWrapper). Test-mode paths route through a backend
selected via the CPP_LIVE_PROXY_BACKEND env var. Each backend implements the
same method shapes the route handlers call, so the dispatch in api/v1/ccxt.py
is a single ``if BACKEND is not None: backend.method(...)`` per handler.

Backend selection happens once at module import. Set via env:

    CPP_LIVE_PROXY_BACKEND=simulator   # delegate to full_market_simulator
    SIMULATOR_CONTROL_URL=http://127.0.0.1:19000   # default
"""

import os
from typing import Optional

from cpp_live_proxy.backends.simulator_backend import SimulatorBackend


def get_backend() -> Optional[SimulatorBackend]:
    name = os.getenv("CPP_LIVE_PROXY_BACKEND", "")
    if name == "simulator":
        url = os.getenv("SIMULATOR_CONTROL_URL", "http://127.0.0.1:19000")
        return SimulatorBackend(control_url=url)
    return None


__all__ = ["SimulatorBackend", "get_backend"]
