from pydantic import BaseModel
from typing import Any, Dict, Optional


class Prediction(BaseModel):
    prediction: float
    price: float
    event_time_ms: int


class MintContractsRequest(BaseModel):
    condition_id: str = ""
    neg_risk: bool = False
    slug: str = ""
    num_sets: int = 0
    account_id: int = 0


class MergeContractsRequest(BaseModel):
    condition_id: str = ""
    neg_risk: bool = False
    slug: str = ""
    num_sets: int = 0
    account_id: int = 0


class RedeemWinningsRequest(BaseModel):
    condition_id: str = ""
    neg_risk: bool = False
    slug: str = ""
    account_id: int = 0


class OrderCreationResult(BaseModel):
    """Normalized order-creation response sent back to the C++ engine.

    Venue-specific fields (polymarket status, ccxt raw payload, etc.) stay
    inside the wrapper layer; only this normalized shape crosses the API
    boundary. Engine handles success by setting exchange_order_id + ack;
    failure → remove from state, allow next-tick repost.
    """
    success: bool
    exchange_order_id: str = ""
    error_msg: str = ""
    # The venue's order as ccxt parsed it (without `info`); carries any fill
    # the venue reported in the response.
    order: Optional[Dict[str, Any]] = None


class Order(BaseModel):
    type: str
    side: str
    status: str
    symbol: str
    exchange_name: str
    market_type: str
    quote_asset: str
    base_asset: str
    create_timestamp: int
    expires_on_ms: int
    account_id: int = 0
    price: str
    quantity: str
    filled_quantity: str
    sizing_coeff_delta: str
    client_order_id: str = ""
    exchange_order_id: str = ""
    prediction: Optional[Prediction] = None
    strategy_id: int
    reduce_only: bool = False
    post_only: bool = False
    is_taker_in_simulation: bool = False
    is_alt_market_routed: bool = False
    route_order_to_alt_symbol: Optional[str] = None
    margin_auto_borrow: bool = False
    margin_auto_repay: bool = False
    position_side: Optional[str] = None
    is_prediction_market_order: bool = False
    clob_id: str = ""
    outcome_name: str = ""
    server_sending_deadline_ms: int = 0
