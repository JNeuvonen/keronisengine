"""Optional aiohttp TraceConfig hook for outbound HTTP auditing."""

from __future__ import annotations

from typing import List
from aiohttp import TraceConfig


def maybe_trace_configs() -> List[TraceConfig]:
    return []
