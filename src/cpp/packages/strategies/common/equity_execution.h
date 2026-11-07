#pragma once
//
// Order-lifecycle machinery shared by US-equity strategies.
//
// Every trap here was paid for once already in a live strategy: an expired-but-
// still-listed order counted as live suppresses the re-post its TTL exists to
// trigger and strands the position open for the rest of the run; a slice sized
// below the min-notional filter is rejected silently; a close order whose
// sizing_coeff_delta is not its share of the trade mis-accounts a partial cover.
// The rules a strategy owns are its own; this is not one of them.
//
#include <optional>
#include <string>

#include "market_making/dtypes/strategy_decision.h"
#include "market_making/engine.h"
#include "market_making/strategy.h"
#include "market_making/ticker.h"
#include "mpreal.h"

namespace EquityExecution {

inline constexpr const char* kExchange = "polygon";
inline constexpr const char* kMarketType = "stock";

// A slice below this is not worth its own order and trips the min-notional
// filter, so the slice COUNT is reduced rather than the slice size.
inline constexpr double kMinSliceNotional = 500.0;

std::string session_date_utc(long long time_ms);

// Minutes elapsed since the regular session opened, or nullopt outside it.
std::optional<int> minutes_since_open(long long now_ms);

bool has_open_position(Core::TradeTicker& trade_ticker);
mpfr::mpreal open_quantity(Core::TradeTicker& trade_ticker);

// A trade opened in an earlier session survived a flatten that was supposed to
// be unconditional. It is an escaped position, not a held one.
bool has_stale_trade(Core::TradeTicker& trade_ticker,
                     const std::string& session);

void cancel_expired_orders(long long now_ms, Core::Engine& engine,
                           Core::Strategy& strategy,
                           Core::TradeTicker& trade_ticker,
                           Core::StrategyDecision& decision);

// Orders that can still fill: neither expired nor already cancelled. Pass a
// trade_id to scope to one trade's closing children; nullopt counts opening
// orders instead.
int count_live_orders(Core::TradeTicker& trade_ticker, long long now_ms,
                      std::optional<unsigned long long> trade_id);

// Posts one opening limit order. Returns false when the order was not
// constructed, which is the caller's cue that the notional was rejected rather
// than that the entry was declined.
bool post_entry(long long now_ms, Core::Engine& engine,
                Core::Strategy& strategy, Core::TradeTicker& trade_ticker,
                Core::StrategyDecision& decision, Core::OrderSide side,
                double limit_price, double notional, double sizing_coeff_delta,
                long long ttl_ms);

// Works every open trade out in slices, topping the working set back up to
// `slices` on each call so a missed fill re-posts instead of stranding the
// position.
int work_out_position(long long now_ms, Core::Engine& engine,
                       Core::Strategy& strategy,
                       Core::TradeTicker& trade_ticker,
                       Core::StrategyDecision& decision, double limit_price,
                       int slices, long long ttl_ms);

}  // namespace EquityExecution
