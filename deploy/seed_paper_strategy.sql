-- Seeds a paper account and the built-in `test_quoter` strategy on BTCUSDT
-- so `live_infra` can start against a fresh Postgres instance.

INSERT INTO account (account_id, name, exchange_name, market_type, account_type, nav, starting_balance)
VALUES (1, 'paper-default', 'hyperliquid', 'swap', 'paper', 10000.0, 10000.00)
ON CONFLICT (account_id) DO NOTHING;

INSERT INTO strategy (
    id,
    strategy_name,
    strategy_type,
    main_exchange,
    is_disabled,
    is_paper_trading,
    is_in_reduce_only,
    is_market_making_strategy,
    is_trading_on_prediction_market,
    is_using_all_available_symbols,
    is_using_percentage_max_position,
    percentage_max_position,
    is_using_nominal_max_position,
    nominal_max_position,
    max_directional_exposure,
    max_sizing_per_symbol,
    candle_interval_ms,
    needs_to_run_realtime,
    allow_co_strategy_compounding,
    account_mode,
    num_max_orders_10_sec,
    num_max_orders_1_min,
    num_max_orders_1_hour,
    version,
    backtest_json_format_version,
    exchanges_list
)
VALUES (
    1,
    'test_quoter',
    'crypto_swap',
    'hyperliquid',
    FALSE,
    TRUE,
    FALSE,
    TRUE,
    FALSE,
    FALSE,
    FALSE,
    0.0,
    TRUE,
    100.0,
    1.0,
    100.0,
    60000,
    TRUE,
    FALSE,
    'shared',
    10,
    60,
    600,
    '2.0',
    '3.0',
    '[{"account_id": 1, "exchange_name": "hyperliquid", "market_type": "swap"}]'::jsonb
)
ON CONFLICT (id) DO NOTHING;

INSERT INTO trade_ticker (
    id,
    strategy_id,
    symbol,
    base_asset,
    quote_asset,
    is_active
)
VALUES (
    1,
    1,
    'BTCUSDT',
    'BTC',
    'USDT',
    TRUE
)
ON CONFLICT (id) DO NOTHING;

SELECT setval(pg_get_serial_sequence('account', 'id'), COALESCE((SELECT MAX(id) FROM account), 1));
SELECT setval(pg_get_serial_sequence('strategy', 'id'), COALESCE((SELECT MAX(id) FROM strategy), 1));
SELECT setval(pg_get_serial_sequence('trade_ticker', 'id'), COALESCE((SELECT MAX(id) FROM trade_ticker), 1));
