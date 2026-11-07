#pragma once

// Trading log event_type constants + LOG_LEVEL_* are defined in common/ as of
// Phase 3 of the logging overhaul — this header re-exports them so existing
// includes (`market_making/constants.h`) keep working. New code should prefer
// `common_types/log_event_types.h` directly.
#include "common_types/log_event_types.h"

namespace Core {

constexpr const char* EXCHANGE_BINANCE = "binance";
constexpr const char* EXCHANGE_BYBIT = "bybit";
constexpr const char* EXCHANGE_BITGET = "bitget";
constexpr const char* EXCHANGE_GATEIO = "gateio";

constexpr const char* SPOT_MARKET_TYPE = "spot";
constexpr const char* SWAP_MARKET_TYPE = "swap";

constexpr int ENGINE_MARKET_EVENTS_BACKPRESSURE_LIMIT = 75;

struct OrderRateLimit {
    int limit;
    long long interval_ms;
};

constexpr OrderRateLimit BINANCE_10_SEC_ORDER_RATE_LIMIT = {300, 10000};
constexpr OrderRateLimit BINANCE_1_MIN_ORDER_RATE_LIMIT = {1200, 60000};

// ---------------------------------------------------------------------------
// Log emit cooldowns (milliseconds) — used by RateLimitedEmitter to keep
// high-frequency warnings from flooding the DB. See
// trading_engine/include/market_making/rate_limited_emitter.h.
// ---------------------------------------------------------------------------
constexpr long long LOG_COOLDOWN_ORDER_REJECTED_MIN_NOTIONAL_MS = 60 * 1000;
constexpr long long LOG_COOLDOWN_ORDERBOOK_OUT_OF_SYNC_MS = 30 * 1000;
constexpr long long LOG_COOLDOWN_STRATEGY_HEARTBEAT_MS = 30 * 1000;
constexpr long long LOG_COOLDOWN_ENGINE_PERIODIC_METRICS_MS = 10 * 1000;

}  // namespace Core
