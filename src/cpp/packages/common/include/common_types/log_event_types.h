#pragma once

// =============================================================================
// Log event types — single source of truth for the closed set consumed by
// trading_activity_log (DB, agent-queryable) + operator dashboard.
//
// Phase 3 of the engine logging overhaul: previously these lived in
// trading_engine/include/market_making/constants.h, accessible only to code
// that depended on trading_engine. Moving to common/ lets live_infra,
// market_data, state_machines, and common utilities all reference the same
// closed set, enforced at the LOG_* macro call site.
//
// Naming convention — SUBSYSTEM_NOUN_VERB_MODIFIER, e.g.:
//   MARKET_DATA_STREAM_RECONNECTED
//   DB_WRITE_FAILED
//   SNAPSHOT_ARCHIVE_GZIP_FAILED
//
// Per-venue detail (Binance vs Bybit) belongs in metadata.exchange_name /
// metadata.market_type, NOT in the event_type. The event_type is "what kind
// of thing," the metadata is "whose."
//
// Adding a new event type: also add a row to
// agentic_system/subsystems/cpp/common/LOG_EVENT_SCHEMA.md documenting the
// required metadata keys and emit level.
// =============================================================================

namespace Core {

// -----------------------------------------------------------------------------
// System / engine lifecycle
// -----------------------------------------------------------------------------
constexpr const char* TRADING_LOG_EVENT_TYPE_SYSTEM_PERFORMANCE =
    "SYSTEM_PERFORMANCE";
constexpr const char* TRADING_LOG_EVENT_TYPE_SYSTEM_STARTUP = "SYSTEM_STARTUP";
constexpr const char* TRADING_LOG_EVENT_TYPE_ENGINE_PERIODIC_METRICS =
    "ENGINE_PERIODIC_METRICS";
constexpr const char* TRADING_LOG_EVENT_TYPE_ENGINE_SHUTDOWN_GRACEFUL =
    "ENGINE_SHUTDOWN_GRACEFUL";
constexpr const char* TRADING_LOG_EVENT_TYPE_ENGINE_STATE_MISMATCH =
    "ENGINE_STATE_MISMATCH";
constexpr const char* TRADING_LOG_EVENT_TYPE_HUMAN_INTERVENTION_REQUIRED =
    "HUMAN_INTERVENTION_REQUIRED";
constexpr const char* TRADING_LOG_EVENT_TYPE_CALLBACK_FAILURE =
    "CALLBACK_FAILURE";

// -----------------------------------------------------------------------------
// Live-infra process lifecycle
// -----------------------------------------------------------------------------
constexpr const char* TRADING_LOG_EVENT_TYPE_LIVE_INFRA_STARTUP_BEGIN =
    "LIVE_INFRA_STARTUP_BEGIN";
constexpr const char* TRADING_LOG_EVENT_TYPE_LIVE_INFRA_STARTUP_PHASE =
    "LIVE_INFRA_STARTUP_PHASE";
constexpr const char* TRADING_LOG_EVENT_TYPE_LIVE_INFRA_STARTUP_COMPLETE =
    "LIVE_INFRA_STARTUP_COMPLETE";
constexpr const char* TRADING_LOG_EVENT_TYPE_LIVE_INFRA_SHUTDOWN_REQUESTED =
    "LIVE_INFRA_SHUTDOWN_REQUESTED";

// -----------------------------------------------------------------------------
// Signal / state-updater flow
// -----------------------------------------------------------------------------
constexpr const char* TRADING_LOG_EVENT_TYPE_SIGNAL = "SIGNAL";
constexpr const char* TRADING_LOG_EVENT_TYPE_SIGNAL_PROCESSED =
    "SIGNAL_PROCESSED";
constexpr const char* TRADING_LOG_EVENT_TYPE_TRADING_STATE_SYNC =
    "TRADING_STATE_SYNC";
constexpr const char* TRADING_LOG_EVENT_TYPE_TRADING_STATE_SYNC_FAILED =
    "TRADING_STATE_SYNC_FAILED";
constexpr const char* TRADING_LOG_EVENT_TYPE_STATE_UPDATER_RUNNING =
    "STATE_UPDATER_RUNNING";
constexpr const char* TRADING_LOG_EVENT_TYPE_REALTIME_SUBSCRIPTIONS_FETCH_FAILED =
    "REALTIME_SUBSCRIPTIONS_FETCH_FAILED";
constexpr const char* TRADING_LOG_EVENT_TYPE_ENGINE_SIGNALS_FETCH_FAILED =
    "ENGINE_SIGNALS_FETCH_FAILED";
constexpr const char* TRADING_LOG_EVENT_TYPE_ENGINE_SIGNAL_ACK_FAILED =
    "ENGINE_SIGNAL_ACK_FAILED";
constexpr const char* TRADING_LOG_EVENT_TYPE_STRATEGY_COMMAND_STATUS_FAILED =
    "STRATEGY_COMMAND_STATUS_FAILED";

// -----------------------------------------------------------------------------
// Order lifecycle
// -----------------------------------------------------------------------------
constexpr const char* TRADING_LOG_EVENT_TYPE_ORDER_CREATED = "ORDER_CREATED";
constexpr const char* TRADING_LOG_EVENT_TYPE_ORDER_CANCELLED =
    "ORDER_CANCELLED";
constexpr const char* TRADING_LOG_EVENT_TYPE_ORDER_FILLED = "ORDER_FILLED";
constexpr const char* TRADING_LOG_EVENT_TYPE_ORDER_REJECTED = "ORDER_REJECTED";
constexpr const char* TRADING_LOG_EVENT_TYPE_ORDER_REJECTED_MIN_NOTIONAL =
    "ORDER_REJECTED_MIN_NOTIONAL";
constexpr const char* TRADING_LOG_EVENT_TYPE_ORDER_REJECTED_SYMBOL_UNKNOWN =
    "ORDER_REJECTED_SYMBOL_UNKNOWN";
constexpr const char* TRADING_LOG_EVENT_TYPE_ORDER_BLOCKED = "ORDER_BLOCKED";
constexpr const char* TRADING_LOG_EVENT_TYPE_LATE_FILL_ON_CANCELLED =
    "LATE_FILL_ON_CANCELLED";
constexpr const char* TRADING_LOG_EVENT_TYPE_FILL_WITHOUT_MATCHING_ORDER =
    "FILL_WITHOUT_MATCHING_ORDER";

// -----------------------------------------------------------------------------
// Trade lifecycle
// -----------------------------------------------------------------------------
constexpr const char* TRADING_LOG_EVENT_TYPE_TRADE_OPENED = "TRADE_OPENED";
constexpr const char* TRADING_LOG_EVENT_TYPE_TRADE_CLOSED = "TRADE_CLOSED";
constexpr const char* TRADING_LOG_EVENT_TYPE_TRADE_REMOVED_INVENTORY_MISMATCH =
    "TRADE_REMOVED_INVENTORY_MISMATCH";
constexpr const char* TRADING_LOG_EVENT_TYPE_TRADE_MANUALLY_REMOVED =
    "TRADE_MANUALLY_REMOVED";
constexpr const char* TRADING_LOG_EVENT_TYPE_TRADE_FORCE_CLOSED =
    "TRADE_FORCE_CLOSED";
constexpr const char* TRADING_LOG_EVENT_TYPE_INVENTORY_CLOSE_ORDER_CREATED =
    "CLOSE_NONTRACKED_INVENTORY";

// -----------------------------------------------------------------------------
// Strategy state
// -----------------------------------------------------------------------------
constexpr const char* TRADING_LOG_EVENT_TYPE_STRATEGY_ACTIVATED =
    "STRATEGY_ACTIVATED";
constexpr const char* TRADING_LOG_EVENT_TYPE_STRATEGY_PAUSED =
    "STRATEGY_PAUSED";
constexpr const char* TRADING_LOG_EVENT_TYPE_STRATEGY_RESUMED =
    "STRATEGY_RESUMED";
constexpr const char* TRADING_LOG_EVENT_TYPE_STRATEGY_HEARTBEAT =
    "STRATEGY_HEARTBEAT";
constexpr const char* TRADING_LOG_EVENT_TYPE_STRATEGY_REGIME_HEARTBEAT =
    "STRATEGY_REGIME_HEARTBEAT";
constexpr const char* TRADING_LOG_EVENT_TYPE_STRATEGY_REGIME_FLIP =
    "STRATEGY_REGIME_FLIP";
constexpr const char* TRADING_LOG_EVENT_TYPE_MODEL_BUNDLE_SEED =
    "MODEL_BUNDLE_SEED";
constexpr const char* TRADING_LOG_EVENT_TYPE_MODEL_BUNDLE_UPDATE =
    "MODEL_BUNDLE_UPDATE";

// -----------------------------------------------------------------------------
// Private (account) streams — user-data WS, order/fill pushes, balance deltas
// -----------------------------------------------------------------------------
constexpr const char* TRADING_LOG_EVENT_TYPE_PRIVATE_STREAM_MSG =
    "PRIVATE_STREAM_MSG";
constexpr const char* TRADING_LOG_EVENT_TYPE_PRIVATE_STREAM_CONNECTED =
    "PRIVATE_STREAM_CONNECTED";
constexpr const char* TRADING_LOG_EVENT_TYPE_PRIVATE_STREAM_DISCONNECTED =
    "PRIVATE_STREAM_DISCONNECTED";
constexpr const char* TRADING_LOG_EVENT_TYPE_PRIVATE_STREAM_AUTH_FAILED =
    "PRIVATE_STREAM_AUTH_FAILED";
constexpr const char* TRADING_LOG_EVENT_TYPE_PRIVATE_STREAM_STALE =
    "PRIVATE_STREAM_STALE";
constexpr const char* TRADING_LOG_EVENT_TYPE_PRIVATE_STREAM_PARSE_ERROR =
    "PRIVATE_STREAM_PARSE_ERROR";

// -----------------------------------------------------------------------------
// Account / credentials / balances
// -----------------------------------------------------------------------------
constexpr const char* TRADING_LOG_EVENT_TYPE_ACCOUNT_INIT_SUCCESS =
    "ACCOUNT_INIT_SUCCESS";
constexpr const char* TRADING_LOG_EVENT_TYPE_ACCOUNT_INIT_FAILED =
    "ACCOUNT_INIT_FAILED";
constexpr const char* TRADING_LOG_EVENT_TYPE_ACCOUNT_CREDENTIAL_FETCH_FAILED =
    "ACCOUNT_CREDENTIAL_FETCH_FAILED";
constexpr const char* TRADING_LOG_EVENT_TYPE_ACCOUNT_BALANCE_FETCH_FAILED =
    "ACCOUNT_BALANCE_FETCH_FAILED";
constexpr const char* TRADING_LOG_EVENT_TYPE_ACCOUNT_ERROR = "ACCOUNT_ERROR";

// -----------------------------------------------------------------------------
// Exchange info / symbol cache
// -----------------------------------------------------------------------------
constexpr const char* TRADING_LOG_EVENT_TYPE_EXCHANGE_INFO_PRELOADED =
    "EXCHANGE_INFO_PRELOADED";
constexpr const char* TRADING_LOG_EVENT_TYPE_EXCHANGE_INFO_LOAD_FAILED =
    "EXCHANGE_INFO_LOAD_FAILED";
constexpr const char* TRADING_LOG_EVENT_TYPE_EXCHANGE_INFO_FALLBACK_TO_DISK =
    "EXCHANGE_INFO_FALLBACK_TO_DISK";
constexpr const char* TRADING_LOG_EVENT_TYPE_SYMBOL_INFO_REFRESH_FAILED =
    "SYMBOL_INFO_REFRESH_FAILED";
constexpr const char* TRADING_LOG_EVENT_TYPE_SYMBOL_INFO_PERSIST_FAILED =
    "SYMBOL_INFO_PERSIST_FAILED";

// -----------------------------------------------------------------------------
// Market data streams — public (orderbook / trade / bar / BBA / polymarket)
// -----------------------------------------------------------------------------
constexpr const char* TRADING_LOG_EVENT_TYPE_MARKET_DATA_STREAM_CONNECTED =
    "MARKET_DATA_STREAM_CONNECTED";
constexpr const char* TRADING_LOG_EVENT_TYPE_MARKET_DATA_STREAM_DISCONNECTED =
    "MARKET_DATA_STREAM_DISCONNECTED";
constexpr const char* TRADING_LOG_EVENT_TYPE_MARKET_DATA_STREAM_RECONNECTING =
    "MARKET_DATA_STREAM_RECONNECTING";
constexpr const char* TRADING_LOG_EVENT_TYPE_MARKET_DATA_STREAM_RECONNECTED =
    "MARKET_DATA_STREAM_RECONNECTED";
constexpr const char* TRADING_LOG_EVENT_TYPE_MARKET_DATA_STREAM_BACKOFF_EXCEEDED =
    "MARKET_DATA_STREAM_BACKOFF_EXCEEDED";
constexpr const char* TRADING_LOG_EVENT_TYPE_MARKET_DATA_STREAM_PARSE_ERROR =
    "MARKET_DATA_STREAM_PARSE_ERROR";
constexpr const char* TRADING_LOG_EVENT_TYPE_MARKET_DATA_STREAM_STOPPING =
    "MARKET_DATA_STREAM_STOPPING";
constexpr const char* TRADING_LOG_EVENT_TYPE_MARKET_DATA_INIT_PHASE =
    "MARKET_DATA_INIT_PHASE";
constexpr const char* TRADING_LOG_EVENT_TYPE_MARKET_DATA_NO_ACTIVE_SYMBOLS =
    "MARKET_DATA_NO_ACTIVE_SYMBOLS";
constexpr const char* TRADING_LOG_EVENT_TYPE_MARKET_DATA_BAR_SYNC_PROGRESS =
    "MARKET_DATA_BAR_SYNC_PROGRESS";
constexpr const char* TRADING_LOG_EVENT_TYPE_MARKET_DATA_STREAM_THROUGHPUT =
    "MARKET_DATA_STREAM_THROUGHPUT";
constexpr const char* TRADING_LOG_EVENT_TYPE_MARKET_EVENTS_QUEUE_BACKPRESSURE =
    "MARKET_EVENTS_QUEUE_BACKPRESSURE";
constexpr const char* TRADING_LOG_EVENT_TYPE_ORDER_ACKNOWLEDGED =
    "ORDER_ACKNOWLEDGED";
constexpr const char* TRADING_LOG_EVENT_TYPE_MARKET_DATA_RECORDER_LIFECYCLE =
    "MARKET_DATA_RECORDER_LIFECYCLE";
constexpr const char* TRADING_LOG_EVENT_TYPE_CRON_RUNNER_LIFECYCLE =
    "CRON_RUNNER_LIFECYCLE";
constexpr const char* TRADING_LOG_EVENT_TYPE_BACKTEST_PHASE = "BACKTEST_PHASE";

// -----------------------------------------------------------------------------
// Orderbook sync state
// -----------------------------------------------------------------------------
constexpr const char* TRADING_LOG_EVENT_TYPE_ORDERBOOK_OUT_OF_SYNC =
    "ORDERBOOK_OUT_OF_SYNC";
constexpr const char* TRADING_LOG_EVENT_TYPE_ORDERBOOK_RESYNCED =
    "ORDERBOOK_RESYNCED";
constexpr const char* TRADING_LOG_EVENT_TYPE_ORDERBOOK_CHECKSUM_MISMATCH =
    "ORDERBOOK_CHECKSUM_MISMATCH";
constexpr const char* TRADING_LOG_EVENT_TYPE_ORDERBOOK_SEQUENCE_GAP =
    "ORDERBOOK_SEQUENCE_GAP";
constexpr const char* TRADING_LOG_EVENT_TYPE_ORDERBOOK_SNAPSHOT_LOADED =
    "ORDERBOOK_SNAPSHOT_LOADED";
constexpr const char* TRADING_LOG_EVENT_TYPE_ORDERBOOK_SNAPSHOT_FAILED =
    "ORDERBOOK_SNAPSHOT_FAILED";

// -----------------------------------------------------------------------------
// State machines (feature engineering)
// -----------------------------------------------------------------------------
constexpr const char* TRADING_LOG_EVENT_TYPE_SM_STATE_BIN_LOADED =
    "SM_STATE_BIN_LOADED";
constexpr const char* TRADING_LOG_EVENT_TYPE_SM_STATE_BIN_LOAD_FAILED =
    "SM_STATE_BIN_LOAD_FAILED";
constexpr const char* TRADING_LOG_EVENT_TYPE_SM_STATE_BIN_CORRUPT =
    "SM_STATE_BIN_CORRUPT";
constexpr const char* TRADING_LOG_EVENT_TYPE_SM_STATE_BIN_PERSISTED =
    "SM_STATE_BIN_PERSISTED";
constexpr const char* TRADING_LOG_EVENT_TYPE_SM_STATE_BIN_PERSIST_FAILED =
    "SM_STATE_BIN_PERSIST_FAILED";
constexpr const char* TRADING_LOG_EVENT_TYPE_SM_DEPENDENCY_BUFFER_FLUSHED =
    "SM_DEPENDENCY_BUFFER_FLUSHED";
constexpr const char* TRADING_LOG_EVENT_TYPE_SM_DEPENDENCY_BUFFER_OVERFLOW =
    "SM_DEPENDENCY_BUFFER_OVERFLOW";
constexpr const char* TRADING_LOG_EVENT_TYPE_SM_STREAM_DISCONNECTED_RESET =
    "SM_STREAM_DISCONNECTED_RESET";
constexpr const char* TRADING_LOG_EVENT_TYPE_SM_WARMUP_COMPLETE =
    "SM_WARMUP_COMPLETE";
constexpr const char* TRADING_LOG_EVENT_TYPE_SM_WARMUP_TIMEOUT =
    "SM_WARMUP_TIMEOUT";
constexpr const char* TRADING_LOG_EVENT_TYPE_SM_WENT_LIVE = "SM_WENT_LIVE";
constexpr const char* TRADING_LOG_EVENT_TYPE_SM_SYNCER_PROGRESS =
    "SM_SYNCER_PROGRESS";
constexpr const char* TRADING_LOG_EVENT_TYPE_SM_SYNCER_SKIPPED_NON_TRADE =
    "SM_SYNCER_SKIPPED_NON_TRADE";
constexpr const char* TRADING_LOG_EVENT_TYPE_SM_SYNCER_STALE_TRADE_ID =
    "SM_SYNCER_STALE_TRADE_ID";
constexpr const char* TRADING_LOG_EVENT_TYPE_SM_SYNCER_NO_STARTING_POINT =
    "SM_SYNCER_NO_STARTING_POINT";
constexpr const char* TRADING_LOG_EVENT_TYPE_SM_SYNCER_NO_HISTORICAL_TRADES =
    "SM_SYNCER_NO_HISTORICAL_TRADES";
constexpr const char* TRADING_LOG_EVENT_TYPE_SM_REGISTERED = "SM_REGISTERED";

// -----------------------------------------------------------------------------
// Observability self-monitoring (the heartbeat + snapshot archive machinery)
// -----------------------------------------------------------------------------
constexpr const char* TRADING_LOG_EVENT_TYPE_OBSERVABILITY_UPSERT_TRANSACTION_FAILED =
    "OBSERVABILITY_UPSERT_TRANSACTION_FAILED";
constexpr const char* TRADING_LOG_EVENT_TYPE_OBSERVABILITY_UPSERT_TIMEOUT =
    "OBSERVABILITY_UPSERT_TIMEOUT";
constexpr const char* TRADING_LOG_EVENT_TYPE_OBSERVABILITY_UPSERT_SLOW =
    "OBSERVABILITY_UPSERT_SLOW";
constexpr const char* TRADING_LOG_EVENT_TYPE_SNAPSHOT_ARCHIVE_MKDIR_FAILED =
    "SNAPSHOT_ARCHIVE_MKDIR_FAILED";
constexpr const char* TRADING_LOG_EVENT_TYPE_SNAPSHOT_ARCHIVE_GZIP_FAILED =
    "SNAPSHOT_ARCHIVE_GZIP_FAILED";
constexpr const char* TRADING_LOG_EVENT_TYPE_SNAPSHOT_ARCHIVE_FILE_OPEN_FAILED =
    "SNAPSHOT_ARCHIVE_FILE_OPEN_FAILED";
constexpr const char* TRADING_LOG_EVENT_TYPE_SNAPSHOT_INDEX_INSERT_FAILED =
    "SNAPSHOT_INDEX_INSERT_FAILED";

// -----------------------------------------------------------------------------
// DB operations (postgre_utils)
// -----------------------------------------------------------------------------
constexpr const char* TRADING_LOG_EVENT_TYPE_DB_CONNECT_FAILED =
    "DB_CONNECT_FAILED";
constexpr const char* TRADING_LOG_EVENT_TYPE_DB_CONNECTION_LOST =
    "DB_CONNECTION_LOST";
constexpr const char* TRADING_LOG_EVENT_TYPE_DB_WRITE_FAILED = "DB_WRITE_FAILED";
constexpr const char* TRADING_LOG_EVENT_TYPE_DB_QUERY_FAILED = "DB_QUERY_FAILED";
constexpr const char* TRADING_LOG_EVENT_TYPE_DB_TRANSACTION_ROLLBACK_UNEXPECTED =
    "DB_TRANSACTION_ROLLBACK_UNEXPECTED";

// -----------------------------------------------------------------------------
// NATS messaging
// -----------------------------------------------------------------------------
constexpr const char* TRADING_LOG_EVENT_TYPE_NATS_CONNECTED = "NATS_CONNECTED";
constexpr const char* TRADING_LOG_EVENT_TYPE_NATS_CONNECT_FAILED =
    "NATS_CONNECT_FAILED";
constexpr const char* TRADING_LOG_EVENT_TYPE_NATS_PUBLISH_FAILED =
    "NATS_PUBLISH_FAILED";
constexpr const char* TRADING_LOG_EVENT_TYPE_NATS_CONNECTION_LOST =
    "NATS_CONNECTION_LOST";
constexpr const char* TRADING_LOG_EVENT_TYPE_NATS_RECONNECTED =
    "NATS_RECONNECTED";
constexpr const char* TRADING_LOG_EVENT_TYPE_NATS_ERROR = "NATS_ERROR";

// -----------------------------------------------------------------------------
// CCXT proxy (HTTP client to Python CCXT wrapper)
// -----------------------------------------------------------------------------
constexpr const char* TRADING_LOG_EVENT_TYPE_CCXT_PROXY_REQUEST_FAILED =
    "CCXT_PROXY_REQUEST_FAILED";
constexpr const char* TRADING_LOG_EVENT_TYPE_CCXT_PROXY_RESPONSE_PARSE_FAILED =
    "CCXT_PROXY_RESPONSE_PARSE_FAILED";
constexpr const char* TRADING_LOG_EVENT_TYPE_CCXT_PROXY_RATE_LIMITED =
    "CCXT_PROXY_RATE_LIMITED";

// -----------------------------------------------------------------------------
// Parquet I/O (common data-library utilities)
// -----------------------------------------------------------------------------
constexpr const char* TRADING_LOG_EVENT_TYPE_PARQUET_READ_FAILED =
    "PARQUET_READ_FAILED";
constexpr const char* TRADING_LOG_EVENT_TYPE_PARQUET_WRITE_FAILED =
    "PARQUET_WRITE_FAILED";
constexpr const char* TRADING_LOG_EVENT_TYPE_PARQUET_SCHEMA_MISMATCH =
    "PARQUET_SCHEMA_MISMATCH";

// -----------------------------------------------------------------------------
// Crypto-margin venue operations (Binance cross-margin)
// -----------------------------------------------------------------------------
constexpr const char* TRADING_LOG_EVENT_TYPE_LOAN_REPAYED = "REPAY_LOAN";
constexpr const char* TRADING_LOG_EVENT_TYPE_LOAN_REPAYED_FAILED =
    "REPAY_LOAN_FAILED";
constexpr const char* TRADING_LOG_EVENT_TYPE_LOAN_REPAY_SUCCESS =
    "REPAY_LOAN_SUCCESS";
constexpr const char* TRADING_LOG_EVENT_TYPE_CRYPTO_MARGIN_BALANCE_SYNCED =
    "CRYPTO_MARGIN_BALANCE_SYNCED";
constexpr const char* TRADING_LOG_EVENT_TYPE_CRYPTO_MARGIN_BALANCE_SYNC_FAILED =
    "CRYPTO_MARGIN_BALANCE_SYNC_FAILED";
constexpr const char* TRADING_LOG_EVENT_TYPE_CRYPTO_MARGIN_FUNDING_TOPUP_TRIGGERED =
    "CRYPTO_MARGIN_FUNDING_TOPUP_TRIGGERED";

// -----------------------------------------------------------------------------
// Polymarket venue-specific
// -----------------------------------------------------------------------------
constexpr const char* TRADING_LOG_EVENT_TYPE_POLYMARKET_FILL_CONFIRMATION_TIMEOUT =
    "POLYMARKET_FILL_CONFIRMATION_TIMEOUT";
constexpr const char* TRADING_LOG_EVENT_TYPE_POLYMARKET_SYNTHETIC_CONTRACT_MINT =
    "POLYMARKET_SYNTHETIC_CONTRACT_MINT";
constexpr const char* TRADING_LOG_EVENT_TYPE_POLYMARKET_SYNTHETIC_CONTRACT_MINT_FAILED =
    "POLYMARKET_SYNTHETIC_CONTRACT_MINT_FAILED";
constexpr const char* TRADING_LOG_EVENT_TYPE_POLYMARKET_WORKER_REJECTED =
    "POLYMARKET_WORKER_REJECTED";
constexpr const char* TRADING_LOG_EVENT_TYPE_POLYMARKET_EVENT_RESOLVED =
    "POLYMARKET_EVENT_RESOLVED";
constexpr const char* TRADING_LOG_EVENT_TYPE_POLYMARKET_EVENT_CLOSED =
    "POLYMARKET_EVENT_CLOSED";
constexpr const char* TRADING_LOG_EVENT_TYPE_POLYMARKET_ORDER_AUTH_FAILED =
    "POLYMARKET_ORDER_AUTH_FAILED";
constexpr const char* TRADING_LOG_EVENT_TYPE_POLYMARKET_POLL_RESULT =
    "POLYMARKET_POLL_RESULT";
constexpr const char* TRADING_LOG_EVENT_TYPE_POLYMARKET_TICKER_CLOSED =
    "POLYMARKET_TICKER_CLOSED";
constexpr const char* TRADING_LOG_EVENT_TYPE_POLYMARKET_ORPHAN_CANCEL_FAILED =
    "POLYMARKET_ORPHAN_CANCEL_FAILED";
constexpr const char* TRADING_LOG_EVENT_TYPE_POLYMARKET_ORDER_STATE_VERIFY =
    "POLYMARKET_ORDER_STATE_VERIFY";
constexpr const char* TRADING_LOG_EVENT_TYPE_POLYMARKET_INIT_COMPLETED =
    "POLYMARKET_INIT_COMPLETED";
constexpr const char* TRADING_LOG_EVENT_TYPE_POLYMARKET_MINT_TICKER_MISSING =
    "POLYMARKET_MINT_TICKER_MISSING";
constexpr const char* TRADING_LOG_EVENT_TYPE_POLYMARKET_REDEEM =
    "POLYMARKET_REDEEM";
constexpr const char* TRADING_LOG_EVENT_TYPE_POLYMARKET_REDEEM_FAILED =
    "POLYMARKET_REDEEM_FAILED";

// -----------------------------------------------------------------------------
// Order state reconciliation (live_infra/state_updater order verification
// against the exchange's view) — distinct from order lifecycle because these
// events fire when the engine and exchange disagree, not when an order is
// placed/filled/cancelled in the normal flow.
// -----------------------------------------------------------------------------
constexpr const char* TRADING_LOG_EVENT_TYPE_ORDER_STATE_VERIFY =
    "ORDER_STATE_VERIFY";
constexpr const char* TRADING_LOG_EVENT_TYPE_ORDER_STATE_VERIFY_MISMATCH =
    "ORDER_STATE_VERIFY_MISMATCH";
constexpr const char* TRADING_LOG_EVENT_TYPE_ORDER_NOT_FOUND_ON_EXCHANGE =
    "ORDER_NOT_FOUND_ON_EXCHANGE";
constexpr const char* TRADING_LOG_EVENT_TYPE_UNACKED_ORDER_TIMEOUT =
    "UNACKED_ORDER_TIMEOUT";
constexpr const char* TRADING_LOG_EVENT_TYPE_FAILED_CANCEL_RETRY =
    "FAILED_CANCEL_RETRY";
constexpr const char* TRADING_LOG_EVENT_TYPE_ORDER_CREATE_FAILED_MSG =
    "ORDER_CREATE_FAILED_MSG";
constexpr const char* TRADING_LOG_EVENT_TYPE_ORDER_CANCEL_FAILED_MSG =
    "ORDER_CANCEL_FAILED_MSG";
constexpr const char* TRADING_LOG_EVENT_TYPE_FILL_STATUS_UPDATE_FAILED =
    "FILL_STATUS_UPDATE_FAILED";

// -----------------------------------------------------------------------------
// Inventory reconciliation — detect/repair divergence between engine's view
// of positions and the exchange's account state.
// -----------------------------------------------------------------------------
constexpr const char* TRADING_LOG_EVENT_TYPE_INVENTORY_STATE_UPDATE_FAILED =
    "INVENTORY_STATE_UPDATE_FAILED";
constexpr const char* TRADING_LOG_EVENT_TYPE_INVENTORY_MISMATCH_DETECTED =
    "INVENTORY_MISMATCH_DETECTED";

// -----------------------------------------------------------------------------
// User-data stream (private WS feed) message handling
// -----------------------------------------------------------------------------
constexpr const char* TRADING_LOG_EVENT_TYPE_USER_DATA_STREAM_PARSE_FAILED =
    "USER_DATA_STREAM_PARSE_FAILED";

// -----------------------------------------------------------------------------
// Live infra — miscellaneous internal operational events
// -----------------------------------------------------------------------------
constexpr const char* TRADING_LOG_EVENT_TYPE_PYTHON_PROXY_HEALTHCHECK_FAILED =
    "PYTHON_PROXY_HEALTHCHECK_FAILED";
constexpr const char* TRADING_LOG_EVENT_TYPE_PREVENT_TRADING_LOCK_CREATED =
    "PREVENT_TRADING_LOCK_CREATED";
constexpr const char* TRADING_LOG_EVENT_TYPE_EXTENSIVE_HEALTH_CHECK_FAILED =
    "EXTENSIVE_HEALTH_CHECK_FAILED";
constexpr const char* TRADING_LOG_EVENT_TYPE_TRADING_STATE_RECOVER_FAILED =
    "TRADING_STATE_RECOVER_FAILED";
constexpr const char* TRADING_LOG_EVENT_TYPE_TRADING_STATE_PARSE_FAILED =
    "TRADING_STATE_PARSE_FAILED";
constexpr const char* TRADING_LOG_EVENT_TYPE_LIVE_EVENT_CONSUMER_ERROR =
    "LIVE_EVENT_CONSUMER_ERROR";
constexpr const char* TRADING_LOG_EVENT_TYPE_MISC_ORDER_CLEANUP =
    "MISC_ORDER_CLEANUP";

// -----------------------------------------------------------------------------
// Trade bookkeeping (post-fill aggregation and partial-close events)
// -----------------------------------------------------------------------------
constexpr const char* TRADING_LOG_EVENT_TYPE_TRADE_FILL_APPENDED =
    "TRADE_FILL_APPENDED";
constexpr const char* TRADING_LOG_EVENT_TYPE_TRADE_PARTIALLY_CLOSED =
    "TRADE_PARTIALLY_CLOSED";
constexpr const char* TRADING_LOG_EVENT_TYPE_POLYMARKET_FILL_REVERTED =
    "POLYMARKET_FILL_REVERTED";

// -----------------------------------------------------------------------------
// Engine exposures / subscription lifecycle (engine-internal housekeeping)
// -----------------------------------------------------------------------------
constexpr const char* TRADING_LOG_EVENT_TYPE_SYMBOL_PRICE_MISSING =
    "SYMBOL_PRICE_MISSING";
constexpr const char* TRADING_LOG_EVENT_TYPE_ORDERBOOK_SUBSCRIPTION_CHANGED =
    "ORDERBOOK_SUBSCRIPTION_CHANGED";

// =============================================================================
// Log levels — actionable semantics. See agentic_system/skills/LOGGING.md.
//
//   ERROR    — actionable. Operator or agent must do something. Never used for
//              expected cache misses, fallbacks, or "not found" when not-found
//              is a nominal outcome. If an ERROR fires with no follow-up, it
//              is noise and the emit site is wrong.
//   WARNING  — anomaly the system recovered from. Nominal flow diverged but
//              the outcome was correct (late fill handled, retry succeeded).
//   INFO     — business event (order, trade, signal) or periodic metric.
//   CRASH    — terminal. The process is about to die. Last-ditch context.
// =============================================================================
constexpr const char* LOG_LEVEL_ERROR = "ERROR";
constexpr const char* LOG_LEVEL_WARNING = "WARNING";
constexpr const char* LOG_LEVEL_INFO = "INFO";
constexpr const char* LOG_LEVEL_CRASH = "CRASH";

}  // namespace Core
