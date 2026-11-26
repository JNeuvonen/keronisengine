class NoThrottle:
    async def __aenter__(self):
        pass

    async def __aexit__(self, exc_type, exc, tb):
        pass


def convert_interval_str_to_ms(interval: str) -> int:
    interval_ms_map = {
        "1m": 60000,
        "3m": 180000,
        "5m": 300000,
        "15m": 900000,
        "30m": 1800000,
        "1h": 3600000,
        "2h": 7200000,
        "4h": 14400000,
        "6h": 21600000,
        "12h": 43200000,
        "1d": 86400000,
        "1w": 604800000,
        "1M": 2592000000,
    }
    return interval_ms_map.get(interval, 60000)


def convert_interval_str_to_minutes(interval: str) -> int:
    interval_minutes_map = {
        "1m": 1,
        "3m": 3,
        "5m": 5,
        "15m": 15,
        "30m": 30,
        "1h": 60,
        "2h": 120,
        "4h": 240,
        "6h": 360,
        "12h": 720,
        "1d": 1440,
        "1w": 10080,
        "1M": 43200,
    }
    return interval_minutes_map.get(interval, 1)


def convert_interval_str_to_bitget_granularity(interval: str) -> str:
    interval_minutes_map = {
        "1m": "1m",
        "3m": "3m",
        "5m": "5m",
        "15m": "15m",
        "30m": "30m",
        "1h": "1H",
        "2h": "2H",
        "4h": "4H",
        "6h": "6H",
        "12h": "12H",
        "1d": "1D",
        "1w": "1W",
        "1M": "1M",
    }
    return interval_minutes_map.get(interval, 1)


def get_client_key(
    exchange_name: str, market_type: str, account_id, is_public: bool
):
    if account_id is None:
        return f"{exchange_name}_{market_type}_{is_public}"
    return f"{exchange_name}_{market_type}_{account_id}_{is_public}"
