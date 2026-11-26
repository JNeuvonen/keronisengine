from cpp_live_proxy.ccxt_wrapper_inventory import CcxtWrapperInventoryMixin


def test_parse_hyperliquid_inventory_state():
    # HL fetch_balance().info shape.
    sample = {
        "info": {
            "marginSummary": {
                "accountValue": "1000.0",
                "totalMarginUsed": "200.0",
            },
            "assetPositions": [
                {
                    "type": "oneWay",
                    "position": {"coin": "BTC", "szi": "0.5", "marginUsed": "150.0"},
                },
                {
                    "type": "oneWay",
                    "position": {"coin": "ETH", "szi": "-2.0", "marginUsed": "50.0"},
                },
            ],
        }
    }
    # The parser does not use `self`; call it unbound with a dummy receiver.
    out = CcxtWrapperInventoryMixin._parse_hyperliquid_inventory_state(None, sample)

    assert len(out["quote_assets"]) == 1
    qa = out["quote_assets"][0]
    assert qa["asset"] == "USDC"
    assert qa["quantity"] == "1000.0"
    assert qa["used"] == "200.0"
    assert float(qa["free"]) == 800.0

    assert len(out["inventory"]) == 2
    btc, eth = out["inventory"]
    assert btc["asset"] == "BTCUSDC"
    assert btc["quote_conversion_symbol"] == "BTCUSDC"
    assert btc["total"] == "0.5"
    assert btc["used_margin"] == "150.0"
    assert btc["exchange_name"] == "hyperliquid"
    assert btc["market_type"] == "swap"
    assert btc["position_side"] == "BOTH"
    assert eth["asset"] == "ETHUSDC"
    assert eth["total"] == "-2.0"


def test_parse_hyperliquid_inventory_state_empty():
    out = CcxtWrapperInventoryMixin._parse_hyperliquid_inventory_state(
        None, {"info": {}}
    )
    assert out["inventory"] == []
    assert len(out["quote_assets"]) == 1
    assert out["quote_assets"][0]["quantity"] == "0"
