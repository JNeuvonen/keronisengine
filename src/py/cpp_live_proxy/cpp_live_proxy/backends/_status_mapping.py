"""MatchingEngine OrderStatus <-> CCXT status string.

CCXT status vocabulary observed by the engine in
``OrderResNormalized::from_ccxt_user_data_stream_msg``:
  - ``"open"`` -> working
  - anything else -> not working (closed/canceled/rejected/expired)

MatchingEngine OrderStatus values (per simulator_control_server/dtype_helpers.h):
  New, Acknowledged, PartiallyFilled, Filled, Canceled, Rejected, Expired

Mapping is intentionally lossy (CCXT collapses many statuses into
not-working). The streamer in Phase D adds ``filled``/``average``/etc. to
preserve fill information.
"""


_SIM_TO_CCXT = {
    "New": "open",
    "Acknowledged": "open",
    "PartiallyFilled": "open",
    "Filled": "closed",
    "Canceled": "canceled",
    "Rejected": "rejected",
    "Expired": "expired",
}


def sim_to_ccxt(sim_status: str) -> str:
    return _SIM_TO_CCXT.get(sim_status, "closed")
