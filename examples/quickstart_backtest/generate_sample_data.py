#!/usr/bin/env python3
"""Generate a synthetic 1-minute OHLCV Parquet dataset and backtest config."""

import json
import math
import sys
from pathlib import Path

import pyarrow as pa
import pyarrow.parquet as pq


def main() -> None:
    out_dir = Path(sys.argv[1] if len(sys.argv) > 1 else "/tmp/keronis_quickstart").resolve()
    bar_dir = out_dir / "derived/bars/binance/swap/time/60000_ms/BTCUSDT"
    part_dir = bar_dir / "parquets/year=2026/month=01/day=01"
    part_dir.mkdir(parents=True, exist_ok=True)

    # 2026-01-01T00:00:00Z in milliseconds
    start_ms = 1767225600000
    interval_ms = 60_000
    num_bars = 360  # 6 hours of 1m bars

    open_times, close_times = [], []
    opens, highs, lows, closes = [], [], [], []
    volumes, quote_volumes, num_trades = [], [], []
    taker_base, taker_quote = [], []

    prev_close = 95000.0
    for i in range(num_bars):
        t_open = start_ms + i * interval_ms
        t_close = t_open + interval_ms - 1
        # Smooth multi-cycle wave so the 5/20 SMA crosses several times
        wave = 1200.0 * math.sin(2.0 * math.pi * i / 60.0) + 1.5 * i
        c = round(95000.0 + wave, 2)
        o = prev_close
        hi = round(max(o, c) + 35.0, 2)
        lo = round(min(o, c) - 35.0, 2)
        vol = 12.5
        qvol = round(vol * c, 2)

        open_times.append(t_open)
        close_times.append(t_close)
        opens.append(o)
        highs.append(hi)
        lows.append(lo)
        closes.append(c)
        volumes.append(vol)
        quote_volumes.append(qvol)
        num_trades.append(250)
        taker_base.append(vol * 0.5)
        taker_quote.append(qvol * 0.5)
        prev_close = c

    table = pa.table(
        {
            "kline_open_time_ms": pa.array(open_times, type=pa.int64()),
            "kline_close_time_ms": pa.array(close_times, type=pa.int64()),
            "open_price": pa.array(opens, type=pa.float64()),
            "high_price": pa.array(highs, type=pa.float64()),
            "low_price": pa.array(lows, type=pa.float64()),
            "close_price": pa.array(closes, type=pa.float64()),
            "volume": pa.array(volumes, type=pa.float64()),
            "quote_asset_volume": pa.array(quote_volumes, type=pa.float64()),
            "number_of_trades": pa.array(num_trades, type=pa.int64()),
            "taker_buy_base_volume": pa.array(taker_base, type=pa.float64()),
            "taker_buy_quote_volume": pa.array(taker_quote, type=pa.float64()),
        }
    )
    pq.write_table(table, part_dir / "2026-01-01.parquet")

    config = {
        "strategies": [
            {
                "id": 1,
                "strategy_name": "sma_crossover",
                "strategy_type": "crypto_swap",
                "main_exchange": "binance",
                "account_mode": "shared",
                "candle_interval_ms": interval_ms,
                "exchanges_list": [
                    {
                        "account_id": 1,
                        "exchange_name": "binance",
                        "market_type": "swap",
                    }
                ],
                "trade_tickers": [
                    {
                        "id": 1,
                        "symbol": "BTCUSDT",
                        "base_asset": "BTC",
                        "quote_asset": "USDT",
                        "is_active": True,
                        "is_closed": False,
                        "is_accepting_orders": True,
                        "is_order_book_enabled": False,
                        "is_neg_risk": False,
                        "num_outcomes": 0,
                        "order_min_size": 0.0,
                        "order_price_min_tick_size": 0.0,
                        "accepting_orders_timestamp_ms": 0,
                        "event_start_time_ms": 0,
                        "event_end_time_ms": 0,
                        "condition_id": "",
                        "slug": "",
                        "outcome_names": [],
                        "clob_token_ids": [],
                    }
                ],
                "models": [],
                "model_schedule_path": "",
                "series_slug": "",
                "is_disabled": False,
                "is_in_reduce_only": False,
                "is_market_making_strategy": False,
                "is_paper_trading": False,
                "is_trading_on_prediction_market": False,
                "is_using_all_available_symbols": False,
                "is_using_nominal_max_position": False,
                "is_using_percentage_max_position": True,
                "allow_co_strategy_compounding": True,
                "discretionary_multi_account": False,
                "needs_to_run_realtime": True,
                "percentage_max_position": 100.0,
                "nominal_max_position": 0.0,
                "max_directional_exposure": 5.0,
                "max_sizing_per_symbol": 0.0,
                "num_max_orders_10_sec": 1000,
                "num_max_orders_1_min": 1000,
                "num_max_orders_1_hour": 10000,
            }
        ],
        "discretionary_accounts": [],
        "market_data": {
            "ohlcv_klines": [
                {
                    "path": str(bar_dir),
                    "symbol": "BTCUSDT",
                    "exchange_name": "binance",
                    "market_type": "swap",
                }
            ],
            "trades": [],
            "aggr_trades": [],
            "order_book_snapshots": [],
            "order_book_updates": [],
            "predictions": [],
        },
        "simulation_start_date_ms": start_ms,
        "simulation_end_date_ms": start_ms + num_bars * interval_ms,
        "market_data_reading_offset_ms": 0,
        "fee_rate_coeff": 1.0,
        "swap_leverage": 5.0,
        "exchanges": [
            {
                "exchange_name": "binance",
                "market_type": "swap",
                "taker_fee_rate": 0.0005,
                "maker_fee_rate": 0.0002,
                "maker_rebate_rate": 0.0,
                "accounts": [
                    {
                        "account_id": 1,
                        "quote_assets": [
                            {
                                "asset": "USDT",
                                "initial_balance": 100000.0,
                                "price_multiplier": 1.0,
                            }
                        ],
                    }
                ],
            }
        ],
        "is_strategy_development": False,
        "emit_trading_logs": False,
        "ohlcv_delay_ms": 0,
        "skip_reading_data_with_offset_on_trades": True,
        "market_data_library_root_path": str(out_dir),
        "output_dir": str(out_dir / "results"),
        "autofill_orders": True,
        "num_required_klines_on_datasets": 1,
        "read_all_market_data_at_once": True,
        "skip_historical_order_accumulation_on_backtest": False,
        "run_strategy_on_bar_arrival": True,
        "latency_to_exchange": 0,
        "ack_cancels_at_exchange_removal": True,
        "use_trade_price_for_fills": False,
        "show_realtime_dashboard": False,
        "aggregate_klines_from_trades": False,
        "generate_predictions_from_klines": False,
        "market_data_replay": {
            "load_mode": "eager",
            "chunk_duration_ms": 3600000,
        },
    }

    config_path = out_dir / "config.json"
    config_path.write_text(json.dumps(config, indent=2) + "\n")
    print(f"Generated sample data and config in {out_dir}")


if __name__ == "__main__":
    main()
