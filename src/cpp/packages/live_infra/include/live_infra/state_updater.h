#pragma once

#include "market_making/discretionary_accounts.h"

#include "common_types/strategy.h"
#include "common_types/trading_log.h"
#include "concurrent_queue_include_helper.h"
#include "live_infra/strategy_command_store.h"
#include "live_infra/types.h"
#include "market_data/market_data_stream_manager.h"
#include "market_data/market_event.h"
#include "market_data/stream_status.h"
// ObservabilityBatch lives in market_making/engine.h. state_updater.h
// references it in the observability_callback signature, so any TU that
// includes state_updater.h must see the type.
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <shared_mutex>
#include <unordered_set>

#include "market_making/engine.h"
#include "sqlite_cache.h"
#include "sqlite_datasets.h"
#include "threadpool.h"
#include "trading/account.h"
#include "ws_stream.h"

namespace Core {
class Engine;
class EngineStateSnapshot;
class Order;
class OrderFillInfo;
class OrderResNormalized;
class TradeTicker;
}  // namespace Core

struct TradingLog;

namespace Live {

// A venue's symbol info failed to load, so the strategy cannot get its
// tickers yet; building it now would orphan its restored trades.
struct SymbolInfoUnavailable : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class StateUpdater {
   public:
    StateUpdater();
    ~StateUpdater();

    std::optional<nlohmann::json> create_order(const Core::Order &order);
    std::optional<nlohmann::json> cancel_order(const Core::Order &order);
    std::vector<Core::PolymarketBulkCancelResult> cancel_polymarket_orders_bulk(
        const std::vector<std::string> &order_ids, long long account_id);

    std::vector<PolymarketDirect::PolymarketClient::SignedOrderBody>
    sign_polymarket_orders(const std::vector<Core::Order> &orders);

    std::vector<Core::PolymarketBulkOrderResult>
    submit_signed_polymarket_orders(
        const std::vector<PolymarketDirect::PolymarketClient::SignedOrderBody>
            &signed_orders,
        long long account_id);

    Core::PolymarketBulkOrderResult submit_signed_polymarket_order(
        const PolymarketDirect::PolymarketClient::SignedOrderBody &signed_order,
        long long account_id);

    std::vector<Core::PolymarketBulkOrderResult> create_polymarket_orders_bulk(
        const std::vector<Core::Order> &orders);

    bool is_polymarket_direct_signing_enabled() const;
    bool is_paper_strategy(int strategy_id) const;

    // Aster bulk order routing
    bool is_aster_direct_signing_enabled() const;
    std::vector<Core::PolymarketBulkOrderResult> create_aster_orders_bulk(
        const std::vector<Core::Order> &orders);
    std::vector<Core::PolymarketBulkCancelResult> cancel_aster_orders_bulk(
        const std::vector<std::string> &order_ids, long long account_id,
        const std::string &symbol);

    // Hyperliquid bulk order routing
    bool is_hyperliquid_direct_signing_enabled() const;
    std::vector<Core::PolymarketBulkOrderResult> create_hyperliquid_orders_bulk(
        const std::vector<Core::Order> &orders);
    std::vector<Core::PolymarketBulkCancelResult>
    cancel_hyperliquid_orders_bulk(const std::vector<std::string> &order_ids,
                                   long long account_id,
                                   const std::string &symbol);

    void cancel_order_by_client_order_id(const std::string &client_order_id,
                                         const std::string &exchange_name,
                                         const std::string &market_type,
                                         const std::string &symbol,
                                         const long long &account_id);

    void on_order_fill(Core::OrderFillInfo &fill);
    void handle_order_state_check(
        Core::Order &order,
        const Core::OrderResNormalized &order_res_from_exchange);

    bool sync_state_to_disk();
    void handle_invidual_order_not_found(Core::Order order);
    void recover_state_from_disk();
    void run_extensive_exchange_health_checks();
    void initialize_exchange_info();
    void system_info_log_callback(const std::string &log,
                                  const nlohmann::json &metadata);
    void system_crash_log_callback(const std::string &log,
                                   const nlohmann::json &metadata);
    void write_db_entries_from_engine_snapshot(
        Core::EngineStateSnapshot snapshot);
    void graceful_shutdown();

    void RunTradeLoop();

    void update_polymarket_event_metas(
        const std::vector<PolymarketEventMeta> &metas);
    std::optional<PolymarketEventMeta> get_polymarket_event_meta(
        const std::string &condition_id);
    std::vector<PolymarketEventMeta> get_all_polymarket_event_metas();

    void register_polymarket_ticker(
        const std::shared_ptr<Core::TradeTicker> &ticker);
    void unregister_polymarket_ticker(const Core::TradeTicker &ticker);

    void dispatch_polymarket_cancel_resolution(const Core::Order &order);

    std::unique_ptr<MarketData::MarketDataStreamManager> bars_realtime;

    moodycamel::BlockingConcurrentQueue<MarketData::MarketEvent>
        live_event_queue_;

    const std::unordered_map<long long, Trading::Account> &get_accounts()
        const {
        return accounts;
    }

    // Account streams plus the CCXT-proxy user-data stream.
    std::vector<MarketData::StreamStatus> get_private_stream_statuses();

   private:
    bool is_prod = false;
    bool observability_writes_db_ = false;
    bool observability_feeds_dashboard_ = false;

    std::atomic<bool> lock_acquired{false};
    bool dataset_needs_refetch = false;

    std::chrono::steady_clock::time_point program_start_time{
        std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point last_order_state_poll_time{
        std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point last_synced_state_to_disk_time{
        std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point last_inventory_state_poll_time{
        std::chrono::steady_clock::now()};
    std::unordered_map<std::string, std::chrono::steady_clock::time_point>
        last_stall_orders_poll_time_by_account_id;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point>
        last_borrowable_inventories_poll_time_by_account_id;
    // Keyed `<endpoint>|<exchange>|<market>` — last emit_ts_ms for
    // PYTHON_PROXY_HEALTHCHECK_FAILED, 60s cooldown per key.
    std::unordered_map<std::string, int64_t> proxy_healthcheck_last_emit_ms_;
    std::chrono::steady_clock::time_point
        last_backup_rest_api_fetch_datasets_time{
            std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point last_check_unacked_orders_time{
        std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point last_fetch_prices_time{
        std::chrono::steady_clock::now() - std::chrono::seconds(6000)};
    std::chrono::steady_clock::time_point last_check_failed_cancels_time{
        std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point last_exchange_info_update_time{
        std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point
        last_update_strategy_virtual_balances_to_db_time{
            std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point last_heartbeat_update_time{
        std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point
        last_periodic_verify_order_states_time{
            std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point last_periodic_validate_inventory_time{
        std::chrono::steady_clock::now() - std::chrono::minutes(60)};
    std::atomic<bool> validate_inventory_running{false};
    // Consecutive checks each paper trade id has disagreed with the DB.
    std::unordered_map<std::string, int> paper_trade_mismatch_streaks;
    std::atomic<bool> verify_order_states_running{false};
    std::atomic<bool> verify_polymarket_order_states_running{false};
    std::atomic<bool> inventory_state_update_running{false};
    std::atomic<bool> redeem_contracts_running{false};
    std::atomic<bool> mint_contracts_running{false};
    // Bumped each main-loop iteration; the dedicated liveness thread gates on
    // its freshness to decide whether to keep the system alive (health file +
    // venue heartbeats) or let the dead-man's-switch fire.
    std::atomic<long long> last_main_loop_tick_ms_{0};
    std::chrono::steady_clock::time_point
        last_periodic_exchange_health_checks_time{
            std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point last_periodic_update_misc_orders_time{
        std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point
        last_symbol_exchange_info_db_write_time{};
    std::unordered_map<int, double> strategy_virtual_balances;

    // Grows when the strategy sync loads a paper strategy; ticker order
    // paths read it from strategy threads.
    std::unordered_set<int> paper_strategy_ids;
    mutable std::shared_mutex paper_strategy_ids_mutex;
    std::set<std::string> paper_exchange_infos_loaded;
    // Each strategy row as last handed to the engine, with the fingerprint it
    // was fed at; the sync diffs the DB against this, never against the
    // engine. The detached ticker-entry sync reads the rows.
    struct FedStrategy {
        std::string fingerprint;
        nlohmann::json columns;
        StrategyConfig row;
    };
    std::unordered_map<int, FedStrategy> fed_strategies;
    mutable std::mutex fed_strategies_mutex;
    std::vector<StrategyConfig> fed_strategy_rows() const;
    // Fingerprints already reported as pending; the skipped ones (restart
    // only, failed build) are not retried until the row changes again.
    std::unordered_map<int, std::string> reported_pending_fingerprints;
    std::unordered_map<int, std::string> skipped_fingerprints;
    std::chrono::steady_clock::time_point last_strategy_rows_poll_time{
        std::chrono::steady_clock::now()};
    // Applies added, changed and removed strategy rows while trading; see
    // LIVE_INFRA.md "Live strategy sync".
    void poll_strategy_rows();
    struct StrategyRowState {
        std::string fingerprint;
        nlohmann::json columns;
    };
    std::optional<std::unordered_map<int, StrategyRowState>>
    fetch_strategy_row_states();
    void sync_strategy_row(int id, const StrategyRowState &state);
    void sync_removed_strategy(int id);
    void feed_strategy(const StrategyConfig &row,
                       const StrategyRowState &state);
    void report_strategy_pending(int id, const std::string &fingerprint,
                                 const std::string &message,
                                 const nlohmann::json &metadata, bool skip);
    void record_paper_mode(int id, bool is_paper);
    std::unique_ptr<Core::Strategy> build_strategy(
        const StrategyConfig &strategy, nlohmann::json &activated_md);
    void write_paper_account_records(
        const std::vector<StrategyConfig> &strategies);
    static std::filesystem::path paper_strategy_modes_path();
    // Operator commands for discretionary strategies; see LIVE_INFRA.md
    // "Strategy commands".
    std::shared_ptr<StrategyCommandStore> strategy_command_store;
    std::chrono::steady_clock::time_point last_strategy_command_flush_time{};
    void flush_strategy_command_reports();
    void resume_open_strategy_commands(const std::vector<int> &strategy_ids);
    // Accounts that paper strategies route to and no configured account backs;
    // their NAV is computed from those strategies' books.
    std::unordered_set<long long> paper_only_account_ids;
    std::unordered_set<unsigned long long> paper_orders_checked;
    // Lowest and highest trade price per exchange|market|symbol since the
    // last paper fill poll: a print through a resting limit fills it.
    std::mutex paper_trade_range_mutex;
    std::unordered_map<std::string, std::pair<double, double>> paper_trade_range;
    std::unordered_map<long long, std::unordered_set<std::string>>
        paper_reported_symbols;
    std::chrono::steady_clock::time_point last_paper_fills_poll_time{
        std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point last_paper_nav_poll_time{
        std::chrono::steady_clock::now() - std::chrono::seconds(60)};
    void poll_paper_fills();
    void poll_paper_nav();
    void poll_discretionary_accounts(bool force = false);
    std::vector<Core::DiscretionaryAccount> discretionary_accounts_;
    bool discretionary_accounts_active_ = false;
    std::chrono::steady_clock::time_point last_discretionary_accounts_poll_{};
    // False when a strategy switched between paper and live while it still
    // has orders or trades in the saved state.
    bool check_paper_mode_switches();

    void account_info_log_callback(const std::string &log);
    void account_error_log_callback(const std::string &log);

    void stream_info_log_callback(const std::string &log);
    void stream_error_log_callback(const std::string &log);

    void trading_log_callback(const std::vector<TradingLog> &logs);
    void engine_state_snapshot_callback(
        const Core::EngineStateSnapshot &snapshot);
    // Observability layer — UPSERTs the 4 state-backed tables
    // (engine_health, strategy_health, state_machine_health) and inserts
    // metric rows into engine_metric, all in one transaction per tick.
    // See agentic_system/skills/ENGINE_DIAGNOSTICS.md.
    void observability_callback(const Core::ObservabilityBatch &batch);
    void init_accounts();
    void create_account_db_records();
    void init_quote_assets();
    void init_strategies();
    void create_new_trade_ticker_entries();
    bool is_prevent_trading_lock_created();
    void create_prevent_trading_lock();
    void update_misc_orders();

    std::optional<Live::TradingState> read_trading_state_from_disk();
    void send_klines_to_engine(const std::string &symbol,
                               const std::string &exchange_name,
                               const std::string &market_type,
                               long long interval_ms);
    void subscribe_market_data_streams();
    void reconnect_streams();
    void live_event_consumer_loop();
    // Dedicated mostly-sleeping thread: writes the system-health watchdog file
    // and sends venue session heartbeats, gated on main-loop liveness.
    void liveness_heartbeat_loop();
    bool is_shutdown_file_found();
    void init_stall_orders_sqlite_cache();
    void init_inventory_mismatches_sqlite_cache();
    void init_nontracked_positions_close_times_cache();
    void init_borrowable_inventories_sqlite_cache();
    void init_caches();
    void update_account_entries_nav_to_db();
    void update_order_state(const Core::OrderResNormalized &order_res);
    void verify_order_states();
    void verify_polymarket_order_states();
    void validate_inventory_against_current_active_trades();
    void validate_paper_trades_against_db();
    void pause_strategy_for_mismatch(int strategy_id,
                                     const nlohmann::json &metadata);

    void close_nontracked_inventory(
        const std::vector<Core::NonTrackedInventoryToBeClosed>
            nontracked_inventory_to_be_closed);
    void poll_order_state_watchdog();
    void poll_inventory_state_watchdog();
    void poll_stall_orders_watchdog();
    void healthcheck_python_proxy();
    void check_core_dumps();
    void update_inventory_state();
    void analytics_infra_misc_db_updates();

    void poll_sync_state_to_disk();
    void poll_check_unacked_orders();
    void poll_check_failed_cancels();
    void poll_fetch_prices();
    void poll_exchange_info_update();
    void poll_borrowable_inventories();
    void poll_heartbeat_cache();
    void poll_ensure_minimum_gt_balance();
    void poll_periodic_validate_inventory();
    void poll_cancel_orphaned_orders();
    void poll_periodic_exchange_health_checks();
    void poll_periodic_update_misc_orders();
    void poll_symbol_exchange_info_to_db();
    void poll_orderbook_full_refresh();
    void init_polymarket_tickers();
    void run_polymarket_tasks();
    void run_prediction_market_mint_contracts();
    void run_prediction_market_redeem_contracts();
    void cancel_orders_if_near_shutdown();
    void create_state_updater_running_log();
    void repay_loans_with_available_funds();
    void close_nontracked_positions(
        const std::vector<Core::NonTrackedInventoryToBeClosed>
            nontracked_inventory_to_be_closed);
    void update_strategy_virtual_balances_to_db(bool force_update = false);
    void handle_order_create_failed_msg(const nlohmann::json &msg);
    void handle_order_cancel_failed_msg(const nlohmann::json &msg);
    void handle_fill_status_update_msg(const nlohmann::json &msg);
    void handle_user_data_stream_msg(const std::string &msg);
    void handle_binance_order_update_stream_msg(const nlohmann::json &msg,
                                                bool is_futures);
    void handle_ohlcv_stream_msg(const std::string &msg,
                                 Trading::ExchangeType exchange_type,
                                 Trading::MarketType market_type);
    void parse_binance_ohlcv_stream_msg(const std::string &msg,
                                        Trading::MarketType market_type);

    std::optional<long long> get_symbol_last_closed_trade_time(
        const std::string &symbol, const std::string &exchange_name,
        const std::string &market_type);
    Trading::Account *get_account(long long account_id);
    std::unordered_map<long long, Trading::Account> accounts;
    std::vector<std::unique_ptr<WsStream>> private_streams;
    std::unique_ptr<SqliteCache<long long>>
        symbol_last_closed_trade_times_cache;
    std::unique_ptr<SqliteCache<int>> unacked_orders_failed_to_retrieve_cache;
    std::unique_ptr<SqliteCache<long long>> stall_orders_sqlite_cache;
    std::unique_ptr<SqliteCache<long long>> inventory_mismatches_sqlite_cache;
    std::unique_ptr<SqliteCache<long long>>
        nontracked_positions_close_times_cache;
    std::unique_ptr<SqliteCache<std::string>>
        nontracked_positions_close_order_ids_cache;
    std::unique_ptr<SqliteCache<long long>> program_start_time_cache;
    std::unique_ptr<SqliteCache<long long>> heartbeat_cache;
    std::unique_ptr<SqliteCache<long long>> borrowable_inventories_sqlite_cache;
    std::vector<std::pair<std::string, long long>> pending_cancel_checks;
    std::mutex pending_cancel_checks_mutex;
    bool init_shutdown = false;

    std::chrono::steady_clock::time_point last_polymarket_events_poll_time{
        std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point last_mint_contracts_poll_time{
        std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point last_redeem_contracts_poll_time{
        std::chrono::steady_clock::now() - std::chrono::seconds(30)};
    std::chrono::steady_clock::time_point last_polymarket_public_sub_time_{
        std::chrono::steady_clock::now() - std::chrono::seconds(30)};
    std::vector<std::string> polymarket_series_slugs;

    std::mutex polymarket_metas_mutex;
    std::unordered_map<std::string, PolymarketEventMeta> polymarket_event_metas;

    // clob_id (asset_id) → tickers trading that token
    std::unordered_map<std::string,
                       std::vector<std::shared_ptr<Core::TradeTicker>>>
        polymarket_clob_tickers_;
    std::mutex polymarket_clob_tickers_mutex_;

    std::unique_ptr<ThreadPool> misc_task_thread_pool;
    std::unordered_set<std::string> polymarket_cancel_resolution_ids;
    std::mutex polymarket_cancel_resolution_ids_mutex;
    std::chrono::steady_clock::time_point last_cancel_resolution_ids_cleanup{
        std::chrono::steady_clock::now()};

    struct PolymarketDelayedTask {
        std::chrono::steady_clock::time_point due_time;
        std::function<void()> task;
    };
    std::vector<PolymarketDelayedTask> polymarket_delayed_tasks_;
    std::mutex polymarket_delayed_tasks_mutex_;

    void schedule_polymarket_delayed_task(std::chrono::milliseconds delay,
                                          std::function<void()> task);
    void process_polymarket_delayed_tasks();

    static constexpr auto POLYMARKET_CANCEL_RESOLUTION_DELAY_MS =
        std::chrono::milliseconds(1000);
};
}  // namespace Live
