#include <fcntl.h>
#include <unistd.h>
#include "market_making/discretionary_accounts.h"
#include "live_infra/state_updater.h"

#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <system_error>
#include <thread>

#include "binance_utils.h"
#include "cache_path_utils.h"
#include "ccxt_proxy.h"
#include "ccxt_utils.h"
#include "chrono_utils.h"
#include "common_types/constants.h"
#include "common_types/log_event_types.h"
#include "datetime_utils.h"
#include "disk_logging/log.h"
#include "disk_utils.h"  // needed on linux build
#include "file_lock.h"
#include "generic_utils.h"
#include "linux_process.h"
#include "live_infra/account_stream_manager.h"
#include "live_infra/config.h"
#include "live_infra/constants.h"
#include "live_infra/engine_signal_store.h"
#include "market_making/strategy_commands.h"
#include "live_infra/exchange_specific/aster.h"
#include "live_infra/exchange_specific/hyperliquid.h"
#include "live_infra/live_trade_ticker.h"
#include "live_infra/paper_trading.h"
#include "live_infra/types.h"
#include "market_data/alt_data_manager.h"
#include "market_data/bar_types_constant.h"
#include "market_data/market_data_streams.h"
#include "market_data/state_machine_dispatcher.h"
#include "market_data/stream_status.h"
#include "market_making/constants.h"
#ifndef DISABLE_VISUALIZATION
#include "realtime_dashboard/dashboard.h"
#endif
#include "market_making/dtypes/constants.h"
#include "market_making/dtypes/def_only_structs.h"
#include "market_making/dtypes/enums.h"
#include "market_making/dtypes/exchange_info.h"
#include "market_making/dtypes/global_general_cache.h"
#include "market_making/dtypes/inventory.h"
#include "market_making/dtypes/order.h"
#include "market_making/dtypes/order_res.h"
#include "market_making/dtypes/quote_asset.h"
#include "market_making/engine.h"
#include "market_making/utils.h"
#include "nats_publisher.h"
#include "postgre_utils.h"
#include "rate_limiter.h"
#include "sqlite_cache.h"
#include "trading/account.h"

std::mutex db_write_mutex;

static void update_service_health_entry(const std::string &service_name,
                                        const std::string &identifier,
                                        const std::string &status,
                                        const nlohmann::json &metadata) {
    auto &config = Live::Config::getInstance();

    nlohmann::json db_record = nlohmann::json::object();
    db_record["service_name"] = service_name;
    db_record["identifier"] = identifier;
    db_record["status"] = status;
    if (!metadata.empty()) {
        db_record["metadata"] = metadata;
    }

    std::vector<json> prev_records = PQUtils::fetch_from_postgres(
        config.get_db_connection_string(),
        ("SELECT * FROM service_health WHERE service_name = '" + service_name +
         "' AND identifier = '" + identifier + "'")
            .c_str());

    if (prev_records.empty()) {
        PQUtils::write_to_postgres(config.get_db_connection_string(),
                                   "service_health", {db_record});
    } else {
        PQUtils::update_entry_by_field(
            config.get_db_connection_string(), "service_health", db_record,
            "id", prev_records[0]["id"].get<std::string>());
    }
}

void Live::StateUpdater::trading_log_callback(
    const std::vector<TradingLog> &logs) {
    // if (!is_prod) {
    // for (const auto &log : logs) {
    //     std::cout << log.message << std::endl;
    // }
    // }
    auto &config = Live::Config::getInstance();
    long long program_start_time_ms = Core::get_program_start_time_ms();
    PQUtils::write_trading_logs(config.get_db_connection_string(), logs,
                                program_start_time_ms);
}

std::optional<long long> Live::StateUpdater::get_symbol_last_closed_trade_time(
    const std::string &symbol, const std::string &exchange_name,
    const std::string &market_type) {
    const std::string &key = Core::Utils::get_symbol_exchange_market_key(
        symbol, exchange_name, market_type);
    const auto &cache = this->symbol_last_closed_trade_times_cache->get(key);
    if (cache.has_value()) {
        return cache.value();
    }
    return std::nullopt;
}

void Live::StateUpdater::write_db_entries_from_engine_snapshot(
    Core::EngineStateSnapshot snapshot) {
    std::lock_guard<std::mutex> lock(db_write_mutex);
    auto &config = Live::Config::getInstance();
    snapshot.program_start_time_ms = Core::get_program_start_time_ms();
    PQUtils::write_engine_state_snapshot(config.get_db_connection_string(),
                                         snapshot);

    const auto &active_trades = snapshot.active_trades;
    const auto &new_completed_trades = snapshot.new_trades;

    const auto &existing_trades = PQUtils::fetch_from_postgres(
        config.get_db_connection_string(),
        "SELECT * FROM trade where is_closed_trade = false and "
        "strategy_version = 2;");

    std::unordered_map<unsigned long long, long long> existing_live_trade_ids;
    std::unordered_map<unsigned long long, nlohmann::json>
        existing_live_trade_db_json;
    for (const auto &trade : existing_trades) {
        try {
            unsigned long long unique_id;
            long long id;

            if (trade["unique_id"].is_string()) {
                unique_id = std::stoull(trade["unique_id"].get<std::string>());
            } else if (trade["unique_id"].is_number()) {
                unique_id = trade["unique_id"].get<unsigned long long>();
            } else {
                continue;
            }

            if (trade["id"].is_string()) {
                id = std::stoll(trade["id"].get<std::string>());
            } else if (trade["id"].is_number()) {
                id = trade["id"].get<long long>();
            } else {
                continue;
            }

            existing_live_trade_ids[unique_id] = id;
            existing_live_trade_db_json[unique_id] = trade;
        } catch (const std::exception &e) {
            continue;
        }
    }

    std::vector<nlohmann::json> trades_to_write;
    for (const auto &trade : new_completed_trades) {
        for (const auto &trade_item : trade.second) {
            if (existing_live_trade_ids.find(trade_item.id) !=
                existing_live_trade_ids.end()) {
                PQUtils::update_entry(
                    config.get_db_connection_string(), "trade",
                    trade_item.to_db_json(true, true, std::nullopt),
                    existing_live_trade_ids[trade_item.id]);
            } else {
                trades_to_write.push_back(
                    trade_item.to_db_json(true, true, std::nullopt));
            }

            const std::string key = Core::Utils::get_symbol_exchange_market_key(
                trade_item.symbol, trade_item.exchange_name,
                trade_item.market_type);
            this->symbol_last_closed_trade_times_cache->insert(
                key, trade_item.close_time_ms);
        }
    }

    for (const auto &trade_item : active_trades) {
        if (existing_live_trade_ids.find(trade_item.id) ==
            existing_live_trade_ids.end()) {
            trades_to_write.push_back(
                trade_item.to_db_json(true, false, std::nullopt));
        } else {
            const auto &db_json = existing_live_trade_db_json[trade_item.id];

            auto safe_get_double = [](const nlohmann::json &j,
                                      const std::string &key,
                                      double default_val) -> double {
                try {
                    if (!j.contains(key) || j[key].is_null()) {
                        return default_val;
                    }
                    if (j[key].is_string()) {
                        return std::stod(j[key].get<std::string>());
                    }
                    return j[key].get<double>();
                } catch (...) {
                    return default_val;
                }
            };

            double db_open_qty = safe_get_double(db_json, "open_quantity", 0.0);
            double db_open_price = safe_get_double(db_json, "open_price", 0.0);
            double db_closed_qty =
                safe_get_double(db_json, "closed_quantity", 0.0);
            double db_close_price =
                safe_get_double(db_json, "close_price", 0.0);

            double db_open_notional = db_open_qty * db_open_price;
            double db_closed_notional = db_closed_qty * db_close_price;

            double trade_open_notional = trade_item.open_quantity.toDouble() *
                                         trade_item.open_price.toDouble();
            double trade_closed_notional =
                trade_item.closed_quantity.toDouble() *
                trade_item.close_price.toDouble();

            double open_delta =
                std::abs(trade_open_notional - db_open_notional);
            double closed_delta =
                std::abs(trade_closed_notional - db_closed_notional);

            if (open_delta > 3 || closed_delta > 3) {
                PQUtils::update_entry(
                    config.get_db_connection_string(), "trade",
                    trade_item.to_db_json(true, false, std::nullopt),
                    existing_live_trade_ids[trade_item.id]);
            }
        }
    }
    PQUtils::write_to_postgres(config.get_db_connection_string(), "trade",
                               trades_to_write);

    nlohmann::json portfolio_tick_light;
    portfolio_tick_light["nav"] = snapshot.nav.toDouble();
    portfolio_tick_light["long_exposure"] = snapshot.long_exposure.toDouble();
    portfolio_tick_light["short_exposure"] = snapshot.short_exposure.toDouble();
    portfolio_tick_light["margin_used"] = snapshot.margin_notional.toDouble();
    portfolio_tick_light["timestamp_ms"] = snapshot.timestamp_ms;
    portfolio_tick_light["is_backtest"] = false;
    std::vector<nlohmann::json> portfolio_ticks;
    portfolio_ticks.push_back(portfolio_tick_light);
    PQUtils::write_to_postgres(config.get_db_connection_string(),
                               "portfolio_tick_light", portfolio_ticks);

    // Write order fills to dedicated table
    std::vector<nlohmann::json> order_fills_to_write;
    for (const auto &[key, fills] : snapshot.new_order_fills) {
        for (const auto &fill : fills) {
            order_fills_to_write.push_back(fill.to_db_json());
        }
    }
    if (!order_fills_to_write.empty()) {
        PQUtils::write_to_postgres(config.get_db_connection_string(),
                                   "order_fill", order_fills_to_write, false);
    }

    // Write completed markouts
    std::vector<nlohmann::json> markouts_to_write;
    for (const auto &markout : snapshot.completed_markouts) {
        markouts_to_write.push_back(markout.to_db_json());
    }
    if (!markouts_to_write.empty()) {
        PQUtils::write_to_postgres(config.get_db_connection_string(), "markout",
                                   markouts_to_write, false);
    }
}

// SQL literal escaping helpers for the observability UPSERT/INSERT paths.
// Declared here (above archive_snapshot_gzip_) so archive_snapshot_gzip_
// can use sql_str_ when building its INSERT statement. The observability
// UPSERTs use inline SQL because row payloads are small per batch and
// libpq's PQexec pattern matches the existing write_trading_logs path;
// prepared statements would add complexity without measurable benefit at
// this scale (~10 rows per 10s).
static std::string sql_escape_(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        if (c == '\'')
            out += "''";
        else
            out += c;
    }
    return out;
}

static std::string sql_jsonb_(const nlohmann::json &j) {
    if (j.is_null()) return "NULL";
    return "'" + sql_escape_(j.dump()) + "'::jsonb";
}

static std::string sql_str_(const std::string &s) {
    return "'" + sql_escape_(s) + "'";
}

static std::string sql_opt_i64_(const std::optional<int64_t> &v) {
    return v.has_value() ? std::to_string(*v) : std::string("NULL");
}

static std::string sql_opt_str_(const std::optional<std::string> &v) {
    return v.has_value() ? sql_str_(*v) : std::string("NULL");
}

// Gzip-compress a std::string → std::vector<Bytef> using zlib's compress2.
// Z_BEST_SPEED prioritizes throughput; compression ratio is adequate for
// JSON snapshots which have high intrinsic redundancy.
static std::vector<unsigned char> gzip_compress_(const std::string &raw) {
    uLongf dest_len = compressBound(static_cast<uLong>(raw.size()));
    std::vector<unsigned char> out(dest_len);
    int rc = compress2(out.data(), &dest_len,
                       reinterpret_cast<const Bytef *>(raw.data()),
                       static_cast<uLong>(raw.size()), Z_BEST_SPEED);
    if (rc != Z_OK) {
        out.clear();
        return out;
    }
    out.resize(dest_len);
    return out;
}

// Write snapshot to
// $DATA_DIR_ROOT/engine_state/{engine_instance}/YYYY/MM/DD/HH/snapshot__{ts}__{eli_hi}.json.gz
// and index it in engine_snapshot_index. Runs on the detached thread that
// invokes engine_state_snapshot_callback — never blocks the engine.
static void archive_snapshot_gzip_(const Core::EngineStateSnapshot &snapshot,
                                   const std::string &conninfo) {
    const char *data_root = std::getenv("DATA_DIR_ROOT");
    if (data_root == nullptr || snapshot.engine_instance.empty()) return;

    // Derive the hour-bucketed directory path.
    std::time_t t = static_cast<std::time_t>(snapshot.timestamp_ms / 1000);
    std::tm tm_utc;
    gmtime_r(&t, &tm_utc);
    char dir_buf[256];
    std::snprintf(dir_buf, sizeof(dir_buf),
                  "%s/engine_state/%s/%04d/%02d/%02d/%02d", data_root,
                  snapshot.engine_instance.c_str(), tm_utc.tm_year + 1900,
                  tm_utc.tm_mon + 1, tm_utc.tm_mday, tm_utc.tm_hour);
    std::error_code ec;
    std::filesystem::create_directories(dir_buf, ec);
    if (ec) {
        nlohmann::json md = {{"dir", dir_buf}, {"error", ec.message()}};
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_SNAPSHOT_ARCHIVE_MKDIR_FAILED,
                  "snapshot archive: mkdir failed", md);
        return;
    }

    // Serialize + compress.
    std::string raw = snapshot.to_json().dump();
    std::vector<unsigned char> compressed = gzip_compress_(raw);
    if (compressed.empty()) {
        nlohmann::json md = {{"raw_size", raw.size()}};
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_SNAPSHOT_ARCHIVE_GZIP_FAILED,
                  "snapshot archive: gzip compress failed", md);
        return;
    }

    char file_buf[512];
    std::snprintf(file_buf, sizeof(file_buf), "%s/snapshot__%lld__%lld.json.gz",
                  dir_buf, snapshot.timestamp_ms, snapshot.event_link_id_hi);
    std::string file_path(file_buf);

    // Collision guard: if file exists, append a .dup suffix.
    if (std::filesystem::exists(file_path)) {
        file_path += ".dup";
    }

    std::ofstream out(file_path, std::ios::binary);
    if (!out) {
        nlohmann::json md = {{"file_path", file_path}};
        LOG_ERROR(
            Core::TRADING_LOG_EVENT_TYPE_SNAPSHOT_ARCHIVE_FILE_OPEN_FAILED,
            "snapshot archive: file open failed", md);
        return;
    }
    out.write(reinterpret_cast<const char *>(compressed.data()),
              compressed.size());
    out.close();

    // Index the file.
    PGconn *conn = PQconnectdb(conninfo.c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        PQfinish(conn);
        return;
    }
    std::string sql =
        "INSERT INTO engine_snapshot_index (engine_instance, ts_ms, "
        "file_path, size_bytes, event_link_id_lo, event_link_id_hi, "
        "is_post_startup, retention_tier) VALUES (" +
        sql_str_(snapshot.engine_instance) + ", " +
        std::to_string(snapshot.timestamp_ms) + ", " + sql_str_(file_path) +
        ", " + std::to_string(compressed.size()) + ", " +
        (snapshot.event_link_id_lo < 0
             ? std::string("NULL")
             : std::to_string(snapshot.event_link_id_lo)) +
        ", " +
        (snapshot.event_link_id_hi < 0
             ? std::string("NULL")
             : std::to_string(snapshot.event_link_id_hi)) +
        ", FALSE, 1)";
    PGresult *res = PQexec(conn, sql.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        nlohmann::json md = {{"error", PQresultErrorMessage(res)}};
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_SNAPSHOT_INDEX_INSERT_FAILED,
                  "snapshot archive: index INSERT failed", md);
    }
    PQclear(res);
    PQfinish(conn);
}

void Live::StateUpdater::engine_state_snapshot_callback(
    const Core::EngineStateSnapshot &snapshot) {
    this->write_db_entries_from_engine_snapshot(snapshot);

    // Gzip-archive to disk if enabled. The DB write above is the existing
    // path and is unchanged. This is purely additive.
    const char *flag = std::getenv("ENGINE_SNAPSHOT_ARCHIVE_ENABLED");
    if (flag != nullptr && std::string(flag) == "1") {
        auto &config = Live::Config::getInstance();
        archive_snapshot_gzip_(snapshot, config.get_db_connection_string());
    }
}

void Live::StateUpdater::observability_callback(
    const Core::ObservabilityBatch &batch) {
#ifndef DISABLE_VISUALIZATION
    if (observability_feeds_dashboard_) {
        Charting::Dashboard::getInstance().set_observability_batch(batch);
    }
#endif
    if (!observability_writes_db_) return;

    auto &config = Live::Config::getInstance();
    const char *conninfo = config.get_db_connection_string();
    const auto start_wall = std::chrono::steady_clock::now();

    PGconn *conn = PQconnectdb(conninfo);
    if (PQstatus(conn) != CONNECTION_OK) {
        nlohmann::json md = {{"error", PQerrorMessage(conn)}};
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_DB_CONNECT_FAILED,
                  "observability_callback: PQconnectdb failed", md);
        PQfinish(conn);
        return;
    }

    // 5-second statement timeout so a slow DB doesn't wedge the periodic
    // thread pool. On timeout the transaction aborts; the next tick's
    // attempt will include fresh data.
    PQexec(conn, "SET statement_timeout = 5000");
    PQexec(conn, "BEGIN");

    auto exec = [&](const std::string &sql) -> bool {
        PGresult *res = PQexec(conn, sql.c_str());
        ExecStatusType st = PQresultStatus(res);
        bool ok = (st == PGRES_COMMAND_OK || st == PGRES_TUPLES_OK);
        if (!ok) {
            nlohmann::json md = {{"error", PQresultErrorMessage(res)},
                                 {"sql_prefix", sql.substr(0, 120)}};
            LOG_ERROR(
                Core::
                    TRADING_LOG_EVENT_TYPE_OBSERVABILITY_UPSERT_TRANSACTION_FAILED,
                "observability_callback: statement failed", md);
        }
        PQclear(res);
        return ok;
    };

    bool all_ok = true;

    // ---- engine_health UPSERT ----
    if (batch.engine.has_value() && !batch.engine_instance.empty()) {
        const auto &e = *batch.engine;
        std::string sql =
            "INSERT INTO engine_health (engine_instance, last_startup_ts, "
            "last_heartbeat_ts, is_running, market_events_queue_size, "
            "trading_logs_queue_size, mem_rss_bytes, total_market_events, "
            "last_shutdown_ts, last_shutdown_reason, build_commit, updated_at)"
            " VALUES (" +
            sql_str_(batch.engine_instance) + ", " +
            std::to_string(e.last_startup_ts_ms) + ", " +
            std::to_string(e.last_heartbeat_ts_ms) + ", " +
            (e.is_running ? "TRUE" : "FALSE") + ", " +
            std::to_string(e.market_events_queue_size) + ", " +
            std::to_string(e.trading_logs_queue_size) + ", " +
            std::to_string(e.mem_rss_bytes) + ", " +
            std::to_string(e.total_market_events) + ", " +
            sql_opt_i64_(e.last_shutdown_ts_ms) + ", " +
            sql_opt_str_(e.last_shutdown_reason) + ", " +
            sql_str_(e.build_commit) +
            ", NOW()) ON CONFLICT (engine_instance) DO UPDATE SET "
            "last_heartbeat_ts = EXCLUDED.last_heartbeat_ts, "
            "is_running = EXCLUDED.is_running, "
            "market_events_queue_size = EXCLUDED.market_events_queue_size, "
            "trading_logs_queue_size = EXCLUDED.trading_logs_queue_size, "
            "mem_rss_bytes = EXCLUDED.mem_rss_bytes, "
            "total_market_events = EXCLUDED.total_market_events, "
            "last_shutdown_ts = EXCLUDED.last_shutdown_ts, "
            "last_shutdown_reason = EXCLUDED.last_shutdown_reason, "
            "build_commit = EXCLUDED.build_commit, "
            "updated_at = NOW()";
        all_ok &= exec(sql);
    }

    // ---- strategy_health UPSERT per row ----
    for (const auto &s : batch.strategies) {
        std::string sql =
            "INSERT INTO strategy_health (strategy_id, engine_instance, "
            "last_heartbeat_ts, is_paused, open_trade_count, last_fill_ts, "
            "last_market_event_ts, inventory_summary, sm_dependencies, "
            "sm_health_summary, period_counts, last_prediction_ts, "
            "last_order_ts, updated_at) VALUES (" +
            std::to_string(s.strategy_id) + ", " +
            sql_str_(batch.engine_instance) + ", " +
            std::to_string(s.last_heartbeat_ts_ms) + ", " +
            (s.is_paused ? "TRUE" : "FALSE") + ", " +
            std::to_string(s.open_trade_count) + ", " +
            sql_opt_i64_(s.last_fill_ts_ms) + ", " +
            std::to_string(s.last_market_event_ts_ms) + ", " +
            sql_jsonb_(s.inventory_summary) + ", " +
            sql_jsonb_(s.sm_dependencies) + ", " +
            sql_jsonb_(s.sm_health_summary) + ", " +
            sql_jsonb_(s.period_counts) + ", " +
            sql_opt_i64_(s.last_prediction_ts_ms) + ", " +
            sql_opt_i64_(s.last_order_ts_ms) +
            ", NOW()) ON CONFLICT (strategy_id, engine_instance) DO UPDATE SET "
            "last_heartbeat_ts = EXCLUDED.last_heartbeat_ts, "
            "is_paused = EXCLUDED.is_paused, "
            "open_trade_count = EXCLUDED.open_trade_count, "
            "last_fill_ts = EXCLUDED.last_fill_ts, "
            "last_market_event_ts = EXCLUDED.last_market_event_ts, "
            "inventory_summary = EXCLUDED.inventory_summary, "
            "sm_dependencies = EXCLUDED.sm_dependencies, "
            "sm_health_summary = EXCLUDED.sm_health_summary, "
            "period_counts = EXCLUDED.period_counts, "
            "last_prediction_ts = COALESCE(EXCLUDED.last_prediction_ts, "
            "strategy_health.last_prediction_ts), "
            "last_order_ts = COALESCE(EXCLUDED.last_order_ts, "
            "strategy_health.last_order_ts), "
            "updated_at = NOW()";
        all_ok &= exec(sql);
    }

    // ---- state_machine_health UPSERT per row ----
    for (const auto &sm : batch.state_machines) {
        std::string sql =
            "INSERT INTO state_machine_health (sm_name, engine_instance, "
            "has_loaded_state, is_live_mode, is_fully_synced, sync_progress, "
            "min_synced_ts, latest_event_ts, dependencies, diagnostic_json, "
            "updated_at) VALUES (" +
            sql_str_(sm.sm_name) + ", " + sql_str_(batch.engine_instance) +
            ", " + (sm.has_loaded_state ? "TRUE" : "FALSE") + ", " +
            (sm.is_live_mode ? "TRUE" : "FALSE") + ", " +
            (sm.is_fully_synced ? "TRUE" : "FALSE") + ", " +
            sql_jsonb_(sm.sync_progress) + ", " +
            sql_opt_i64_(sm.min_synced_ts) + ", " +
            sql_opt_i64_(sm.latest_event_ts) + ", " +
            sql_jsonb_(sm.dependencies) + ", " +
            sql_jsonb_(sm.diagnostic_json) +
            ", NOW()) ON CONFLICT (sm_name, engine_instance) DO UPDATE SET "
            "has_loaded_state = EXCLUDED.has_loaded_state, "
            "is_live_mode = EXCLUDED.is_live_mode, "
            "is_fully_synced = EXCLUDED.is_fully_synced, "
            "sync_progress = EXCLUDED.sync_progress, "
            "min_synced_ts = EXCLUDED.min_synced_ts, "
            "latest_event_ts = EXCLUDED.latest_event_ts, "
            "dependencies = EXCLUDED.dependencies, "
            "diagnostic_json = EXCLUDED.diagnostic_json, "
            "updated_at = NOW()";
        all_ok &= exec(sql);
    }

    // ---- engine_metric INSERT per sample ----
    // Batched into one multi-row INSERT for efficiency when we have many
    // samples; single-row otherwise.
    if (!batch.metrics.empty()) {
        std::string sql =
            "INSERT INTO engine_metric (ts_ms, metric_name, value, "
            "strategy_id, symbol, labels, engine_instance) VALUES ";
        bool first = true;
        for (const auto &m : batch.metrics) {
            if (!first) sql += ", ";
            first = false;
            sql += "(";
            sql += std::to_string(m.ts_ms);
            sql += ", " + sql_str_(m.metric_name);
            sql += ", " + std::to_string(m.value);
            sql += ", " + (m.strategy_id.has_value()
                               ? std::to_string(*m.strategy_id)
                               : std::string("NULL"));
            sql += ", " + (m.symbol.has_value() ? sql_str_(*m.symbol)
                                                : std::string("NULL"));
            sql += ", " + sql_jsonb_(m.labels);
            sql += ", " + sql_str_(batch.engine_instance);
            sql += ")";
        }
        all_ok &= exec(sql);
    }

    if (all_ok) {
        PQexec(conn, "COMMIT");
    } else {
        PQexec(conn, "ROLLBACK");
    }
    PQfinish(conn);

    // Self-monitoring — captured after the transaction so it reflects
    // commit-inclusive wall clock. Emitted as a single log row rather than a
    // metric to avoid a feedback loop through engine_metric.
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_wall)
            .count();
    constexpr long long SLOW_THRESHOLD_MS = 2000;
    if (elapsed_ms > SLOW_THRESHOLD_MS) {
        // Over-threshold slow — informational; agents monitor it via the
        // observability.build_latency_ms metric on engine_metric.
        const size_t total_rows = batch.metrics.size() +
                                  batch.strategies.size() +
                                  batch.state_machines.size();
        nlohmann::json md = {
            {"elapsed_ms", elapsed_ms},
            {"threshold_ms", SLOW_THRESHOLD_MS},
            {"query_class", "observability_upsert_transaction"},
            {"target_tables",
             nlohmann::json::array(
                 {"engine_metric", "strategy_health", "state_machine_health"})},
            {"metrics_count", batch.metrics.size()},
            {"strategies_count", batch.strategies.size()},
            {"state_machines_count", batch.state_machines.size()},
            {"total_row_count", total_rows}};
        LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_OBSERVABILITY_UPSERT_SLOW,
                 "observability_callback slow", md);
    }
}

static void publish_trading_log_callback(const TradingLog &log) {
    NatsPublisher::getInstance().publish_trading_log(log);
}

static void publish_realtime_trading_state_callback(
    const Core::Engine &engine) {
    auto snapshot = Core::Engine::getInstance().build_realtime_snapshot();
    NatsPublisher::getInstance().publish("trading_state",
                                         snapshot.to_json().dump());
}

static void publish_strategy_telemetry_callback(const std::string &payload) {
    NatsPublisher::getInstance().publish("strategy_telemetry", payload);
}

// A pipeline rewrote one key's alt-data files; reload it where a strategy in
// this engine provisioned it.
static Core::EngineSignalOutcome apply_alt_data_changed_signal(
    const Core::EngineSignal &signal) {
    const auto source = MarketData::alt_data_source_from_string(
        signal.payload.value("source", std::string()));
    if (!source.has_value()) {
        return {Core::EngineSignalStatus::FAILED, "unknown alt-data source"};
    }
    if (signal.key.empty()) {
        return {Core::EngineSignalStatus::FAILED, "missing key"};
    }
    if (MarketData::AltDataManager::instance().refresh_keys(
            *source, {signal.key}) == 0) {
        return {Core::EngineSignalStatus::SKIPPED,
                "key not loaded in this engine, or its files are missing"};
    }
    return {Core::EngineSignalStatus::APPLIED, ""};
}

// An operator command for a discretionary strategy this engine holds. It is
// queued only after its status row is claimed, so a redelivery after a crash
// is dropped instead of run twice.
static Core::EngineSignalOutcome apply_strategy_command_signal(
    Live::StrategyCommandStore &store, const Core::EngineSignal &signal) {
    Core::StrategyCommand command;
    command.id = signal.id;
    command.payload = signal.payload;
    command.ts_ms = DatetimeUtils::get_current_time_ms();
    try {
        command.strategy_id = signal.payload.at("strategy_id").get<int>();
        command.op = signal.payload.at("op").get<std::string>();
    } catch (const std::exception &e) {
        return {Core::EngineSignalStatus::FAILED,
                std::string("bad payload: ") + e.what()};
    }
    if (command.op.empty()) {
        return {Core::EngineSignalStatus::FAILED, "missing op"};
    }

    std::optional<std::string> refusal;
    bool held = false;
    for (const auto &strategy : Core::Engine::getInstance().get_strategies()) {
        if (strategy->strategy_config.id != command.strategy_id) continue;
        held = true;
        if (!Core::accepts_strategy_commands(
                strategy->strategy_config.strategy_name)) {
            refusal = "strategy takes no commands";
        } else if (strategy->is_disabled_now()) {
            refusal = "strategy is disabled";
        }
        break;
    }
    if (!held) {
        return {Core::EngineSignalStatus::SKIPPED,
                "strategy not loaded in this engine"};
    }
    if (refusal) {
        store.claim(command.id, command.strategy_id, "failed", *refusal);
        return {Core::EngineSignalStatus::FAILED, *refusal};
    }
    if (!store.claim(command.id, command.strategy_id, "accepted", "")) {
        return {Core::EngineSignalStatus::SKIPPED, "command already claimed"};
    }
    Core::StrategyCommandInbox::instance().push(std::move(command));
    return {Core::EngineSignalStatus::APPLIED, ""};
}

Live::StateUpdater::StateUpdater() {
    program_start_time = std::chrono::steady_clock::now();
    is_prod = std::getenv("IS_PROD") != nullptr &&
              std::strcmp(std::getenv("IS_PROD"), "1") == 0;

    const std::string engine_instance = []() {
        const char *host = std::getenv("HOSTNAME");
        const char *inst = std::getenv("ENGINE_INSTANCE_NAME");
        return std::string(host ? host : "unknown") + ":" +
               std::string(inst ? inst : "default");
    }();
    // Acks are keyed by engine_instance, so engines reading the same rows
    // never hide one from another.
    auto signal_store = std::make_shared<Live::EngineSignalStore>(
        Live::Config::getInstance().get_db_connection_string(),
        engine_instance);
    auto fetch_engine_signals =
        [signal_store](const std::vector<std::string> &signal_types) {
            return signal_store->fetch_unacked(signal_types);
        };
    auto ack_engine_signal = [signal_store](
                                 const Core::EngineSignal &signal,
                                 const Core::EngineSignalOutcome &outcome) {
        signal_store->ack(signal, outcome);
    };
    Core::Engine::getInstance().register_engine_signal_handler(
        Core::ENGINE_SIGNAL_ALT_DATA_CHANGED, apply_alt_data_changed_signal);
    this->strategy_command_store = std::make_shared<Live::StrategyCommandStore>(
        Live::Config::getInstance().get_db_connection_string());
    Core::StrategyCommandInbox::instance().set_reporting_enabled(true);
    Core::Engine::getInstance().register_engine_signal_handler(
        Core::ENGINE_SIGNAL_STRATEGY_COMMAND,
        [store = this->strategy_command_store](const Core::EngineSignal &signal) {
            return apply_strategy_command_signal(*store, signal);
        });

    auto fetch_realtime_subscriptions =
        []() -> std::vector<Core::RealtimeDataSubscription> {
        auto &config = Live::Config::getInstance();
        std::vector<Core::RealtimeDataSubscription> result;
        auto records = PQUtils::fetch_from_postgres(
            config.get_db_connection_string(),
            "SELECT * FROM realtime_data_subscription WHERE is_active = true");
        for (const auto &record : records) {
            Core::RealtimeDataSubscription sub;
            sub.id = std::stoi(record["id"].get<std::string>());
            sub.data_type = record["data_type"].get<std::string>();
            sub.symbol = record["symbol"].get<std::string>();
            sub.exchange_name = record["exchange_name"].get<std::string>();
            sub.market_type = record["market_type"].get<std::string>();
            sub.is_active = true;
            result.push_back(sub);
        }
        return result;
    };

    LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_STARTUP_PHASE,
             "Pre-loading exchange info before engine init", nlohmann::json{});
    initialize_exchange_info();
    LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_EXCHANGE_INFO_PRELOADED,
             "Exchange info pre-loaded", nlohmann::json{});

    const auto env_is_one = [](const char *name) {
        const char *value = std::getenv(name);
        return value != nullptr && std::string(value) == "1";
    };
    observability_writes_db_ = env_is_one("ENGINE_OBSERVABILITY_ENABLED");
#ifndef DISABLE_VISUALIZATION
    observability_feeds_dashboard_ = env_is_one("ENABLE_DASHBOARD");
#endif

    auto &engine = Core::Engine::getInstance();
    engine.initialize(
        [this](const std::vector<TradingLog> &logs) {
            this->trading_log_callback(logs);
        },
        [this](const Core::EngineStateSnapshot &snapshot) {
            this->engine_state_snapshot_callback(snapshot);
        },
        Core::EngineConfig{
            .is_backtest = false,
            .is_strategy_development = is_prod ? false : false,
            .emit_trading_logs = true,
            .emit_logs_in_real_time = true,
            .publish_trading_log_callback = publish_trading_log_callback,
            .emit_realtime_trading_state = true,
            .publish_realtime_trading_state_callback =
                publish_realtime_trading_state_callback,
            .seconds_between_realtime_trading_state_updates = 1,
            .fees_by_exchange_and_market =
                {
                    {std::string(Core::EXCHANGE_BINANCE) + ":" +
                         std::string(Core::SWAP_MARKET_TYPE),
                     Core::FeeModel{.taker_fee_rate = 0.0005,
                                    .maker_fee_rate = 0.0002}},
                    {std::string(Core::EXCHANGE_BINANCE) + ":" +
                         std::string(Core::SPOT_MARKET_TYPE),
                     Core::FeeModel{.taker_fee_rate = 0.001,
                                    .maker_fee_rate = 0.00075}},
                    {std::string(Core::EXCHANGE_BYBIT) + ":" +
                         std::string(Core::SPOT_MARKET_TYPE),
                     Core::FeeModel{.taker_fee_rate = 0.001,
                                    .maker_fee_rate = 0.00075}},
                    {std::string(Core::EXCHANGE_BITGET) + ":" +
                         std::string(Core::SPOT_MARKET_TYPE),
                     Core::FeeModel{.taker_fee_rate = 0.001,
                                    .maker_fee_rate = 0.00075}},
                    {std::string(Core::EXCHANGE_GATEIO) + ":" +
                         std::string(Core::SPOT_MARKET_TYPE),
                     Core::FeeModel{.taker_fee_rate = 0.001,
                                    .maker_fee_rate = 0.00075}},
                },
            .enable_engine_signals = true,
            .fetch_engine_signals_callback = fetch_engine_signals,
            .ack_engine_signal_callback = ack_engine_signal,
            .enable_realtime_subscriptions = true,
            .fetch_realtime_subscriptions_callback =
                fetch_realtime_subscriptions,
            .seconds_between_subscription_polls = 10,
            .is_order_creation_disabled =
                std::getenv("IS_ORDER_CREATION_DISABLED") != nullptr &&
                std::string(std::getenv("IS_ORDER_CREATION_DISABLED")) == "1",
            .enable_strategy_telemetry = true,
            .publish_strategy_telemetry_callback =
                publish_strategy_telemetry_callback,
            .engine_instance = engine_instance,
            .observability_callback =
                (observability_writes_db_ || observability_feeds_dashboard_)
                    ? std::make_optional<Core::ObservabilityCallback>(
                          [this](const Core::ObservabilityBatch &batch) {
                              this->observability_callback(batch);
                          })
                    : std::nullopt});
    Core::Engine::getInstance().initialize_symbol_precision_access();

    // Phase 3 of logging overhaul — route every typed LOG_INFO/LOG_WARNING/
    // LOG_ERROR call in this process (from live_infra, common, market_data,
    // state_machines, etc.) into the engine's structured sink so agents can
    // query them from trading_activity_log. See
    // agentic_system/skills/LOGGING.md. _DISK_ONLY variants skip this sink.
    DiskLogging::set_external_sink([](const char *level, const char *event_type,
                                      const std::string &msg,
                                      const nlohmann::json &metadata) {
        auto &eng = Core::Engine::getInstance();
        if (!eng.isInitialized()) return;
        eng.create_system_log("", level, msg, event_type, metadata, -1, "", "",
                              /*is_trading_activity_log=*/true);
    });

    RateLimiter::instance().set_on_limit_callback(
        [&engine](const std::string &key, int retry_after_s) {
            auto separator = key.find(':');
            std::string exchange_name = key.substr(0, separator);
            std::string market_type =
                separator == std::string::npos ? "" : key.substr(separator + 1);

            nlohmann::json metadata;
            metadata["cooldown_s"] = retry_after_s;
            metadata["http_status"] = 429;

            engine.create_system_log(
                "", Core::LOG_LEVEL_WARNING, key + " rate limited (HTTP 429)",
                "rate_limit", metadata, -1, exchange_name, market_type);
        });

    this->bars_realtime = std::make_unique<MarketData::MarketDataStreamManager>(
        [this](const std::string &message) {
            Core::Engine::getInstance().create_system_log(
                "", Core::LOG_LEVEL_ERROR, message,
                Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_STREAM_PARSE_ERROR,
                nlohmann::json::object(), -1, "", "");
        },
        [this](const std::string &message) {
            Core::Engine::getInstance().create_system_log(
                "", Core::LOG_LEVEL_INFO, message,
                Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_INIT_PHASE,
                nlohmann::json::object(), -1, "", "");
        },
        ChronoUtils::make_timepoint(2025, 1, 1, 0, 0, 0), true);

    MarketData::MarketDataStreams::get_instance().set_live_event_callback(
        [this](MarketData::MarketEvent &&event) {
            this->live_event_queue_.enqueue(std::move(event));
        });

    LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_STARTUP_PHASE,
             "Starting bars livestreamer", nlohmann::json{});
    std::thread bars_livestreamer_thread(
        [this]() { this->bars_realtime->start_stream(); });
    bars_livestreamer_thread.detach();

    std::thread live_event_thread(
        [this]() { this->live_event_consumer_loop(); });
    live_event_thread.detach();

    std::thread liveness_thread([this]() { this->liveness_heartbeat_loop(); });
    liveness_thread.detach();

    LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_STARTUP_PHASE,
             "Finished reading datasets", nlohmann::json{});

    LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_STARTUP_PHASE,
             "Initializing accounts", nlohmann::json{});
    init_accounts();
    LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_ACCOUNT_INIT_SUCCESS,
             "Initialized accounts", nlohmann::json{});

    LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_STARTUP_PHASE,
             "Initializing polymarket direct signing", nlohmann::json{});
    CcxtProxy::instance().init_polymarket_direct_signing();
    if (CcxtProxy::instance().is_polymarket_direct_signing_enabled()) {
        LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_STARTUP_PHASE,
                 "Polymarket direct signing ENABLED", nlohmann::json{});
    }

    if (Core::Engine::getInstance().is_order_creation_disabled()) {
        LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_STARTUP_PHASE,
                 "ORDER CREATION DISABLED (IS_ORDER_CREATION_DISABLED=1)",
                 nlohmann::json{});
    }

    Live::AsterExchange::init_and_register_account_streams(
        [this](const Core::OrderResNormalized &order_res) {
            this->update_order_state(order_res);
        });

    // Gate all Hyperliquid startup work (credential fetch, meta load, WS subs)
    // on a configured HL account. With none, the venue stays fully inert — no
    // outbound calls to hyperliquid.xyz.
    if (!Live::Config::getInstance().hyperliquid_futures_accounts.empty()) {
        Live::HyperliquidExchange::init_and_register_account_streams(
            [this](const Core::OrderResNormalized &order_res) {
                this->update_order_state(order_res);
            });
    }

    LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_STARTUP_PHASE,
             "Initializing quote assets", nlohmann::json{});
    init_quote_assets();
    LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_STARTUP_PHASE,
             "Initialized quote assets", nlohmann::json{});

    LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_STARTUP_PHASE,
             "Initializing strategies", nlohmann::json{});
    init_strategies();
    LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_STARTUP_PHASE,
             "Initialized strategies", nlohmann::json{});

    LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_STARTUP_PHASE,
             "Subscribing to market data streams", nlohmann::json{});
    subscribe_market_data_streams();
    LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_STARTUP_PHASE,
             "Subscribed to market data streams", nlohmann::json{});
    init_caches();
    create_account_db_records();

    misc_task_thread_pool = std::make_unique<ThreadPool>(12);

#ifndef DISABLE_VISUALIZATION
    Charting::Dashboard::getInstance().set_private_stream_status_provider(
        [this]() { return this->get_private_stream_statuses(); });
#endif
}

std::vector<MarketData::StreamStatus>
Live::StateUpdater::get_private_stream_statuses() {
    auto statuses =
        Live::AccountStreamManager::getInstance().get_stream_statuses();
    const auto now = std::chrono::steady_clock::now();
    const long long now_ms = DatetimeUtils::get_current_time_ms();
    for (size_t i = 0; i < private_streams.size(); ++i) {
        auto &stream = private_streams[i];
        MarketData::StreamStatus s;
        s.key = "private:ccxt_proxy:user_data:" + std::to_string(i);
        s.is_private = true;
        s.stream_type = "user_data";
        s.exchange_name = "ccxt_proxy";
        s.host = MarketData::StreamStatusUtils::url_host(stream->wsurl_);
        // is_disconnected() waits on ws_mutex, held by connect() through a
        // thread join; the atomics never block.
        s.is_connected =
            stream->is_initialized_.load() && !stream->is_disconnected_.load();
        s.last_msg_age_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - stream->get_last_msg_received())
                .count();
        s.msg_count = stream->get_msg_count();
        s.sampled_at_ms = now_ms;
        statuses.push_back(std::move(s));
    }
    return statuses;
}

void Live::StateUpdater::account_info_log_callback(const std::string &log) {
    LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_STARTUP_PHASE, log,
             nlohmann::json{});
    Core::Engine::getInstance().create_system_log(
        "", Core::LOG_LEVEL_INFO, log,
        Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_STARTUP_PHASE,
        nlohmann::json::object(), -1, "", "");
}

void Live::StateUpdater::account_error_log_callback(const std::string &log) {
    LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_ACCOUNT_ERROR, log,
              nlohmann::json{});
    Core::Engine::getInstance().create_system_log(
        "", Core::LOG_LEVEL_ERROR, log,
        Core::TRADING_LOG_EVENT_TYPE_ACCOUNT_ERROR, nlohmann::json::object(),
        -1, "", "");
}

std::unique_ptr<Core::Strategy> Live::StateUpdater::build_strategy(
    const StrategyConfig &source, nlohmann::json &activated_md) {
    auto strategy = source;
    if (strategy.discretionary_multi_account && !this->discretionary_accounts_active_) {
        this->discretionary_accounts_active_ = true;
        this->poll_discretionary_accounts(true);
    }
    if ((strategy.strategy_name == "crypto-discretionary-paper" && !strategy.is_paper_trading) ||
        (strategy.strategy_name == "crypto-discretionary-realmoney" && strategy.is_paper_trading)) {
        throw std::runtime_error("Discretionary executor name conflicts with execution mode");
    }
    if (strategy.discretionary_multi_account && (strategy.strategy_name == "crypto-discretionary-paper" || strategy.strategy_name == "crypto-discretionary-realmoney")) {
        strategy.exchanges_list.clear();
        for (const auto& account : this->discretionary_accounts_) {
            if (account.paper == strategy.is_paper_trading) {
                strategy.exchanges_list.push_back({account.exchange, account.market, account.id});
            }
        }
    }
    if (strategy.is_paper_trading &&
        strategy.is_trading_on_prediction_market) {
        Core::Engine::getInstance().create_system_log(
            "", Core::LOG_LEVEL_ERROR,
            "paper trading does not support prediction-market strategies; "
            "strategy not loaded",
            Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_STARTUP_PHASE,
            nlohmann::json::object(), strategy.id);
        return nullptr;
    }
    // Paper trades counted on a real account read as untracked inventory
    // there, and reconciliation acts on the real account.
    for (const auto &exchange : strategy.exchanges_list) {
        if (strategy.is_paper_trading && exchange.account_id.has_value() &&
            this->accounts.contains(*exchange.account_id)) {
            throw std::runtime_error(
                "paper strategy uses real account " +
                std::to_string(*exchange.account_id) +
                "; paper strategies need a paper-only account id");
        }
    }
    if (strategy.is_paper_trading) {
        {
            std::unique_lock lock(this->paper_strategy_ids_mutex);
            this->paper_strategy_ids.insert(strategy.id);
        }
        for (const auto &exchange : strategy.exchanges_list) {
            if (exchange.account_id.has_value()) {
                this->paper_only_account_ids.insert(*exchange.account_id);
            }
        }
    }

    auto strategy_ptr = std::make_unique<Core::Strategy>(strategy);

    // Engine-owned once loaded; a rebuilt strategy keeps the running value.
    this->strategy_virtual_balances.try_emplace(strategy.id,
                                                strategy.virtual_balance);

    std::vector<std::string> initialized_symbols;
    nlohmann::json failed_symbols_and_reasons = nlohmann::json::array();

    if (!strategy.is_trading_on_prediction_market) {
        for (const auto &trade_ticker : strategy.trade_tickers) {
            std::vector<Core::ExchangeAndMarket> exchanges_and_markets;
            for (const auto &exchange : strategy.exchanges_list) {
                if (exchange.exchange_name == "polygon") {
                    if (Core::ExchangeInfo::getInstance().is_symbol_active(
                            trade_ticker.symbol, exchange.exchange_name,
                            exchange.market_type)) {
                        exchanges_and_markets.push_back(exchange);
                    }
                    continue;
                }
                bool matched_account = false;
                for (auto &[key, account] : this->accounts) {
                    account.get_active_symbols();
                    if (account.exchange_name == exchange.exchange_name &&
                        account.market_type == exchange.market_type &&
                        account.symbol_exists(trade_ticker.symbol)) {
                        matched_account = true;
                        exchanges_and_markets.push_back(
                            Core::ExchangeAndMarket{
                                .exchange_name = exchange.exchange_name,
                                .market_type = exchange.market_type,
                                .account_id = exchange.account_id});
                    }
                }
                // A paper strategy needs no account, only the venue's
                // public symbol info, which init_accounts loads only for
                // configured accounts.
                if (!matched_account && strategy.is_paper_trading) {
                    const auto venue = exchange.exchange_name + ":" +
                                       exchange.market_type;
                    // An empty load (proxy still warming, no disk cache)
                    // is not cached, so the next build tries again.
                    if (!this->paper_exchange_infos_loaded.contains(venue)) {
                        const auto infos =
                            Core::ExchangeInfo::getInstance()
                                .get_exchange_symbols_info(
                                    exchange.exchange_name,
                                    exchange.market_type);
                        if (!infos.empty()) {
                            Core::Engine::getInstance().add_symbol_exchange_info(
                                exchange.exchange_name, exchange.market_type,
                                infos);
                            this->paper_exchange_infos_loaded.insert(venue);
                        }
                    }
                    if (!this->paper_exchange_infos_loaded.contains(venue) &&
                        !strategy.is_disabled) {
                        throw SymbolInfoUnavailable("no symbol info for " +
                                                    venue);
                    }
                    if (Core::ExchangeInfo::getInstance().is_symbol_active(
                            trade_ticker.symbol, exchange.exchange_name,
                            exchange.market_type)) {
                        exchanges_and_markets.push_back(exchange);
                    }
                }
            }

            if (exchanges_and_markets.empty()) {
                failed_symbols_and_reasons.push_back(
                    {{"symbol", trade_ticker.symbol},
                     {"reason", "NO_MATCHING_EXCHANGE_OR_MARKET"}});
                continue;
            }

            Live::OrderFrequencyLimits order_frequency_limits;
            order_frequency_limits.num_max_orders_10_sec =
                strategy.num_max_orders_10_sec;
            order_frequency_limits.num_max_orders_1_min =
                strategy.num_max_orders_1_min;
            order_frequency_limits.num_max_orders_1_hour =
                strategy.num_max_orders_1_hour;

            auto ticker_ptr = std::make_shared<Live::LiveTradeTicker>(
                exchanges_and_markets, trade_ticker.symbol,
                trade_ticker.quote_asset, trade_ticker.base_asset,
                strategy.id, *this, order_frequency_limits);
            ticker_ptr->configure_from_strategy(strategy);
            strategy_ptr->add_trade_ticker(trade_ticker.symbol,
                                           std::move(ticker_ptr));
            initialized_symbols.push_back(trade_ticker.symbol);
        }
    }

    // A disabled row or one with no venue legitimately builds empty.
    if (!strategy.is_trading_on_prediction_market && !strategy.is_disabled &&
        !strategy.exchanges_list.empty() && !strategy.trade_tickers.empty() &&
        initialized_symbols.empty()) {
        throw SymbolInfoUnavailable(
            "none of " + std::to_string(strategy.trade_tickers.size()) +
            " tickers matched a venue");
    }

    const long long activated_at_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    nlohmann::json strategy_flags = {
        {"is_market_making_strategy", strategy.is_market_making_strategy},
        {"is_paper_trading", strategy.is_paper_trading},
        {"is_trading_on_prediction_market",
         strategy.is_trading_on_prediction_market},
        {"is_backtest", false},
    };
    activated_md = {
        {"strategy_flags", strategy_flags},
        {"symbols", initialized_symbols},
        {"tickers_initialized",
         static_cast<int>(initialized_symbols.size())},
        {"tickers_failed",
         static_cast<int>(failed_symbols_and_reasons.size())},
        {"failed_symbols_and_reasons", failed_symbols_and_reasons},
        {"dynamic_tickers", strategy.is_trading_on_prediction_market},
        {"strategy_name", strategy.strategy_name},
        {"activated_at_ms", activated_at_ms},
    };
    return strategy_ptr;
}

void Live::StateUpdater::init_strategies() {
    auto &config = Live::Config::getInstance();
    // Read before the rows: a change in between is re-applied by the sync.
    const auto row_states =
        this->fetch_strategy_row_states().value_or(
            std::unordered_map<int, StrategyRowState>{});
    const auto &strategies = PQUtils::fetch_strategies(
        config.get_db_connection_string(), "2.0");
    LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_STARTUP_PHASE,
             "Fetched " + std::to_string(strategies.size()) +
                 " strategies from database",
             nlohmann::json{});

    std::set<std::string> series_slugs_set;

    for (const auto &strategy : strategies) {
        nlohmann::json activated_md;
        std::unique_ptr<Core::Strategy> strategy_ptr;
        try {
            strategy_ptr = this->build_strategy(strategy, activated_md);
        } catch (const std::exception &e) {
            // Left unfed so the row sync retries it; its restored trades are
            // held meanwhile.
            Core::Engine::getInstance().create_system_log(
                "", Core::LOG_LEVEL_ERROR,
                std::string("strategy not loaded: ") + e.what(),
                Core::TRADING_LOG_EVENT_TYPE_HUMAN_INTERVENTION_REQUIRED,
                nlohmann::json::object(), strategy.id);
            continue;
        }
        const auto state = row_states.find(strategy.id);
        this->feed_strategy(strategy, state != row_states.end()
                                          ? state->second
                                          : StrategyRowState{});
        if (!strategy_ptr) continue;

        for (const auto &exchange : strategy.exchanges_list) {
            if (exchange.exchange_name != "polymarket") continue;
            if (!strategy.series_slugs.empty()) {
                series_slugs_set.insert(strategy.series_slugs.begin(),
                                        strategy.series_slugs.end());
            } else if (!strategy.series_slug.empty()) {
                series_slugs_set.insert(strategy.series_slug);
            }
        }

        Core::Engine::getInstance().create_system_log(
            "", Core::LOG_LEVEL_INFO, "strategy activated",
            Core::TRADING_LOG_EVENT_TYPE_STRATEGY_ACTIVATED, activated_md,
            strategy.id);
        Core::Engine::getInstance().add_strategy(std::move(strategy_ptr));
    }

    std::vector<int> strategy_ids;
    for (const auto &strategy : strategies) strategy_ids.push_back(strategy.id);
    this->resume_open_strategy_commands(strategy_ids);

    this->polymarket_series_slugs.assign(series_slugs_set.begin(),
                                         series_slugs_set.end());
    if (!this->polymarket_series_slugs.empty()) {
        LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_STARTUP_PHASE,
                 "Polymarket series slugs configured: " +
                     std::to_string(this->polymarket_series_slugs.size()),
                 nlohmann::json{});
        init_polymarket_tickers();
    }
}

void Live::StateUpdater::resume_open_strategy_commands(const std::vector<int>& strategy_ids) {
    try {
        auto commands = this->strategy_command_store->open_commands(strategy_ids);
        if (!this->discretionary_accounts_active_) {
            for (auto& command : commands) Core::StrategyCommandInbox::instance().push(std::move(command));
            return;
        }
        std::set<long long> settled;
        if (const auto state = this->read_trading_state_from_disk()) {
            for (const auto& [id, value] : state->discretionary_settlements.items()) {
                settled.insert(value.value("signal_id",0LL));
            }
        }
        std::string references = "0";
        for (const auto& command : commands) references += "," + std::to_string(command.payload.value("signal_id",command.id));
        const auto query = "SELECT signal_id::text FROM discretionary_paper_settlement WHERE signal_id IN (" + references + ") UNION ALL SELECT '-1'";
        const auto rows = PQUtils::fetch_from_postgres(Live::Config::getInstance().get_db_connection_string(),query.c_str());
        for (const auto& row : rows) settled.insert(std::stoll(row["signal_id"].get<std::string>()));
        for (auto& command : commands) {
            bool account_accounting = false;
            for (const auto& strategy : this->fed_strategy_rows()) {
                if (strategy.id==command.strategy_id) account_accounting = strategy.discretionary_multi_account && strategy.is_paper_trading;
            }
            if (account_accounting && rows.empty()) throw std::runtime_error("Cannot verify settled commands before recovery");
            const auto reference = command.payload.value("signal_id",command.id);
            if (account_accounting && settled.contains(reference)) {
                Core::StrategyCommandInbox::instance().report({command.id,command.strategy_id,Core::STRATEGY_COMMAND_DONE,"trade already settled"});
            } else {
                Core::StrategyCommandInbox::instance().push(std::move(command));
            }
        }
    } catch (const std::exception& e) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_STRATEGY_COMMAND_STATUS_FAILED,
                  "Failed to resume open strategy commands",(nlohmann::json{{"error",e.what()}}));
    }
}

void Live::StateUpdater::flush_strategy_command_reports() {
    const auto now = std::chrono::steady_clock::now();
    if (now - this->last_strategy_command_flush_time < std::chrono::seconds(1)) {
        return;
    }
    this->last_strategy_command_flush_time = now;
    auto &inbox = Core::StrategyCommandInbox::instance();
    auto books = inbox.take_changed_books();
    if (!books.empty()) {
        try {
            this->strategy_command_store->save_books(books);
        } catch (const std::exception &e) {
            LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_STRATEGY_COMMAND_STATUS_FAILED,
                      "Failed to write strategy books",
                      (nlohmann::json{{"error", e.what()}, {"books", books.size()}}));
            inbox.mark_books_unsaved(books);
        }
    }
    // Links first, so a command's final status never shows before its orders.
    auto links = inbox.take_order_links();
    if (!links.empty()) {
        try {
            this->strategy_command_store->link_orders(links);
        } catch (const std::exception &e) {
            LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_STRATEGY_COMMAND_STATUS_FAILED,
                      "Failed to write strategy command orders",
                      (nlohmann::json{{"error", e.what()}, {"orders", links.size()}}));
            inbox.restore_order_links(std::move(links));
            return;
        }
    }
    auto reports = inbox.take_reports();
    if (reports.empty()) return;
    try {
        this->strategy_command_store->apply(reports);
    } catch (const std::exception &e) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_STRATEGY_COMMAND_STATUS_FAILED,
                  "Failed to write strategy command statuses",
                  (nlohmann::json{{"error", e.what()},
                                  {"reports", reports.size()}}));
        inbox.restore_reports(std::move(reports));
    }
}

void Live::StateUpdater::init_accounts() {
    const auto &config = Live::Config::getInstance();
    Core::Engine::getInstance().create_system_log(
        "", Core::LOG_LEVEL_INFO, "Initializing accounts",
        Core::TRADING_LOG_EVENT_TYPE_ACCOUNT_INIT_SUCCESS,
        nlohmann::json::object(), -1, "", "");
    int num_accounts = 0;
    for (const auto &[account_id, access_and_secret] :
         config.binance_futures_accounts) {
        num_accounts++;
        Trading::Account account(
            Trading::MarketType::SWAP_MARKET_TYPE,
            Trading::ExchangeType::BINANCE_ACCOUNT, account_id,
            access_and_secret.access, access_and_secret.secret,
            [this](const std::string &log) {
                this->account_info_log_callback(log);
            },
            [this](const std::string &log) {
                this->account_error_log_callback(log);
            });

        const auto &exchange_info =
            Core::ExchangeInfo::getInstance().get_exchange_symbols_info(
                "binance", "swap");
        Core::Engine::getInstance().add_symbol_exchange_info("binance", "swap",
                                                             exchange_info);

        accounts.insert({account_id, std::move(account)});
    }
    for (const auto &[account_id, access_and_secret] :
         config.binance_spot_accounts) {
        num_accounts++;
        Trading::Account account(
            Trading::MarketType::SPOT_MARKET_TYPE,
            Trading::ExchangeType::BINANCE_ACCOUNT, account_id,
            access_and_secret.access, access_and_secret.secret,
            [this](const std::string &log) {
                this->account_info_log_callback(log);
            },
            [this](const std::string &log) {
                this->account_error_log_callback(log);
            });

        const auto &exchange_info =
            Core::ExchangeInfo::getInstance().get_exchange_symbols_info(
                "binance", "spot");
        Core::Engine::getInstance().add_symbol_exchange_info("binance", "spot",
                                                             exchange_info);

        accounts.insert({account_id, std::move(account)});
    }
    for (const auto &[account_id, access_and_secret] :
         config.bybit_futures_accounts) {
        num_accounts++;
        Trading::Account account(
            Trading::MarketType::SWAP_MARKET_TYPE,
            Trading::ExchangeType::BYBIT_ACCOUNT, account_id,
            access_and_secret.access, access_and_secret.secret,
            [this](const std::string &log) {
                this->account_info_log_callback(log);
            },
            [this](const std::string &log) {
                this->account_error_log_callback(log);
            });

        const auto &exchange_info =
            Core::ExchangeInfo::getInstance().get_exchange_symbols_info("bybit",
                                                                        "swap");
        Core::Engine::getInstance().add_symbol_exchange_info("bybit", "swap",
                                                             exchange_info);
        accounts.insert({account_id, std::move(account)});
    }

    for (const auto &[account_id, access_and_secret] :
         config.bybit_spot_accounts) {
        num_accounts++;
        Trading::Account account(
            Trading::MarketType::SPOT_MARKET_TYPE,
            Trading::ExchangeType::BYBIT_ACCOUNT, account_id,
            access_and_secret.access, access_and_secret.secret,
            [this](const std::string &log) {
                this->account_info_log_callback(log);
            },
            [this](const std::string &log) {
                this->account_error_log_callback(log);
            });

        const auto &exchange_info =
            Core::ExchangeInfo::getInstance().get_exchange_symbols_info("bybit",
                                                                        "spot");

        Core::Engine::getInstance().add_symbol_exchange_info("bybit", "spot",
                                                             exchange_info);
        accounts.insert({account_id, std::move(account)});
    }

    for (const auto &[account_id, access_and_secret] :
         config.bitget_futures_accounts) {
        num_accounts++;
        Trading::Account account(
            Trading::MarketType::SWAP_MARKET_TYPE,
            Trading::ExchangeType::BITGET_ACCOUNT, account_id,
            access_and_secret.access, access_and_secret.secret,
            [this](const std::string &log) {
                this->account_info_log_callback(log);
            },
            [this](const std::string &log) {
                this->account_error_log_callback(log);
            });

        const auto &exchange_info =
            Core::ExchangeInfo::getInstance().get_exchange_symbols_info(
                "bitget", "swap");
        Core::Engine::getInstance().add_symbol_exchange_info("bitget", "swap",
                                                             exchange_info);
        accounts.insert({account_id, std::move(account)});
    }

    for (const auto &[account_id, access_and_secret] :
         config.bitget_spot_accounts) {
        num_accounts++;
        Trading::Account account(
            Trading::MarketType::SPOT_MARKET_TYPE,
            Trading::ExchangeType::BITGET_ACCOUNT, account_id,
            access_and_secret.access, access_and_secret.secret,
            [this](const std::string &log) {
                this->account_info_log_callback(log);
            },
            [this](const std::string &log) {
                this->account_error_log_callback(log);
            });

        const auto &exchange_info =
            Core::ExchangeInfo::getInstance().get_exchange_symbols_info(
                "bitget", "spot");

        Core::Engine::getInstance().add_symbol_exchange_info("bitget", "spot",
                                                             exchange_info);
        accounts.insert({account_id, std::move(account)});
    }

    for (const auto &[account_id, access_and_secret] :
         config.gateio_spot_accounts) {
        num_accounts++;
        Trading::Account account(
            Trading::MarketType::SPOT_MARKET_TYPE,
            Trading::ExchangeType::GATEIO_ACCOUNT, account_id,
            access_and_secret.access, access_and_secret.secret,
            [this](const std::string &log) {
                this->account_info_log_callback(log);
            },
            [this](const std::string &log) {
                this->account_error_log_callback(log);
            });

        const auto &exchange_info =
            Core::ExchangeInfo::getInstance().get_exchange_symbols_info(
                "gateio", "spot");

        Core::Engine::getInstance().add_symbol_exchange_info("gateio", "spot",
                                                             exchange_info);
        accounts.insert({account_id, std::move(account)});
    }

    for (const auto &[account_id, access_and_secret] :
         config.polymarket_accounts) {
        num_accounts++;
        Trading::Account account(
            Trading::MarketType::SPOT_MARKET_TYPE,
            Trading::ExchangeType::POLYMARKET_ACCOUNT, account_id,
            access_and_secret.access, access_and_secret.secret,
            [this](const std::string &log) {
                this->account_info_log_callback(log);
            },
            [this](const std::string &log) {
                this->account_error_log_callback(log);
            });
        accounts.insert({account_id, std::move(account)});
    }

    for (const auto &[account_id, access_and_secret] :
         config.aster_futures_accounts) {
        num_accounts++;
        Trading::Account account(
            Trading::MarketType::SWAP_MARKET_TYPE,
            Trading::ExchangeType::ASTER_ACCOUNT, account_id,
            access_and_secret.access, access_and_secret.secret,
            [this](const std::string &log) {
                this->account_info_log_callback(log);
            },
            [this](const std::string &log) {
                this->account_error_log_callback(log);
            });

        const auto &exchange_info =
            Core::ExchangeInfo::getInstance().get_exchange_symbols_info("aster",
                                                                        "swap");
        Core::Engine::getInstance().add_symbol_exchange_info("aster", "swap",
                                                             exchange_info);
        accounts.insert({account_id, std::move(account)});
    }

    for (const auto &[account_id, access_and_secret] :
         config.hyperliquid_futures_accounts) {
        num_accounts++;
        Trading::Account account(
            Trading::MarketType::SWAP_MARKET_TYPE,
            Trading::ExchangeType::HYPERLIQUID_ACCOUNT, account_id,
            access_and_secret.access, access_and_secret.secret,
            [this](const std::string &log) {
                this->account_info_log_callback(log);
            },
            [this](const std::string &log) {
                this->account_error_log_callback(log);
            });

        const auto &exchange_info =
            Core::ExchangeInfo::getInstance().get_exchange_symbols_info(
                "hyperliquid", "swap");
        Core::Engine::getInstance().add_symbol_exchange_info(
            "hyperliquid", "swap", exchange_info);
        accounts.insert({account_id, std::move(account)});
    }

    for (const auto &[account_id, access_and_secret] :
         config.ibkr_spot_accounts) {
        num_accounts++;
        Trading::Account account(
            Trading::MarketType::SPOT_MARKET_TYPE,
            Trading::ExchangeType::IBKR_ACCOUNT, account_id,
            access_and_secret.access, access_and_secret.secret,
            [this](const std::string &log) {
                this->account_info_log_callback(log);
            },
            [this](const std::string &log) {
                this->account_error_log_callback(log);
            });

        const auto &exchange_info =
            Core::ExchangeInfo::getInstance().get_exchange_symbols_info("ibkr",
                                                                        "spot");
        Core::Engine::getInstance().add_symbol_exchange_info("ibkr", "spot",
                                                             exchange_info);
        accounts.insert({account_id, std::move(account)});
    }

    nlohmann::json metadata = nlohmann::json::object();
    Core::Engine::getInstance().create_system_log(
        "", Core::LOG_LEVEL_INFO,
        "Initialized " + std::to_string(num_accounts) + " accounts",
        Core::TRADING_LOG_EVENT_TYPE_ACCOUNT_INIT_SUCCESS, metadata, -1);
}

void Live::StateUpdater::init_quote_assets() {
    const auto &config = Live::Config::getInstance();
    Core::Engine::getInstance().create_system_log(
        "", Core::LOG_LEVEL_INFO, "Initializing quote assets",
        Core::TRADING_LOG_EVENT_TYPE_ACCOUNT_INIT_SUCCESS,
        nlohmann::json::object(), -1);

    for (auto &[account_id, account] : accounts) {
        const auto &inventory_state = account.get_inventory_state();
        if (inventory_state.has_value()) {
            for (const auto &quote_asset : inventory_state.value().first) {
                Core::Engine::getInstance().update_quote_asset(
                    account.exchange_name, account.market_type,
                    quote_asset.asset, account_id, quote_asset);
            }
            for (const auto &inventory : inventory_state.value().second) {
                Core::Engine::getInstance().update_inventory(
                    inventory.exchange_name, inventory.market_type,
                    inventory.asset, account_id, inventory);
            }
        }
    }
}

Live::StateUpdater::~StateUpdater() {
#ifndef DISABLE_VISUALIZATION
    Charting::Dashboard::getInstance().set_private_stream_status_provider(
        nullptr);
#endif
}

std::optional<nlohmann::json> Live::StateUpdater::create_order(
    const Core::Order &order) {
    auto it = accounts.find(order.account_id);
    if (it != accounts.end()) {
        return it->second.create_order(order);
    }
    return std::nullopt;
}

std::vector<PolymarketDirect::PolymarketClient::SignedOrderBody>
Live::StateUpdater::sign_polymarket_orders(
    const std::vector<Core::Order> &orders) {
    if (orders.empty()) {
        return {};
    }
    auto it = accounts.find(orders.front().account_id);
    if (it != accounts.end()) {
        return it->second.sign_polymarket_orders(orders);
    }
    return {};
}

std::vector<Core::PolymarketBulkOrderResult>
Live::StateUpdater::submit_signed_polymarket_orders(
    const std::vector<PolymarketDirect::PolymarketClient::SignedOrderBody>
        &signed_orders,
    long long account_id) {
    auto it = accounts.find(account_id);
    if (it != accounts.end()) {
        return it->second.submit_signed_polymarket_orders(signed_orders,
                                                          account_id);
    }
    return std::vector<Core::PolymarketBulkOrderResult>(signed_orders.size());
}

Core::PolymarketBulkOrderResult
Live::StateUpdater::submit_signed_polymarket_order(
    const PolymarketDirect::PolymarketClient::SignedOrderBody &signed_order,
    long long account_id) {
    auto it = accounts.find(account_id);
    if (it != accounts.end()) {
        return it->second.submit_signed_polymarket_order(signed_order,
                                                         account_id);
    }
    Core::PolymarketBulkOrderResult result;
    result.error_msg = "account not found";
    return result;
}

std::vector<Core::PolymarketBulkOrderResult>
Live::StateUpdater::create_polymarket_orders_bulk(
    const std::vector<Core::Order> &orders) {
    if (orders.empty()) {
        return {};
    }
    auto it = accounts.find(orders.front().account_id);
    if (it != accounts.end()) {
        return it->second.create_polymarket_orders_bulk(orders);
    }
    return std::vector<Core::PolymarketBulkOrderResult>(orders.size());
}

bool Live::StateUpdater::is_polymarket_direct_signing_enabled() const {
    return CcxtProxy::instance().is_polymarket_direct_signing_enabled();
}

bool Live::StateUpdater::is_paper_strategy(int strategy_id) const {
    std::shared_lock lock(this->paper_strategy_ids_mutex);
    return paper_strategy_ids.contains(strategy_id);
}

bool Live::StateUpdater::is_aster_direct_signing_enabled() const {
    return CcxtProxy::instance().is_aster_direct_signing_enabled();
}

std::vector<Core::PolymarketBulkOrderResult>
Live::StateUpdater::create_aster_orders_bulk(
    const std::vector<Core::Order> &orders) {
    if (orders.empty()) return {};
    auto it = accounts.find(orders.front().account_id);
    if (it != accounts.end()) {
        return it->second.create_aster_orders_bulk(orders);
    }
    return std::vector<Core::PolymarketBulkOrderResult>(orders.size());
}

std::vector<Core::PolymarketBulkCancelResult>
Live::StateUpdater::cancel_aster_orders_bulk(
    const std::vector<std::string> &order_ids, long long account_id,
    const std::string &symbol) {
    auto it = accounts.find(account_id);
    if (it != accounts.end()) {
        return it->second.cancel_aster_orders_bulk(order_ids, symbol);
    }
    return std::vector<Core::PolymarketBulkCancelResult>(order_ids.size());
}

bool Live::StateUpdater::is_hyperliquid_direct_signing_enabled() const {
    return CcxtProxy::instance().is_hyperliquid_direct_signing_enabled();
}

std::vector<Core::PolymarketBulkOrderResult>
Live::StateUpdater::create_hyperliquid_orders_bulk(
    const std::vector<Core::Order> &orders) {
    if (orders.empty()) return {};
    auto it = accounts.find(orders.front().account_id);
    if (it != accounts.end()) {
        return it->second.create_hyperliquid_orders_bulk(orders);
    }
    return std::vector<Core::PolymarketBulkOrderResult>(orders.size());
}

std::vector<Core::PolymarketBulkCancelResult>
Live::StateUpdater::cancel_hyperliquid_orders_bulk(
    const std::vector<std::string> &order_ids, long long account_id,
    const std::string &symbol) {
    auto it = accounts.find(account_id);
    if (it != accounts.end()) {
        return it->second.cancel_hyperliquid_orders_bulk(order_ids, symbol);
    }
    return std::vector<Core::PolymarketBulkCancelResult>(order_ids.size());
}

std::optional<nlohmann::json> Live::StateUpdater::cancel_order(
    const Core::Order &order) {
    auto it = accounts.find(order.account_id);
    if (it != accounts.end()) {
        return it->second.cancel_order(order);
    }
    return std::nullopt;
}

std::vector<Core::PolymarketBulkCancelResult>
Live::StateUpdater::cancel_polymarket_orders_bulk(
    const std::vector<std::string> &order_ids, long long account_id) {
    auto it = accounts.find(account_id);
    if (it != accounts.end()) {
        return it->second.cancel_polymarket_orders_bulk(order_ids);
    }
    return std::vector<Core::PolymarketBulkCancelResult>(order_ids.size());
}

void Live::StateUpdater::cancel_order_by_client_order_id(
    const std::string &client_order_id, const std::string &exchange_name,
    const std::string &market_type, const std::string &symbol,
    const long long &account_id) {
    auto it = accounts.find(account_id);
    if (it != accounts.end()) {
        it->second.cancel_order_by_client_order_id(
            client_order_id, exchange_name, market_type, symbol, account_id);
    }
}

void Live::StateUpdater::on_order_fill(Core::OrderFillInfo &fill) {
    Core::Engine::getInstance().order_filled_ack(fill);
}

std::optional<Live::TradingState>
Live::StateUpdater::read_trading_state_from_disk() {
    std::string path = CommonUtils::CachePathUtils::get_trading_state_path();
    CommonUtils::CachePathUtils::migrate_cache_file_if_needed(
        "trading_state.json", path);
    std::ifstream file(path);
    if (!file.is_open()) {
        return std::nullopt;
    }
    try {
        nlohmann::json j = nlohmann::json::parse(file);
        return j.get<Live::TradingState>();
    } catch (const nlohmann::json::exception &e) {
        Core::Engine::getInstance().create_system_log(
            "", Core::LOG_LEVEL_ERROR,
            "Failed to parse trading_state.json: " + std::string(e.what()),
            Core::TRADING_LOG_EVENT_TYPE_TRADING_STATE_PARSE_FAILED,
            nlohmann::json{{"phase", "parse_trading_state_json"},
                           {"file_path", path},
                           {"what", e.what()}},
            -1);
        return std::nullopt;
    }
}

bool Live::StateUpdater::sync_state_to_disk() {
    Live::TradingState trading_state;
    // Track which stage failed so the catch-block emit carries useful context
    // instead of the ambiguous "something broke". Each transition point updates
    // `phase` so whichever phase throws is captured.
    const char *phase = "init";
    try {
        phase = "engine_reads";
        auto &engine = Core::Engine::getInstance();
        trading_state.orders = engine.get_orders();
        trading_state.trades = engine.get_trades();
        for (auto &order : engine.get_held_orders()) {
            trading_state.orders.push_back(std::move(order));
        }
        for (auto &trade : engine.get_held_trades()) {
            trading_state.trades.push_back(std::move(trade));
        }
        trading_state.new_trades = engine.get_new_trades();
        trading_state.new_orders = engine.get_new_orders();
        trading_state.new_orders_cancels = engine.get_new_orders_cancels();
        trading_state.new_order_fills = engine.get_new_order_fills();
        trading_state.tracked_misc_orders = engine.get_tracked_misc_orders();
        trading_state.discretionary_settlements = Core::DiscretionaryAccounts::instance().pending();
        // A close racing the snapshot is checkpointed on the next pass, never beside its earlier open state.
        for (const auto& trade : trading_state.trades) trading_state.discretionary_settlements.erase(std::to_string(trade.id));

        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<> dis(100000, 999999);

        const std::string final_file =
            CommonUtils::CachePathUtils::get_trading_state_path();
        const std::string temp_file =
            final_file + "_" +
            std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()) +
            "_" + std::to_string(dis(gen)) + ".tmp";

        phase = "write_tempfile";
        std::ofstream file(temp_file);
        nlohmann::json j = trading_state;
        file << j.dump(4);
        file.flush();
        if (!file) throw std::runtime_error("Failed to write trading state checkpoint");
        file.close();
        if (!trading_state.discretionary_settlements.empty()) {
            const int fd = ::open(temp_file.c_str(),O_RDONLY);
            if (fd<0) throw std::runtime_error("Cannot open settlement checkpoint for fsync");
            const int result = ::fsync(fd);
            ::close(fd);
            if (result!=0) throw std::runtime_error("Cannot fsync settlement checkpoint");
        }

        phase = "read_back_verify";
        const auto trading_state_from_disk =
            this->read_trading_state_from_disk();
        phase = "rename_final";
        std::filesystem::rename(temp_file, final_file);
        if (!trading_state.discretionary_settlements.empty()) {
            const auto directory = std::filesystem::path(final_file).parent_path();
            const int fd = ::open(directory.c_str(),O_RDONLY|O_DIRECTORY);
            if (fd<0) throw std::runtime_error("Cannot open checkpoint directory for fsync");
            const int result = ::fsync(fd);
            ::close(fd);
            if (result!=0) throw std::runtime_error("Cannot fsync checkpoint directory");
        }
        if (trading_state_from_disk.has_value()) {
            if (trading_state_from_disk.value() != trading_state) {
                // Counts only: the full state is already in engine_state_snapshot.
                const nlohmann::json metadata = {
                    {"trades_count", trading_state.trades.size()},
                    {"orders_count", trading_state.orders.size()}};
                Core::Engine::getInstance().create_system_log(
                    "", Core::LOG_LEVEL_INFO,
                    "Synced new trading_state to disk",
                    Core::TRADING_LOG_EVENT_TYPE_TRADING_STATE_SYNC, metadata,
                    -1);
            }
        }
        return true;
    } catch (const std::exception &e) {
        // Pin `snap_before` so operators can see in-memory engine state at
        // the moment sync failed. 5-min throttle bypassed — this is an ERROR
        // path with known-bounded volume.
        Core::Engine::getInstance().emit_engine_state_snapshot(true);
        Core::Engine::getInstance().create_system_log(
            "", Core::LOG_LEVEL_ERROR, "state sync failed",
            Core::TRADING_LOG_EVENT_TYPE_TRADING_STATE_SYNC_FAILED,
            nlohmann::json{
                {"phase", phase},
                {"what", e.what()},
                {"orders_count", trading_state.orders.size()},
                {"trades_count", trading_state.trades.size()},
                {"new_orders_count", trading_state.new_orders.size()},
                {"snapshot_before_flushed", true},
            },
            -1);
    }
    return false;
}

void Live::StateUpdater::recover_state_from_disk() {
    const auto &trading_state = this->read_trading_state_from_disk();
    if (!trading_state.has_value()) {
        Core::Engine::getInstance().create_system_log(
            "", Core::LOG_LEVEL_ERROR,
            "Failed to recover state from disk: file not found",
            Core::TRADING_LOG_EVENT_TYPE_TRADING_STATE_RECOVER_FAILED,
            nlohmann::json::object(), -1);
        return;
    }
    Core::DiscretionaryAccounts::instance().restore(trading_state->discretionary_settlements);
    Core::Engine::getInstance().recover_state_from_disk(
        trading_state.value().trades, trading_state.value().orders);
    Core::Engine::getInstance().set_unsynced_analytics_data(
        trading_state.value().new_orders, trading_state.value().new_trades,
        trading_state.value().new_orders_cancels,
        trading_state.value().new_order_fills);
    Core::Engine::getInstance().set_tracked_misc_orders(
        trading_state.value().tracked_misc_orders);
}

void Live::StateUpdater::graceful_shutdown() {
    this->init_shutdown = true;
    Core::Engine::getInstance().create_system_log(
        "", Core::LOG_LEVEL_INFO, "Initiating graceful shutdown",
        Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_SHUTDOWN_REQUESTED,
        nlohmann::json::object(), -1);
    std::this_thread::sleep_for(std::chrono::seconds(
        3));  // allow some time for all queues to empty for
              // sure, 3 seconds should be more than plenty
    Core::Engine::getInstance().sync_persistent_state_to_disk();
    sync_state_to_disk();
    update_strategy_virtual_balances_to_db(true);
}

Trading::Account *Live::StateUpdater::get_account(long long account_id) {
    auto it = accounts.find(account_id);
    if (it != accounts.end()) {
        return &it->second;
    }
    return nullptr;
}

void Live::StateUpdater::handle_order_state_check(
    Core::Order &order,
    const Core::OrderResNormalized &order_res_from_exchange) {
    if (!order.is_order_creation_acknowledged) {
        Core::Engine::getInstance().order_created_ack(order);
    }

    if (order_res_from_exchange.filled_quantity > order.filled_quantity) {
        auto fill_info = order.from_order_res_normalized(
            Core::Engine::getInstance(), order_res_from_exchange);
        this->on_order_fill(fill_info);
        return;
    }

    if (order_res_from_exchange.status == Core::ORDER_STATUS_NOT_WORKING) {
        Core::Engine::getInstance().order_canceled_ack(order);
    }
}

void Live::StateUpdater::verify_order_states() {
    const auto orders = Core::Engine::getInstance().get_orders();

    for (auto order : orders) {
        if (order.is_prediction_market_order ||
            is_paper_strategy(order.strategy_id)) {
            continue;
        }

        auto *account = get_account(order.account_id);

        if (account != nullptr) {
            if (order.is_alt_market_routed &&
                !order.route_order_to_alt_symbol.has_value()) {
                Core::Engine::getInstance().create_system_log(
                    order.symbol, Core::LOG_LEVEL_INFO,
                    "Order is alt market routed but no alt symbol is set",
                    Core::TRADING_LOG_EVENT_TYPE_ORDER_STATE_VERIFY,
                    nlohmann::json::object(), order.strategy_id);
                continue;
            }

            const auto &orders_from_exchange =
                account->get_orders_on_symbol_cached(
                    order.get_order_api_symbol());

            if (orders_from_exchange.size() == 0) {
                Core::Engine::getInstance().create_system_log(
                    order.symbol, Core::LOG_LEVEL_INFO,
                    "Exchange returned no orders on symbol: " + order.symbol,
                    Core::TRADING_LOG_EVENT_TYPE_ORDER_STATE_VERIFY,
                    nlohmann::json::object(), order.strategy_id);
            }

            bool found_order = false;
            const std::string &order_api_symbol = order.get_order_api_symbol();
            for (const auto &order_res : orders_from_exchange) {
                if (Core::Utils::is_same_order_adaptive(
                        order, order_res.client_order_id,
                        order_res.exchange_order_id)) {
                    if (order_res.symbol != order_api_symbol) {
                        nlohmann::json metadata = nlohmann::json::object();
                        metadata["order_symbol"] = order.symbol;
                        metadata["order_api_symbol"] = order_api_symbol;
                        metadata["order_res_symbol"] = order_res.symbol;
                        metadata["client_order_id"] = order.client_order_id;
                        Core::Engine::getInstance().create_system_log(
                            order.symbol, Core::LOG_LEVEL_ERROR,
                            "Symbol mismatch in verify_order_states: "
                            "expected " +
                                order_api_symbol + ", got " + order_res.symbol,
                            Core::
                                TRADING_LOG_EVENT_TYPE_ORDER_STATE_VERIFY_MISMATCH,
                            metadata, order.strategy_id, order.exchange_name,
                            order.market_type);
                        continue;
                    }
                    found_order = true;
                    this->handle_order_state_check(order, order_res);
                }
            }

            if (!found_order) {
                Core::Engine::getInstance().create_system_log(
                    order.symbol, Core::LOG_LEVEL_INFO,
                    "Querying individual order because it was not found by "
                    "bulk fetch",
                    Core::TRADING_LOG_EVENT_TYPE_ORDER_STATE_VERIFY,
                    nlohmann::json::object(), order.strategy_id);
                if (order.client_order_id.empty() &&
                    order.exchange_order_id.empty()) {
                    continue;
                }
                const auto &invidual_order =
                    account->query_invidual_order(order);
                if (invidual_order.has_value()) {
                    Core::Engine::getInstance().create_system_log(
                        order.symbol, Core::LOG_LEVEL_INFO,
                        "Found individual order on symbol: " + order.symbol,
                        Core::TRADING_LOG_EVENT_TYPE_ORDER_STATE_VERIFY,
                        nlohmann::json::object(), order.strategy_id);
                    std::cout
                        << "Found individual order on symbol: " << order.symbol
                        << " for strategy " << order.strategy_id << std::endl;
                    this->handle_order_state_check(order,
                                                   invidual_order.value());
                } else {
                    const auto age_ms = DatetimeUtils::get_current_time_ms() -
                                        order.create_timestamp;
                    const auto min_age_ms =
                        order.is_order_creation_acknowledged ? 60000 * 3 : 5000;
                    if (age_ms < min_age_ms) {
                        continue;
                    }
                    std::cout
                        << "Handling individual order not found for strategy "
                        << order.strategy_id << " and symbol " << order.symbol
                        << std::endl;
                    this->handle_invidual_order_not_found(order);
                }
            }
        }
    }
}

// ── Orphaned order canceller for exchanges with global open orders ──────

static std::atomic<long long> s_last_orphan_cancel_time_ms{0};
static std::atomic<bool> s_orphan_cancel_running{false};

static void cancel_orphaned_exchange_orders(Live::StateUpdater &updater) {
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count();
    if (now_ms - s_last_orphan_cancel_time_ms.load(std::memory_order_relaxed) <
        5000)
        return;
    s_last_orphan_cancel_time_ms.store(now_ms, std::memory_order_relaxed);

    if (s_orphan_cancel_running.exchange(true)) return;

    auto *updater_ptr = &updater;
    std::thread([updater_ptr]() {
        try {
            auto &engine = Core::Engine::getInstance();

            for (const auto &[account_id, account] :
                 updater_ptr->get_accounts()) {
                auto ex = Core::exchange_from_str(account.exchange_name);
                if (!Core::exchange_supports_global_open_orders(ex)) continue;

                auto exchange_orders =
                    CcxtProxy::instance().get_all_open_orders(
                        account.exchange_name, account.market_type, account_id);

                std::unordered_set<std::string> engine_ids;
                for (const auto &o : engine.get_orders()) {
                    if (o.account_id != account_id) continue;
                    if (o.exchange_order_id.empty()) continue;
                    engine_ids.insert(o.exchange_order_id);
                }

                std::unordered_map<std::string, std::vector<std::string>>
                    orphaned_by_symbol;
                for (const auto &eo : exchange_orders) {
                    if (engine_ids.count(eo.exchange_order_id)) continue;
                    orphaned_by_symbol[eo.symbol].push_back(
                        eo.exchange_order_id);
                }

                for (auto &[symbol, ids] : orphaned_by_symbol) {
                    LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_MISC_ORDER_CLEANUP,
                             "[OrphanCancel] Cancelling " +
                                 std::to_string(ids.size()) +
                                 " orphaned orders on " +
                                 account.exchange_name + " symbol=" + symbol +
                                 " account=" + std::to_string(account_id),
                             nlohmann::json{});
                    if (ex == Core::Exchange::ASTER) {
                        updater_ptr->cancel_aster_orders_bulk(ids, account_id,
                                                              symbol);
                    } else if (ex == Core::Exchange::HYPERLIQUID) {
                        updater_ptr->cancel_hyperliquid_orders_bulk(
                            ids, account_id, symbol);
                    }
                }
            }
        } catch (const std::exception &e) {
            LOG_ERROR(
                Core::TRADING_LOG_EVENT_TYPE_MISC_ORDER_CLEANUP,
                std::string("cancel_orphaned_exchange_orders exception: ") +
                    e.what(),
                nlohmann::json{});
        } catch (...) {
            LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_MISC_ORDER_CLEANUP,
                      "cancel_orphaned_exchange_orders unknown exception",
                      nlohmann::json{});
        }
        s_orphan_cancel_running.store(false);
    }).detach();
}

void Live::StateUpdater::poll_order_state_watchdog() {
    const auto now_tp = std::chrono::steady_clock::now();
    if (now_tp - last_order_state_poll_time < std::chrono::seconds(3)) {
        return;
    }

    last_order_state_poll_time = now_tp;

    // CEX order verification
    if (!verify_order_states_running.load()) {
        verify_order_states_running.store(true);
        std::thread([this]() {
            try {
                verify_order_states();
            } catch (const std::exception &e) {
                Core::Engine::getInstance().create_system_log(
                    "", Core::LOG_LEVEL_ERROR,
                    std::string("verify_order_states exception: ") + e.what(),
                    Core::TRADING_LOG_EVENT_TYPE_ORDER_STATE_VERIFY,
                    nlohmann::json::object(), -1, "", "");
            } catch (...) {
                Core::Engine::getInstance().create_system_log(
                    "", Core::LOG_LEVEL_ERROR,
                    "verify_order_states unknown exception",
                    Core::TRADING_LOG_EVENT_TYPE_ORDER_STATE_VERIFY,
                    nlohmann::json::object(), -1, "", "");
            }
            verify_order_states_running.store(false);
        }).detach();
    }

    // Orphaned order canceller (exchanges with global open orders fetch)
    cancel_orphaned_exchange_orders(*this);

    // Polymarket order verification (separate thread)
    if (!verify_polymarket_order_states_running.load()) {
        verify_polymarket_order_states_running.store(true);
        std::thread([this]() {
            try {
                verify_polymarket_order_states();
            } catch (const std::exception &e) {
                LOG_ERROR(
                    Core::TRADING_LOG_EVENT_TYPE_POLYMARKET_ORDER_STATE_VERIFY,
                    std::string("verify_polymarket_order_states exception: ") +
                        e.what(),
                    nlohmann::json{});
            } catch (...) {
                LOG_ERROR(
                    Core::TRADING_LOG_EVENT_TYPE_POLYMARKET_ORDER_STATE_VERIFY,
                    "verify_polymarket_order_states unknown exception",
                    nlohmann::json{});
            }
            verify_polymarket_order_states_running.store(false);
        }).detach();
    }
}

void Live::StateUpdater::update_inventory_state() {
    for (auto &[account_id, account] : accounts) {
        const auto &inventory_state = account.get_inventory_state();
        if (inventory_state.has_value()) {
            Core::Engine::getInstance().update_inventory_and_quote_assets(
                account.exchange_name, account.market_type, account_id,
                inventory_state.value().second, inventory_state.value().first);
        }
    }
}

void Live::StateUpdater::poll_inventory_state_watchdog() {
    if (inventory_state_update_running.load()) {
        return;
    }
    if (std::chrono::steady_clock::now() - last_inventory_state_poll_time >
        std::chrono::seconds(3)) {
        last_inventory_state_poll_time = std::chrono::steady_clock::now();
        inventory_state_update_running.store(true);

        std::thread([this]() {
            try {
                this->update_inventory_state();
            } catch (const std::exception &e) {
                Core::Engine::getInstance().create_system_log(
                    "", Core::LOG_LEVEL_ERROR,
                    std::string("update_inventory_state exception: ") +
                        e.what(),
                    Core::TRADING_LOG_EVENT_TYPE_INVENTORY_STATE_UPDATE_FAILED,
                    nlohmann::json::object(), -1, "", "");
            } catch (...) {
                Core::Engine::getInstance().create_system_log(
                    "", Core::LOG_LEVEL_ERROR,
                    "update_inventory_state unknown exception",
                    Core::TRADING_LOG_EVENT_TYPE_INVENTORY_STATE_UPDATE_FAILED,
                    nlohmann::json::object(), -1, "", "");
            }
            inventory_state_update_running.store(false);
        }).detach();
    }
}

void Live::StateUpdater::poll_sync_state_to_disk() {
    if (this->init_shutdown) {
        return;
    }
    if (std::chrono::steady_clock::now() - last_synced_state_to_disk_time >
        std::chrono::seconds(2)) {
        sync_state_to_disk();
        last_synced_state_to_disk_time = std::chrono::steady_clock::now();
    }
}

static constexpr long long kPaperBbaMaxAgeMs = 5000;
static constexpr long long kPaperFeedMaxAgeMs = 10000;

static double paper_slippage_bps() {
    static const double bps = [] {
        const char *env = std::getenv("PAPER_SLIPPAGE_BPS");
        return env != nullptr ? std::stod(env) : 3.0;
    }();
    return bps;
}

void Live::StateUpdater::poll_paper_fills() {
    if (std::chrono::steady_clock::now() - last_paper_fills_poll_time <
        std::chrono::seconds(1)) {
        return;
    }
    {
        std::shared_lock lock(this->paper_strategy_ids_mutex);
        if (paper_strategy_ids.empty()) return;
    }
    last_paper_fills_poll_time = std::chrono::steady_clock::now();

    auto &engine = Core::Engine::getInstance();
    const long long now_ms = DatetimeUtils::get_current_time_ms();
    const bool feed_fresh = now_ms - engine.get_latest_market_event_time_ms() <=
                            kPaperFeedMaxAgeMs;

    std::unordered_map<std::string, std::pair<double, double>> trade_range;
    {
        std::lock_guard lock(paper_trade_range_mutex);
        trade_range.swap(paper_trade_range);
    }

    std::unordered_set<unsigned long long> open_ids;
    for (const auto &order : engine.get_orders()) {
        if (!is_paper_strategy(order.strategy_id)) continue;
        open_ids.insert(order.id);
        if (!order.is_order_creation_acknowledged ||
            order.first_cancel_request_time_ms != 0) {
            continue;
        }

        Live::PaperQuote quote;
        const auto bba =
            engine.get_bba(order.symbol, order.exchange_name, order.market_type);
        if (bba.has_value() && now_ms - bba->timestamp_ms <= kPaperBbaMaxAgeMs &&
            bba->best_bid_price > 0 && bba->best_ask_price > 0) {
            quote.best_bid = bba->best_bid_price;
            quote.best_ask = bba->best_ask_price;
        }
        if (feed_fresh) {
            const auto last = engine.get_price(order.symbol, order.exchange_name,
                                               order.market_type);
            if (last.has_value() && last.value() > 0) quote.last = last;
        }
        const bool has_price = quote.best_ask.has_value() || quote.last.has_value();
        if (!has_price) continue;

        const bool is_first_check = paper_orders_checked.insert(order.id).second;
        Core::OrderResNormalized res;
        res.symbol = order.get_order_api_symbol();
        res.client_order_id = order.client_order_id;
        res.status = Core::ORDER_STATUS_NOT_WORKING;
        res.orig_quantity = order.quantity;
        res.timestamp = order.create_timestamp;
        res.last_updated_timestamp = now_ms;
        if (is_first_check && order.post_only) {
            if (Live::PaperTrading::rejects_post_only(
                    order.side, order.price.toDouble(), quote)) {
                res.price = order.price;
                res.filled_quantity = order.filled_quantity;
                this->update_order_state(res);
            }
            continue;
        }
        if (!is_first_check) {
            const auto range = trade_range.find(order.exchange_name + "|" +
                                                order.market_type + "|" +
                                                order.symbol);
            if (range != trade_range.end()) {
                quote.trade_low = range->second.first;
                quote.trade_high = range->second.second;
            }
        }
        const auto fill =
            Live::PaperTrading::evaluate(order.side, order.price.toDouble(),
                                         is_first_check, quote,
                                         paper_slippage_bps());
        if (!fill.has_value()) continue;

        const mpfr::mpreal fee_rate = engine.get_fee_rate(
            order.exchange_name, order.market_type, fill->is_maker);
        res.price = fill->price;
        res.filled_quantity = order.quantity;
        res.last_trade_timestamp = now_ms;
        res.fee = Core::OrderResFees{
            .currency = "USDT", .amount = order.quantity * fill->price * fee_rate};
        res.is_maker = fill->is_maker;
        this->update_order_state(res);
    }
    std::erase_if(paper_orders_checked,
                  [&](unsigned long long id) { return !open_ids.contains(id); });
}

void Live::StateUpdater::poll_discretionary_accounts(bool force) {
    if (!this->discretionary_accounts_active_) return;
    const auto now = std::chrono::steady_clock::now();
    if (!force && now - last_discretionary_accounts_poll_ < std::chrono::seconds(3)) return;
    last_discretionary_accounts_poll_ = now;
    auto& accounts = Core::DiscretionaryAccounts::instance();
    const auto pending = accounts.pending();
    auto& config = Live::Config::getInstance();
    if (!pending.empty()) {
        if (!this->sync_state_to_disk()) return;
        // Dollar quoting keeps settlement data separate from SQL syntax.
        const auto query = "SELECT settle_discretionary_paper_trades($settlements$" + pending.dump() + "$settlements$::jsonb) AS saved";
        const auto result = PQUtils::fetch_from_postgres(config.get_db_connection_string(), query.c_str());
        if (result.empty() || result.front().value("saved", std::string{}) != "1") return;
        accounts.acknowledge(pending);
    }
    const auto rows = PQUtils::fetch_from_postgres(config.get_db_connection_string(), R"SQL(
        SELECT d.*,a.account_type,a.exchange_name,a.market_type,
            (d.paper_capital + coalesce((SELECT sum(pnl) FROM discretionary_paper_settlement p WHERE p.account_id=d.account_id),0))::text AS capital
        FROM discretionary_account d JOIN account a USING(account_id)
        UNION ALL SELECT NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL
    )SQL");
    if (rows.empty()) return;
    std::vector<Core::DiscretionaryAccount> updated;
    for (const auto& row : rows) {
        if (row["account_id"] == "") continue;
        updated.push_back({.id = std::stoll(row["account_id"].get<std::string>()),
            .paper = row["account_type"] == "paper", .enabled = row["enabled"] == "t",
            .exchange = row["exchange_name"].get<std::string>(), .market = row["market_type"].get<std::string>(),
            .capital = row["capital"] == "" ? 0.0 : std::stod(row["capital"].get<std::string>()),
            .max_exposure = std::stod(row["max_directional_exposure"].get<std::string>()),
            .max_order_notional = std::stod(row["max_order_notional"].get<std::string>()),
            .rate_10s = std::stoi(row["num_max_orders_10_sec"].get<std::string>()),
            .rate_1m = std::stoi(row["num_max_orders_1_min"].get<std::string>()),
            .rate_1h = std::stoi(row["num_max_orders_1_hour"].get<std::string>())});
    }
    for (auto& account : updated) {
        if (account.paper) continue;
        const auto found = this->accounts.find(account.id);
        if (found == this->accounts.end() || found->second.exchange_name != account.exchange ||
            found->second.market_type != account.market) account.enabled = false;
    }
    accounts.configure(updated);
    this->discretionary_accounts_ = updated;
}

void Live::StateUpdater::poll_paper_nav() {
    if (paper_only_account_ids.empty() ||
        std::chrono::steady_clock::now() - last_paper_nav_poll_time <
            std::chrono::seconds(3)) {
        return;
    }
    last_paper_nav_poll_time = std::chrono::steady_clock::now();

    auto &engine = Core::Engine::getInstance();
    std::unordered_map<long long, double> navs;
    std::unordered_map<long long, std::pair<std::string, std::string>> venues;
    for (const auto &strategy : engine.get_strategies()) {
        if (!is_paper_strategy(strategy->strategy_config.id)) continue;
        std::set<long long> counted;
        auto exchanges = strategy->strategy_config.exchanges_list;
        if (strategy->strategy_config.discretionary_multi_account &&
            strategy->strategy_config.strategy_name == "crypto-discretionary-paper") {
            exchanges.clear();
            for (const auto& account : this->discretionary_accounts_) {
                if (!account.paper) continue;
                exchanges.push_back({account.exchange, account.market, account.id});
                if (!this->accounts.contains(account.id)) this->paper_only_account_ids.insert(account.id);
            }
        }
        for (const auto &exchange : exchanges) {
            const long long account_id = exchange.account_id.value_or(-1);
            if (!paper_only_account_ids.contains(account_id) ||
                !counted.insert(account_id).second) {
                continue;
            }
            if (strategy->strategy_config.discretionary_multi_account) {
                const auto account = Core::DiscretionaryAccounts::instance().get(account_id);
                if (!account || !account->paper) continue;
                navs.try_emplace(account_id, account->capital);
            } else {
                navs[account_id] += strategy->get_virtual_balance();
            }
            venues[account_id] = {exchange.exchange_name,
                                           exchange.market_type};
        }
    }

    // Closed trades are already in the virtual balance; open ones are marked.
    std::unordered_map<long long, std::map<std::string, Core::Inventory>>
        positions;
    for (const auto &trade : engine.get_trades()) {
        if (!is_paper_strategy(trade.strategy_id) ||
            !navs.contains(trade.account_id)) {
            continue;
        }
        const auto price =
            engine.get_price(trade.symbol, trade.exchange_name, trade.market_type);
        const double mark =
            price.value_or(trade.get_average_open_price().toDouble());
        navs[trade.account_id] +=
            Live::PaperTrading::mark_to_market_pnl(trade, mark) -
            trade.calculate_trade_fee(engine).toDouble();

        auto &position = positions[trade.account_id][trade.symbol];
        const auto remaining = trade.open_quantity - trade.closed_quantity;
        if (trade.type == Core::TradeType::LONG) {
            position.long_total += remaining;
        } else {
            position.short_total += remaining;
        }
    }

    for (const auto &[account_id, nav] : navs) {
        const auto &[exchange_name, market_type] = venues[account_id];
        auto &reported = paper_reported_symbols[account_id];
        auto &open = positions[account_id];
        // A position that closed must be reported as zero, or the engine keeps
        // the last nonzero row.
        for (const auto &symbol : reported) open.try_emplace(symbol);
        reported.clear();

        std::vector<Core::Inventory> inventories;
        for (auto &[symbol, position] : open) {
            position.total = position.long_total - position.short_total;
            position.asset = symbol;
            position.quote_conversion_symbol = symbol;
            position.exchange_name = exchange_name;
            position.market_type = market_type;
            position.account_id = account_id;
            if (position.long_total != 0 || position.short_total != 0) {
                reported.insert(symbol);
            }
            inventories.push_back(position);
        }
        const Core::QuoteAsset balance{.asset = "USDT",
                                       .quantity = nav,
                                       .used = 0,
                                       .free = nav,
                                       .price = 1,
                                       .account_id = account_id,
                                       .exchange_name = exchange_name,
                                       .market_type = market_type};
        engine.update_inventory_and_quote_assets(exchange_name, market_type,
                                                 account_id, inventories,
                                                 {balance});
    }
}

std::filesystem::path Live::StateUpdater::paper_strategy_modes_path() {
    return std::filesystem::path(
               CommonUtils::CachePathUtils::get_trading_state_path())
               .parent_path() /
           "paper_strategy_modes.json";
}

bool Live::StateUpdater::check_paper_mode_switches() {
    const auto path = paper_strategy_modes_path();
    nlohmann::json modes = nlohmann::json::object();
    if (std::filesystem::exists(path)) {
        std::ifstream file(path);
        modes = nlohmann::json::parse(file, nullptr, false);
        if (!modes.is_object()) modes = nlohmann::json::object();
    }

    const auto state = this->read_trading_state_from_disk();
    std::vector<int> conflicts;
    for (const auto &strategy : Core::Engine::getInstance().get_strategies()) {
        const int id = strategy->strategy_config.id;
        const bool is_paper = is_paper_strategy(id);
        const auto key = std::to_string(id);
        if (modes.contains(key) && modes[key].get<bool>() != is_paper &&
            state.has_value()) {
            const bool has_state =
                std::any_of(state->orders.begin(), state->orders.end(),
                            [&](const auto &o) { return o.strategy_id == id; }) ||
                std::any_of(state->trades.begin(), state->trades.end(),
                            [&](const auto &t) { return t.strategy_id == id; });
            if (has_state) conflicts.push_back(id);
        }
        modes[key] = is_paper;
    }

    if (!conflicts.empty()) {
        const nlohmann::json metadata = {{"strategy_ids", conflicts}};
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_SHUTDOWN_REQUESTED,
                  "Strategy switched between paper and live with open orders "
                  "or trades; switch it back or close it out first",
                  metadata);
        Core::Engine::getInstance().create_system_log(
            "", Core::LOG_LEVEL_ERROR,
            "Strategy switched between paper and live with open orders or "
            "trades; refusing to start",
            Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_SHUTDOWN_REQUESTED, metadata,
            -1);
        return false;
    }
    std::ofstream(path) << modes.dump(1);
    return true;
}

void Live::StateUpdater::update_order_state(
    const Core::OrderResNormalized &order_res) {
    auto symbols_orders = Core::Engine::getInstance().get_orders();
    bool found = false;
    for (auto &order : symbols_orders) {
        if (Core::Utils::is_same_order_adaptive(order,
                                                order_res.client_order_id,
                                                order_res.exchange_order_id)) {
            const std::string &order_api_symbol = order.get_order_api_symbol();
            if (order_api_symbol != order_res.symbol) {
                nlohmann::json metadata = nlohmann::json::object();
                metadata["order_symbol"] = order.symbol;
                metadata["order_api_symbol"] = order_api_symbol;
                metadata["order_res_symbol"] = order_res.symbol;
                metadata["client_order_id"] = order.client_order_id;
                metadata["order_id"] = order.id;
                metadata["order_res"] = order_res.to_json();
                Core::Engine::getInstance().create_system_log(
                    order.symbol, Core::LOG_LEVEL_ERROR,
                    "Symbol mismatch in update_order_state (possible "
                    "client_order_id collision): order symbol (" +
                        order_api_symbol + ") != order_res symbol (" +
                        order_res.symbol + ")",
                    Core::TRADING_LOG_EVENT_TYPE_ORDER_STATE_VERIFY_MISMATCH,
                    metadata, order.strategy_id, order.exchange_name,
                    order.market_type);
                continue;
            }
            this->handle_order_state_check(order, order_res);
            found = true;
        }
    }
    if (!found) {
        if (order_res.status != Core::ORDER_STATUS_NOT_WORKING) {
            nlohmann::json metadata = nlohmann::json::object();
            metadata["order_res"] = order_res.to_json().dump(4);
            Core::Engine::getInstance().create_system_log(
                order_res.symbol, Core::LOG_LEVEL_ERROR,
                "Order not found when updating order state: " +
                    order_res.client_order_id,
                Core::TRADING_LOG_EVENT_TYPE_ORDER_NOT_FOUND_ON_EXCHANGE,
                metadata, -1);
        }
    }
}

void Live::StateUpdater::register_polymarket_ticker(
    const std::shared_ptr<Core::TradeTicker> &ticker) {
    std::lock_guard<std::mutex> lock(polymarket_clob_tickers_mutex_);
    for (const auto &clob_id : ticker->clob_ids_by_index) {
        polymarket_clob_tickers_[clob_id].push_back(ticker);
        Core::Engine::getInstance().init_polymarket_orderbook(clob_id);
    }
}

void Live::StateUpdater::unregister_polymarket_ticker(
    const Core::TradeTicker &ticker) {
    std::lock_guard<std::mutex> lock(polymarket_clob_tickers_mutex_);
    for (const auto &clob_id : ticker.clob_ids_by_index) {
        auto it = polymarket_clob_tickers_.find(clob_id);
        if (it == polymarket_clob_tickers_.end()) continue;
        auto &vec = it->second;
        vec.erase(std::remove_if(
                      vec.begin(), vec.end(),
                      [&ticker](const auto &t) { return t.get() == &ticker; }),
                  vec.end());
        if (vec.empty()) {
            polymarket_clob_tickers_.erase(it);
        }
    }
}

void Live::StateUpdater::stream_info_log_callback(const std::string &log) {
    Core::Engine::getInstance().create_system_log(
        "", Core::LOG_LEVEL_INFO, log,
        Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_STARTUP_PHASE,
        nlohmann::json::object(), -1);
}

void Live::StateUpdater::stream_error_log_callback(const std::string &log) {
    Core::Engine::getInstance().create_system_log(
        "", Core::LOG_LEVEL_ERROR, log,
        Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_STREAM_PARSE_ERROR,
        nlohmann::json::object(), -1);
}

void Live::StateUpdater::system_info_log_callback(
    const std::string &log, const nlohmann::json &metadata) {
    Core::Engine::getInstance().create_system_log(
        "", Core::LOG_LEVEL_INFO, log,
        Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_STARTUP_PHASE, metadata, -1);
}

void Live::StateUpdater::system_crash_log_callback(
    const std::string &log, const nlohmann::json &metadata) {
    Core::Engine::getInstance().create_system_log(
        "", Core::LOG_LEVEL_CRASH, log,
        Core::TRADING_LOG_EVENT_TYPE_CALLBACK_FAILURE, metadata, -1);
}

void Live::StateUpdater::handle_order_create_failed_msg(
    const nlohmann::json &msg) {
    const auto &client_order_id = msg["client_order_id"].get<std::string>();
    const auto &symbol = msg["symbol"].get<std::string>();
    const auto &exchange_name = msg["exchange_name"].get<std::string>();
    const auto &market_type = msg["market_type"].get<std::string>();
    const auto &account_id = msg["account_id"].get<long long>();
    const std::string error = msg.contains("error")
                                  ? msg["error"].get<std::string>()
                                  : "Unknown error";

    Core::Engine::getInstance().create_system_log(
        symbol, Core::LOG_LEVEL_ERROR,
        "Order creation failed on exchange: " + error,
        Core::TRADING_LOG_EVENT_TYPE_ORDER_CREATE_FAILED_MSG, msg, -1,
        exchange_name, market_type);

    auto orders = Core::Engine::getInstance().get_orders();
    for (auto &order : orders) {
        if (Core::Utils::is_same_order_by_client_and_account(
                order, client_order_id, account_id)) {
            Core::Engine::getInstance().order_canceled_ack(order, true);
            return;
        }
    }
}

void Live::StateUpdater::handle_order_cancel_failed_msg(
    const nlohmann::json &msg) {
    const auto &client_order_id = msg["client_order_id"].get<std::string>();
    const auto &symbol = msg["symbol"].get<std::string>();
    const auto &exchange_name = msg["exchange_name"].get<std::string>();
    const auto &market_type = msg["market_type"].get<std::string>();
    const auto &account_id = msg["account_id"].get<long long>();
    const std::string error = msg.contains("error")
                                  ? msg["error"].get<std::string>()
                                  : "Unknown error";

    Core::Engine::getInstance().create_system_log(
        symbol, Core::LOG_LEVEL_ERROR,
        "Order cancel failed on exchange: " + error,
        Core::TRADING_LOG_EVENT_TYPE_ORDER_CANCEL_FAILED_MSG, msg, -1,
        exchange_name, market_type);

    {
        std::lock_guard<std::mutex> lock(this->pending_cancel_checks_mutex);
        this->pending_cancel_checks.push_back({client_order_id, account_id});
    }
}

void Live::StateUpdater::handle_fill_status_update_msg(
    const nlohmann::json &msg) {
    const std::string taker_order_id =
        msg.contains("taker_order_id")
            ? msg["taker_order_id"].get<std::string>()
            : "";
    const std::string status =
        msg.contains("status") ? msg["status"].get<std::string>() : "";
    const std::string market =
        msg.contains("market") ? msg["market"].get<std::string>() : "";

    if (taker_order_id.empty() || status.empty()) {
        Core::Engine::getInstance().create_system_log(
            "", Core::LOG_LEVEL_ERROR,
            "fill_status_update missing taker_order_id or status",
            Core::TRADING_LOG_EVENT_TYPE_FILL_STATUS_UPDATE_FAILED, msg, -1);
        return;
    }

    auto &engine = Core::Engine::getInstance();
    for (auto &strategy : engine.get_strategies()) {
        auto tickers_lock = strategy->lock_on_pred_market_strat();
        for (auto &[symbol, ticker] : strategy->trade_tickers) {
            if (!ticker->is_trading_on_prediction_market.load()) continue;
            if (!market.empty() && symbol != market) continue;

            if (status == "confirmed") {
                ticker->confirm_polymarket_fills(engine, taker_order_id);
            } else if (status == "failed") {
                ticker->revert_polymarket_fills(engine, taker_order_id);
            }
        }
    }
}

void Live::StateUpdater::handle_user_data_stream_msg(const std::string &msg) {
    if (!lock_acquired.load()) {
        return;
    }

    nlohmann::json json_data;
    try {
        json_data = nlohmann::json::parse(msg);
    } catch (const nlohmann::json::exception &e) {
        nlohmann::json metadata = nlohmann::json::object();
        metadata["msg"] = msg;
        metadata["error"] = e.what();
        Core::Engine::getInstance().create_system_log(
            "", Core::LOG_LEVEL_ERROR,
            "Failed to parse user data stream message JSON",
            Core::TRADING_LOG_EVENT_TYPE_USER_DATA_STREAM_PARSE_FAILED,
            metadata, -1);
        return;
    }

    if (!json_data.contains("event_type") ||
        !json_data["event_type"].is_string()) {
        nlohmann::json metadata = nlohmann::json::object();
        metadata["msg"] = msg;
        Core::Engine::getInstance().create_system_log(
            "", Core::LOG_LEVEL_ERROR,
            "User data stream message missing or invalid event_type",
            Core::TRADING_LOG_EVENT_TYPE_USER_DATA_STREAM_PARSE_FAILED,
            metadata, -1);
        return;
    }

    const auto &event_type = json_data["event_type"].get<std::string>();

    try {
        Core::Engine::getInstance().create_system_log(
            "", Core::LOG_LEVEL_INFO, "Received private stream message",
            Core::TRADING_LOG_EVENT_TYPE_PRIVATE_STREAM_MSG, json_data, -1);

        if (event_type == Live::Constants::STREAM_EVENT_TYPE_ORDER_UPDATE) {
            if (json_data.contains("orders")) {
                const auto &orders = json_data["orders"];
                for (const auto &order : orders) {
                    const auto &order_res = Core::OrderResNormalized::
                        from_ccxt_user_data_stream_msg(order);
                    this->update_order_state(order_res);
                }
            }
        } else if (event_type ==
                   Live::Constants::STREAM_EVENT_TYPE_POSITION_UPDATE) {
            std::cout << "POSITION_UPDATE: \n\n"
                      << json_data.dump(4) << std::endl;
        } else if (event_type ==
                   Live::Constants::STREAM_EVENT_TYPE_ORDER_CREATE_FAILED) {
            this->handle_order_create_failed_msg(json_data);
        } else if (event_type ==
                   Live::Constants::STREAM_EVENT_TYPE_ORDER_CANCEL_FAILED) {
            this->handle_order_cancel_failed_msg(json_data);
        } else if (event_type ==
                   Live::Constants::STREAM_EVENT_TYPE_FILL_STATUS_UPDATE) {
            this->handle_fill_status_update_msg(json_data);
        }
    } catch (const std::exception &e) {
        nlohmann::json metadata = nlohmann::json::object();
        metadata["msg"] = msg;
        metadata["event_type"] = event_type;
        metadata["error"] = e.what();
        Core::Engine::getInstance().create_system_log(
            "", Core::LOG_LEVEL_ERROR,
            "Error processing user data stream message",
            Core::TRADING_LOG_EVENT_TYPE_USER_DATA_STREAM_PARSE_FAILED,
            metadata, -1);
    }
}

void Live::StateUpdater::subscribe_market_data_streams() {
    // streams all user data from python proxy. msgs from all exchanges
    // arrive from the same stream
    const char *user_data_env = std::getenv("CCXT_PROXY_USER_DATA_WS_URL");
    std::string user_data_url =
        user_data_env ? std::string(user_data_env)
                      : "ws://localhost:8040/api/v1/user-data/stream";
    auto user_data_stream = std::make_unique<WsStream>(
        user_data_url,
        [this](const std::string &msg) {
            this->handle_user_data_stream_msg(msg);
        },
        "",
        [this](const std::string &msg) {
            this->stream_info_log_callback(msg);
        },
        [this](const std::string &msg) {
            this->stream_error_log_callback(msg);
        });
    user_data_stream->connect();
    private_streams.push_back(std::move(user_data_stream));
}

bool Live::StateUpdater::is_prevent_trading_lock_created() {
    return std::filesystem::exists(
        CommonUtils::CachePathUtils::get_prevent_trading_lock_path());
}

void Live::StateUpdater::healthcheck_python_proxy() {
    // Rate-limited emit — the healthcheck runs periodically (every N seconds)
    // and we only want one log row per failure per (exchange, market) per
    // minute. Failures in this function were previously only written to the
    // service_health table and invisible to agents via trading_activity_log.
    const int64_t now_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    constexpr int64_t PROXY_HEALTHCHECK_FAIL_COOLDOWN_MS = 60000;

    auto emit_healthcheck_fail =
        [&](const std::string &endpoint, const std::string &exchange,
            const std::string &market, const std::string &what,
            long long account_id) {
            const std::string key = endpoint + "|" + exchange + "|" + market;
            auto it = proxy_healthcheck_last_emit_ms_.find(key);
            if (it != proxy_healthcheck_last_emit_ms_.end() &&
                now_ms - it->second < PROXY_HEALTHCHECK_FAIL_COOLDOWN_MS) {
                return;
            }
            proxy_healthcheck_last_emit_ms_[key] = now_ms;
            Core::Engine::getInstance().create_system_log(
                "", Core::LOG_LEVEL_ERROR, "python proxy healthcheck failed",
                Core::TRADING_LOG_EVENT_TYPE_PYTHON_PROXY_HEALTHCHECK_FAILED,
                nlohmann::json{
                    {"endpoint", endpoint},
                    {"exchange_name", exchange},
                    {"market_type", market},
                    {"account_id", account_id},
                    {"what", what},
                },
                -1, exchange, market);
        };

    for (auto &[account_id, account] : this->accounts) {
        try {
            const auto &active_symbols = account.get_active_symbols();
            if (!active_symbols.empty()) {
                auto res = CcxtProxy::instance()
                               .call_stream_instance_ip_rotation_check(
                                   account.exchange_name, account.market_type);

                const std::string &identifier = "http_req_for_" +
                                                account.exchange_name + "_" +
                                                account.market_type;

                update_service_health_entry("python_proxy", identifier,
                                            res ? "ok" : "error",
                                            nlohmann::json::object());
                if (!res) {
                    emit_healthcheck_fail("stream_instance_ip_rotation_check",
                                          account.exchange_name,
                                          account.market_type, "returned_false",
                                          account_id);
                }
            }
        } catch (const std::exception &e) {
            const std::string &identifier = "http_req_for_" +
                                            account.exchange_name + "_" +
                                            account.market_type;
            update_service_health_entry("python_proxy", identifier, "error",
                                        nlohmann::json::object());
            emit_healthcheck_fail("stream_instance_ip_rotation_check",
                                  account.exchange_name, account.market_type,
                                  e.what(), account_id);
        } catch (...) {
            const std::string &identifier = "http_req_for_" +
                                            account.exchange_name + "_" +
                                            account.market_type;
            update_service_health_entry("python_proxy", identifier, "error",
                                        nlohmann::json::object());
            emit_healthcheck_fail("stream_instance_ip_rotation_check",
                                  account.exchange_name, account.market_type,
                                  "unknown_exception", account_id);
        }
    }

    // Inventory state health checks
    for (auto &[account_id, account] : this->accounts) {
        try {
            const auto &inventory_state = account.get_inventory_state();

            const std::string &identifier =
                "call_signed_request_for_" + account.exchange_name + "_" +
                account.market_type + "_" + std::to_string(account_id);

            update_service_health_entry(
                "python_proxy", identifier,
                inventory_state.has_value() ? "ok" : "error",
                nlohmann::json::object());
            if (!inventory_state.has_value()) {
                emit_healthcheck_fail(
                    "call_signed_request_inventory", account.exchange_name,
                    account.market_type, "no_inventory_state", account_id);
            }
        } catch (const std::exception &e) {
            const std::string &identifier =
                "call_signed_request_for_" + account.exchange_name + "_" +
                account.market_type + "_" + std::to_string(account_id);
            update_service_health_entry("python_proxy", identifier, "error",
                                        nlohmann::json::object());
            emit_healthcheck_fail("call_signed_request_inventory",
                                  account.exchange_name, account.market_type,
                                  e.what(), account_id);
        } catch (...) {
            const std::string &identifier =
                "call_signed_request_for_" + account.exchange_name + "_" +
                account.market_type + "_" + std::to_string(account_id);
            update_service_health_entry("python_proxy", identifier, "error",
                                        nlohmann::json::object());
            emit_healthcheck_fail("call_signed_request_inventory",
                                  account.exchange_name, account.market_type,
                                  "unknown_exception", account_id);
        }
    }
}

void Live::StateUpdater::create_prevent_trading_lock() {
    std::string lock_path =
        CommonUtils::CachePathUtils::get_prevent_trading_lock_path();
    if (!std::filesystem::exists(lock_path)) {
        Core::Engine::getInstance().create_system_log(
            "", Core::LOG_LEVEL_ERROR, "Prevent trading lock created",
            Core::TRADING_LOG_EVENT_TYPE_PREVENT_TRADING_LOCK_CREATED,
            nlohmann::json::object(), -1);
        std::ofstream ofs(lock_path);
        ofs << std::to_string(DatetimeUtils::get_current_time_ms());
    }
}

void Live::StateUpdater::poll_fetch_prices() {
    if (std::chrono::steady_clock::now() - last_fetch_prices_time <
        std::chrono::seconds(120)) {
        return;
    }
    last_fetch_prices_time = std::chrono::steady_clock::now();

    std::vector<std::pair<std::string, std::string>> exchange_market_type_pairs{
        {"binance", "swap"}, {"binance", "spot"}, {"bybit", "spot"},
        {"bitget", "spot"},  {"gateio", "spot"},
    };
    if (!Live::Config::getInstance().hyperliquid_futures_accounts.empty()) {
        exchange_market_type_pairs.push_back({"hyperliquid", "swap"});
    }

    for (const auto &[exchange_name, market_type] :
         exchange_market_type_pairs) {
        if (exchange_name == "polymarket") {
            continue;
        }

        const auto &prices =
            CcxtProxy::instance().fetch_prices(exchange_name, market_type);

        for (const auto &price : prices) {
            const auto &symbol = price.symbol;
            const auto price_value = price.price;
            Core::Engine::getInstance().update_price(symbol, exchange_name,
                                                     market_type, price_value);
        }
    }
}

void Live::StateUpdater::update_misc_orders() {
    const auto &nontracked_inventory_close_orders =
        Core::Engine::getInstance().get_tracked_misc_orders();

    for (const auto &[key, orders] : nontracked_inventory_close_orders) {
        for (const auto &order : orders) {
            const auto &order_res = CcxtProxy::instance().query_order(
                order.exchange_name, order.market_type, order.account_id,
                order.symbol, order.client_order_id);

            if (order_res.has_value()) {
                if (order_res->status == "closed") {
                    Core::Engine::getInstance().remove_tracked_misc_order(
                        order);
                } else if (order_res->status == "open") {
                    if (order.expires_on_ms <
                        DatetimeUtils::get_current_time_ms()) {
                        CcxtProxy::instance().cancel_order(order);
                    }
                }
            } else {
                Core::Engine::getInstance().remove_tracked_misc_order(order);
            }
        }
    }
}

void Live::StateUpdater::pause_strategy_for_mismatch(
    int strategy_id, const nlohmann::json &metadata) {
    for (const auto &strategy : Core::Engine::getInstance().get_strategies()) {
        if (strategy->strategy_config.id != strategy_id ||
            strategy->is_disabled_now()) {
            continue;
        }
        strategy->set_disabled(true);
        Core::Engine::getInstance().create_system_log(
            "", Core::LOG_LEVEL_ERROR,
            "Strategy paused: its trades disagree with the books",
            Core::TRADING_LOG_EVENT_TYPE_HUMAN_INTERVENTION_REQUIRED, metadata,
            strategy_id);
    }
}

// Paper has no venue to reconcile against; the DB trade ledger is its record.
void Live::StateUpdater::validate_paper_trades_against_db() {
    std::unordered_set<int> paper_ids;
    {
        std::shared_lock lock(this->paper_strategy_ids_mutex);
        paper_ids = this->paper_strategy_ids;
    }
    if (paper_ids.empty()) return;
    std::string id_list;
    for (const int id : paper_ids) {
        id_list += (id_list.empty() ? "" : ",") + std::to_string(id);
    }
    // The sentinel row tells a failed query apart from no open trades.
    const auto rows = PQUtils::fetch_from_postgres(
        Live::Config::getInstance().get_db_connection_string(),
        ("SELECT '' AS unique_id, -1 AS strategy_id, '' AS symbol, "
         "-1 AS account_id UNION ALL SELECT unique_id::text, strategy_id, "
         "symbol, account_id FROM trade WHERE backtest_id IS NULL AND NOT "
         "is_closed_trade AND unique_id IS NOT NULL AND strategy_id IN (" +
         id_list + ")")
            .c_str());
    if (rows.empty()) {
        Core::Engine::getInstance().create_system_log(
            "", Core::LOG_LEVEL_WARNING,
            "Paper trade check skipped: DB query failed",
            Core::TRADING_LOG_EVENT_TYPE_INVENTORY_MISMATCH_DETECTED,
            nlohmann::json::object(), -1);
        return;
    }
    const auto as_int = [](const nlohmann::json &v) {
        return v.is_string() ? std::stoll(v.get<std::string>())
                             : v.get<long long>();
    };
    struct Side {
        int strategy_id;
        std::string symbol;
        long long account_id;
    };
    std::unordered_map<std::string, Side> db_open;
    for (const auto &row : rows) {
        const int strategy_id = static_cast<int>(as_int(row["strategy_id"]));
        if (strategy_id < 0) continue;
        db_open[row["unique_id"].get<std::string>()] = {
            strategy_id, row["symbol"].get<std::string>(),
            as_int(row["account_id"])};
    }
    std::unordered_map<std::string, Side> engine_open;
    for (const auto &trade : Core::Engine::getInstance().get_trades()) {
        if (!paper_ids.contains(trade.strategy_id)) continue;
        engine_open[std::to_string(trade.id)] = {trade.strategy_id, trade.symbol,
                                                 trade.account_id};
    }

    std::unordered_map<std::string, int> streaks;
    std::map<int, nlohmann::json> confirmed_by_strategy;
    std::map<long long, long long> confirmed_by_account;
    const auto track = [&](const std::string &id, const Side &side,
                           const char *where) {
        // Per side: a short trade is engine_only before its row lands and
        // db_only before its close lands, which is two lags, not a mismatch.
        const std::string key = std::string(where) + ":" + id;
        const auto previous = this->paper_trade_mismatch_streaks.find(key);
        const int streak =
            previous == this->paper_trade_mismatch_streaks.end()
                ? 1
                : previous->second + 1;
        streaks[key] = streak;
        // One check apart covers trades still in flight to the DB.
        if (streak < 2) return;
        auto &entry = confirmed_by_strategy[side.strategy_id];
        if (entry.is_null()) {
            entry = {{"db_only", nlohmann::json::array()},
                     {"engine_only", nlohmann::json::array()}};
        }
        entry[where].push_back({{"trade_id", id}, {"symbol", side.symbol}});
        confirmed_by_account[side.account_id]++;
    };
    for (const auto &[id, side] : db_open) {
        if (!engine_open.contains(id)) track(id, side, "db_only");
    }
    for (const auto &[id, side] : engine_open) {
        if (!db_open.contains(id)) track(id, side, "engine_only");
    }
    this->paper_trade_mismatch_streaks = std::move(streaks);

    for (const auto &[strategy_id, entry] : confirmed_by_strategy) {
        nlohmann::json metadata = {
            {"db_only_count", entry["db_only"].size()},
            {"engine_only_count", entry["engine_only"].size()},
            {"db_only", entry["db_only"]},
            {"engine_only", entry["engine_only"]}};
        Core::Engine::getInstance().create_system_log(
            "", Core::LOG_LEVEL_ERROR,
            "Paper trades disagree with the DB ledger",
            Core::TRADING_LOG_EVENT_TYPE_ENGINE_STATE_MISMATCH, metadata,
            strategy_id);
        this->pause_strategy_for_mismatch(strategy_id, metadata);
    }

    std::set<long long> paper_accounts;
    for (const auto &[id, side] : db_open) paper_accounts.insert(side.account_id);
    for (const auto &[id, side] : engine_open) {
        paper_accounts.insert(side.account_id);
    }
    const auto &connection = Live::Config::getInstance().get_db_connection_string();
    for (const long long account_id : paper_accounts) {
        const auto it = confirmed_by_account.find(account_id);
        nlohmann::json db_record = {
            {"number_of_mismatches",
             it == confirmed_by_account.end() ? 0 : it->second},
            {"abs_mismatch_notional", 0},
            {"account_id", account_id},
            {"is_global", false},
            {"exchange_name", "paper"},
            {"market_type", "paper"}};
        const auto previous = PQUtils::fetch_from_postgres(
            connection,
            ("SELECT account_id FROM inventory_mismatch_state WHERE "
             "account_id = " +
             std::to_string(account_id))
                .c_str());
        if (previous.empty()) {
            PQUtils::write_to_postgres(connection, "inventory_mismatch_state",
                                       {db_record});
        } else {
            PQUtils::update_entry_by_field(connection,
                                           "inventory_mismatch_state", db_record,
                                           "account_id",
                                           std::to_string(account_id));
        }
    }
}

// Detects and pauses only: closing untracked inventory and repaying loans
// send real requests, and stay off until real accounts trade again.
void Live::StateUpdater::validate_inventory_against_current_active_trades() {
    this->validate_paper_trades_against_db();
    if (this->accounts.empty()) return;
    this->poll_fetch_prices();
    this->update_inventory_state();

    long long total_mismatches = 0;
    double total_abs_mismatch_notional = 0;
    std::unordered_map<long long, long long> mismatched_by_account_id;
    std::unordered_map<long long, double> abs_mismatch_notional_by_account_id;
    std::unordered_set<std::string> processed_symbols;

    std::set<std::pair<long long, std::string>> mismatched_account_symbols;

    Core::Engine::getInstance().create_system_log(
        "", Core::LOG_LEVEL_INFO,
        "Validating inventory against current active "
        "trades",
        Core::TRADING_LOG_EVENT_TYPE_INVENTORY_MISMATCH_DETECTED,
        nlohmann::json::object(), -1);

    for (auto &[account_id, account] : this->accounts) {
        const auto &inventory_state = account.get_inventory_state();

        if (!inventory_state.has_value()) {
            Core::Engine::getInstance().create_system_log(
                "", Core::LOG_LEVEL_ERROR,
                "Inventory request did not return a value for account: " +
                    std::to_string(account_id),
                Core::TRADING_LOG_EVENT_TYPE_INVENTORY_MISMATCH_DETECTED,
                nlohmann::json::object(), -1);
            continue;
        }

        Core::Engine::getInstance().update_inventory_and_quote_assets(
            account.exchange_name, account.market_type, account_id,
            inventory_state.value().second, inventory_state.value().first);

        if (account.exchange_name == "polymarket") {
            continue;
        }

        auto full_account_state =
            Core::Engine::getInstance().get_full_account_state(account_id);
        std::erase_if(full_account_state.trades, [&](const Core::Trade &t) {
            return this->is_paper_strategy(t.strategy_id);
        });

        std::vector<std::string> active_symbols =
            Core::ExchangeInfo::getInstance().get_active_symbols(
                account.exchange_name, account.market_type);

        for (const auto &symbol : active_symbols) {
            if (symbol == "BRICUSDT" || symbol == "BONDUSDT") {
                continue;
            }

            const auto &key =
                Core::Utils::get_symbol_exchange_market_account_key(
                    symbol, account.exchange_name, account.market_type,
                    account_id);
            if (processed_symbols.contains(key)) {
                continue;
            }
            processed_symbols.insert(key);

            const auto &price = Core::Engine::getInstance().get_price(
                symbol, account.exchange_name, account.market_type);

            if (!price.has_value()) {
                continue;
            }

            const bool is_hedge_venue = account.exchange_name == "binance" &&
                                        account.market_type == "swap";

            struct SideCheck {
                std::string suffix;
                std::optional<Core::PositionSide> side;
                mpfr::mpreal inv;
                mpfr::mpreal trade_qty;
            };
            std::vector<SideCheck> checks;
            if (is_hedge_venue) {
                checks.push_back(
                    {":LONG", Core::PositionSide::LONG_SIDE,
                     Core::Utils::get_account_symbol_inventory_by_side(
                         symbol, account.exchange_name, account.market_type,
                         account_id, full_account_state.inventories,
                         Core::PositionSide::LONG_SIDE),
                     Core::Utils::get_acc_symbol_trade_open_quantity_by_side(
                         full_account_state.trades, symbol,
                         account.exchange_name, account.market_type, account_id,
                         Core::PositionSide::LONG_SIDE)});
                checks.push_back(
                    {":SHORT", Core::PositionSide::SHORT_SIDE,
                     Core::Utils::get_account_symbol_inventory_by_side(
                         symbol, account.exchange_name, account.market_type,
                         account_id, full_account_state.inventories,
                         Core::PositionSide::SHORT_SIDE),
                     Core::Utils::get_acc_symbol_trade_open_quantity_by_side(
                         full_account_state.trades, symbol,
                         account.exchange_name, account.market_type, account_id,
                         Core::PositionSide::SHORT_SIDE)});
            } else {
                checks.push_back(
                    {"", std::nullopt,
                     Core::Utils::get_account_symbol_inventory(
                         symbol, account.exchange_name, account.market_type,
                         account_id, full_account_state.inventories),
                     Core::Utils::get_acc_symbol_trade_open_quantity(
                         full_account_state.trades, symbol,
                         account.exchange_name, account.market_type,
                         account_id)});
            }

            for (const auto &chk : checks) {
                const std::string side_key = key + chk.suffix;
                const double signed_delta =
                    (chk.inv - chk.trade_qty).toDouble();

                const double diff_abs = std::abs(signed_delta) * price.value();

                if (diff_abs > 5) {
                    auto it_curr_mismatch_count =
                        this->inventory_mismatches_sqlite_cache->get(side_key);

                    if (it_curr_mismatch_count.has_value() &&
                        it_curr_mismatch_count.value() > 0) {
                        long long new_value =
                            it_curr_mismatch_count.value() + 1;
                        this->inventory_mismatches_sqlite_cache->insert(
                            side_key, new_value);
                        if (new_value > 4) {
                            nlohmann::json metadata = nlohmann::json::object();
                            metadata["account_id"] = account_id;
                            metadata["symbol"] = symbol;
                            metadata["exchange_name"] = account.exchange_name;
                            metadata["market_type"] = account.market_type;
                            metadata["inventory"] = chk.inv.toDouble();
                            metadata["trade_open_quantity"] =
                                chk.trade_qty.toDouble();
                            metadata["mismatch_notional"] = diff_abs;
                            metadata["price"] = price.value();
                            metadata["mismatch_quantity"] = signed_delta;
                            metadata["position_side"] =
                                chk.side.has_value()
                                    ? (chk.side.value() ==
                                               Core::PositionSide::LONG_SIDE
                                           ? "LONG"
                                           : "SHORT")
                                    : "BOTH";

                            Core::Engine::getInstance().create_system_log(
                                symbol, Core::LOG_LEVEL_ERROR,
                                "Inventory mismatch for symbol: " + symbol +
                                    " on account: " +
                                    std::to_string(account_id) + " side=" +
                                    metadata["position_side"]
                                        .get<std::string>(),
                                Core::
                                    TRADING_LOG_EVENT_TYPE_ENGINE_STATE_MISMATCH,
                                metadata, -1);

                            total_mismatches++;
                            total_abs_mismatch_notional += diff_abs;

                            mismatched_by_account_id[account_id]++;
                            abs_mismatch_notional_by_account_id[account_id] +=
                                diff_abs;

                            mismatched_account_symbols.insert(
                                {account_id, symbol});
                        }
                    } else {
                        this->inventory_mismatches_sqlite_cache->insert(
                            side_key, 1);
                    }
                } else {
                    this->inventory_mismatches_sqlite_cache->insert(side_key,
                                                                    0);
                }
            }
        }
    }

    std::unordered_set<int> strategies_to_pause;
    for (const auto &trade : Core::Engine::getInstance().get_trades()) {
        if (mismatched_account_symbols.contains(
                {trade.account_id, trade.symbol})) {
            strategies_to_pause.insert(trade.strategy_id);
        }
    }
    for (const int strategy_id : strategies_to_pause) {
        this->pause_strategy_for_mismatch(
            strategy_id, {{"reason", "venue inventory mismatch"}});
    }

    Core::Engine::getInstance().create_system_log(
        "", Core::LOG_LEVEL_INFO,
        "Ratio of inventory mismatches: " + std::to_string(total_mismatches) +
            "/" + std::to_string(processed_symbols.size()),
        Core::TRADING_LOG_EVENT_TYPE_INVENTORY_MISMATCH_DETECTED,
        nlohmann::json::object(), -1);

    double nav = Core::Engine::getInstance().get_nav();

    if (nav == 0) {
        const auto start_time = std::chrono::steady_clock::now();
        const auto timeout = std::chrono::seconds(10);

        while (nav == 0 &&
               std::chrono::steady_clock::now() - start_time < timeout) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            nav = Core::Engine::getInstance().get_nav();
        }

        if (nav == 0) {
            Core::Engine::getInstance().create_system_log(
                "", Core::LOG_LEVEL_WARNING,
                "Nav is 0 after 10 second timeout, skipping inventory "
                "mismatch "
                "check",
                Core::TRADING_LOG_EVENT_TYPE_INVENTORY_MISMATCH_DETECTED,
                nlohmann::json::object(), -1);
        } else {
            const auto mismatch_ratio = total_abs_mismatch_notional / nav;
            if (mismatch_ratio > 0.05) {
                Core::Engine::getInstance().create_system_log(
                    "", Core::LOG_LEVEL_WARNING,
                    "Inventory mismatch is too high: " +
                        std::to_string(mismatch_ratio * 100) + "%",
                    Core::TRADING_LOG_EVENT_TYPE_HUMAN_INTERVENTION_REQUIRED,
                    nlohmann::json::object(), -1);
            }
        }
    }

    auto &config = Live::Config::getInstance();

    for (auto &[account_id, account] : this->accounts) {
        const auto mismatches_count_it =
            mismatched_by_account_id.find(account_id);
        const auto abs_mismatch_notional_it =
            abs_mismatch_notional_by_account_id.find(account_id);

        long long mismatches_count = 0;
        double abs_mismatch_notional = 0;

        if (mismatches_count_it != mismatched_by_account_id.end() &&
            abs_mismatch_notional_it !=
                abs_mismatch_notional_by_account_id.end()) {
            mismatches_count = mismatches_count_it->second;
            abs_mismatch_notional = abs_mismatch_notional_it->second;
        }

        nlohmann::json db_record = nlohmann::json::object();
        db_record["number_of_mismatches"] = mismatches_count;
        db_record["abs_mismatch_notional"] = abs_mismatch_notional;
        db_record["account_id"] = account_id;
        db_record["is_global"] = false;
        db_record["exchange_name"] = account.exchange_name;
        db_record["market_type"] = account.market_type;

        std::vector<json> prev_records = PQUtils::fetch_from_postgres(
            config.get_db_connection_string(),
            ("SELECT * FROM inventory_mismatch_state WHERE account_id = " +
             std::to_string(account_id))
                .c_str());

        if (prev_records.empty()) {
            PQUtils::write_to_postgres(config.get_db_connection_string(),
                                       "inventory_mismatch_state", {db_record});
        } else {
            PQUtils::update_entry_by_field(
                config.get_db_connection_string(), "inventory_mismatch_state",
                db_record, "account_id", std::to_string(account_id));
        }
    }
}

void Live::StateUpdater::handle_invidual_order_not_found(Core::Order order) {
    if (!order.is_order_creation_acknowledged) {
        Core::Engine::getInstance().create_system_log(
            order.symbol, Core::LOG_LEVEL_ERROR,
            "handle_invidual_order_not_found: unacked order not found, "
            "removing from state",
            Core::TRADING_LOG_EVENT_TYPE_ORDER_NOT_FOUND_ON_EXCHANGE,
            order.to_json(), order.strategy_id, order.exchange_name,
            order.market_type);
        Core::Engine::getInstance().order_canceled_ack(order, true);
        return;
    }

    const auto order_key = Core::Utils::get_order_key(order);
    const auto unacked_orders_failed_to_retrieve_it =
        this->unacked_orders_failed_to_retrieve_cache->get(order_key);

    if (!unacked_orders_failed_to_retrieve_it.has_value()) {
        this->unacked_orders_failed_to_retrieve_cache->insert(order_key, 1);
        return;
    } else {
        this->unacked_orders_failed_to_retrieve_cache->insert(
            order_key, unacked_orders_failed_to_retrieve_it.value() + 1);
    }

    if (unacked_orders_failed_to_retrieve_it.has_value() &&
        unacked_orders_failed_to_retrieve_it.value() < 2) {
        return;
    }

    const auto now = DatetimeUtils::get_current_time_ms();
    const auto one_hour_ms = 60 * 60 * 1000;
    if (order.create_timestamp + one_hour_ms < now) {
        Core::Engine::getInstance().create_system_log(
            order.symbol, Core::LOG_LEVEL_ERROR,
            "handle_invidual_order_not_found: acked order not found and "
            "older than 1 hour, removing from state",
            Core::TRADING_LOG_EVENT_TYPE_ORDER_NOT_FOUND_ON_EXCHANGE,
            order.to_json(), order.strategy_id, order.exchange_name,
            order.market_type);
        Core::Engine::getInstance().order_canceled_ack(order, true);
    }
}

void Live::StateUpdater::create_state_updater_running_log() {
    static std::chrono::steady_clock::time_point
        last_created_state_updater_running_log =
            std::chrono::steady_clock::now();
    if (std::chrono::steady_clock::now() -
            last_created_state_updater_running_log <
        std::chrono::seconds(60 * 5)) {
        return;
    }
    last_created_state_updater_running_log = std::chrono::steady_clock::now();
    update_service_health_entry("trading_program", "state_updater_running",
                                "ok", nlohmann::json::object());
}

void Live::StateUpdater::poll_check_unacked_orders() {
    if (std::chrono::steady_clock::now() - last_check_unacked_orders_time <
        std::chrono::seconds(2)) {
        return;
    }
    last_check_unacked_orders_time = std::chrono::steady_clock::now();
    const auto unacked_orders =
        Core::Engine::getInstance().get_unacked_orders();
    const auto now = DatetimeUtils::get_current_time_ms();
    const auto loop_start_time = std::chrono::steady_clock::now();
    for (const auto &order : unacked_orders) {
        if (order.is_prediction_market_order ||
            is_paper_strategy(order.strategy_id)) {
            continue;
        }
        if (std::chrono::steady_clock::now() - loop_start_time >=
            std::chrono::seconds(2)) {
            break;
        }
        if (order.create_timestamp + 15000 < now) {
            if (order.client_order_id.empty() &&
                order.exchange_order_id.empty()) {
                continue;
            }
            const auto order_res = CcxtProxy::instance().query_order(order);
            if (order_res.has_value()) {
                this->update_order_state(order_res.value());
            } else {
                this->handle_invidual_order_not_found(order);
            }
        }
    }
}

void Live::StateUpdater::poll_check_failed_cancels() {
    std::vector<std::pair<std::string, long long>> pending_checks;
    {
        std::lock_guard<std::mutex> lock(this->pending_cancel_checks_mutex);
        pending_checks = std::move(this->pending_cancel_checks);
        this->pending_cancel_checks.clear();
    }

    if (!pending_checks.empty()) {
        auto orders = Core::Engine::getInstance().get_orders();
        for (const auto &[client_order_id, account_id] : pending_checks) {
            for (auto &order : orders) {
                if (order.is_prediction_market_order ||
                    is_paper_strategy(order.strategy_id)) {
                    continue;
                }
                if (Core::Utils::is_same_order_by_client_and_account(
                        order, client_order_id, account_id)) {
                    const auto order_res =
                        CcxtProxy::instance().query_order(order);
                    if (order_res.has_value()) {
                        this->update_order_state(order_res.value());
                    } else {
                        Core::Engine::getInstance().create_system_log(
                            order.symbol, Core::LOG_LEVEL_ERROR,
                            "Order not found after cancel failed, removing "
                            "from "
                            "state",
                            Core::TRADING_LOG_EVENT_TYPE_FAILED_CANCEL_RETRY,
                            nlohmann::json::object(), -1);
                        Core::Engine::getInstance().order_canceled_ack(order);
                    }
                    break;
                }
            }
        }
    }

    if (std::chrono::steady_clock::now() - last_check_failed_cancels_time <
        std::chrono::seconds(15)) {
        return;
    }
    last_check_failed_cancels_time = std::chrono::steady_clock::now();

    const auto failed_cancels =
        Core::Engine::getInstance().get_failed_cancels();
    const auto now = DatetimeUtils::get_current_time_ms();
    for (auto order : failed_cancels) {
        if (order.is_prediction_market_order ||
            is_paper_strategy(order.strategy_id)) {
            continue;
        }
        if (order.first_cancel_request_time_ms != 0 &&
            order.first_cancel_request_time_ms + 15000 < now) {
            if (order.client_order_id.empty() &&
                order.exchange_order_id.empty()) {
                continue;
            }
            const auto order_res = CcxtProxy::instance().query_order(order);
            if (order_res.has_value()) {
                this->update_order_state(order_res.value());
            } else {
                Core::Engine::getInstance().create_system_log(
                    order.symbol, Core::LOG_LEVEL_ERROR,
                    "poll_check_failed_cancels: order not found, removing "
                    "from "
                    "state",
                    Core::TRADING_LOG_EVENT_TYPE_FAILED_CANCEL_RETRY,
                    nlohmann::json::object(), -1);
                Core::Engine::getInstance().order_canceled_ack(order);
            }
        }
    }
}

void Live::StateUpdater::create_new_trade_ticker_entries() {
    auto &config = Live::Config::getInstance();
    std::vector<std::pair<std::string, std::string>> exchange_market_type_pairs{
        {"binance", "swap"},
        {"binance", "spot"},
        {"gateio", "spot"},
        {"ibkr", "spot"}};

    for (const auto &[exchange_name, market_type] :
         exchange_market_type_pairs) {
        const auto &symbols = CcxtProxy::instance().get_exchange_active_symbols(
            exchange_name, market_type);

        for (const auto &strategy : this->fed_strategy_rows()) {
            if (strategy.main_exchange != exchange_name) {
                continue;
            }

            if (strategy.is_using_all_symbols &&
                strategy.strategy_type.find(market_type) != std::string::npos) {
                for (const auto &symbol : symbols) {
                    auto it = std::find_if(strategy.trade_tickers.begin(),
                                           strategy.trade_tickers.end(),
                                           [&symbol](const auto &tt) {
                                               return tt.symbol == symbol;
                                           });

                    if (it == strategy.trade_tickers.end()) {
                        const auto &[base_asset, quote_asset] =
                            CCXTUtils::infer_base_quote_assets(
                                symbol, exchange_name == "ibkr" ? "USD" : "");

                        if (!CCXTUtils::is_supported_quote_for_venue(
                                exchange_name, quote_asset)) {
                            continue;
                        }

                        std::cout << "Quote asset: " << quote_asset
                                  << std::endl;

                        nlohmann::json trade_ticker_json =
                            nlohmann::json::object();
                        trade_ticker_json["symbol"] = symbol;
                        trade_ticker_json["base_asset"] = base_asset;
                        trade_ticker_json["quote_asset"] =
                            exchange_name == "ibkr" ? "USD" : quote_asset;
                        trade_ticker_json["strategy_id"] = strategy.id;

                        std::cout << "Creating new trade ticker: "
                                  << trade_ticker_json.dump() << std::endl;

                        PQUtils::write_to_postgres(
                            config.get_db_connection_string(), "trade_ticker",
                            {trade_ticker_json});
                    }
                }
            }
        }
    }
}

void Live::StateUpdater::live_event_consumer_loop() {
    LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_STARTUP_PHASE,
             "Live event consumer thread started", nlohmann::json{});
    while (true) {
        MarketData::MarketEvent event;
        if (live_event_queue_.wait_dequeue_timed(
                event, std::chrono::milliseconds(100))) {
            switch (event.type) {
                case MarketData::MarketEvent::BBA: {
                    if (!event.bba.has_value()) break;
                    auto &b = event.bba.value();
                    Core::Engine::getInstance().update_bba(
                        b.exchange_name, b.market_type, b.symbol, b.bid_price,
                        b.bid_qty, b.ask_price, b.ask_qty, b.timestamp_ms);
                    break;
                }
                case MarketData::MarketEvent::L2_UPDATE: {
                    if (!event.l2_update.has_value()) break;
                    auto &l2 = event.l2_update.value();
                    if (l2.sub_type ==
                        MarketData::L2UpdateEvent::FULL_REFRESH) {
                        if (!l2.full_refresh_orderbook.has_value()) break;
                        Core::Engine::getInstance()
                            .apply_full_refresh_orderbook(
                                l2.exchange_name, l2.market_type, l2.symbol,
                                l2.full_refresh_orderbook.value());
                    } else if (l2.sub_type ==
                               MarketData::L2UpdateEvent::LEVELS_ONLY) {
                        if (!l2.full_refresh_orderbook.has_value()) break;
                        Core::Engine::getInstance().apply_orderbook_levels_only(
                            l2.exchange_name, l2.market_type, l2.symbol,
                            l2.full_refresh_orderbook.value());
                    } else {
                        Core::Engine::getInstance().apply_orderbook_update(
                            l2.update);
                    }
                    break;
                }
                case MarketData::MarketEvent::TRADE: {
                    if (!event.trade.has_value()) break;
                    const auto &t = event.trade.value();
                    if (!(t.price > 0)) break;
                    std::lock_guard lock(paper_trade_range_mutex);
                    auto [it, inserted] = paper_trade_range.try_emplace(
                        t.exchange_name + "|" + t.market_type + "|" + t.symbol,
                        t.price, t.price);
                    if (!inserted) {
                        it->second.first = std::min(it->second.first, t.price);
                        it->second.second = std::max(it->second.second, t.price);
                    }
                    break;
                }
                case MarketData::MarketEvent::POLYMARKET_TICK_SIZE_CHANGE: {
                    if (!event.polymarket_tick_size_change.has_value()) break;
                    auto &ts = event.polymarket_tick_size_change.value();
                    for (auto &strategy :
                         Core::Engine::getInstance().get_strategies()) {
                        if (!strategy->is_trading_on_prediction_market)
                            continue;
                        auto lock = strategy->lock_on_pred_market_strat();
                        auto it = strategy->trade_tickers.find(ts.condition_id);
                        if (it == strategy->trade_tickers.end()) continue;
                        it->second->order_price_min_tick_size.store(
                            ts.new_tick_size);
                    }
                    break;
                }
                case MarketData::MarketEvent::POLYMARKET_BBA: {
                    if (!event.polymarket_bba.has_value()) break;
                    auto &pm = event.polymarket_bba.value();
                    std::lock_guard<std::mutex> lock(
                        polymarket_clob_tickers_mutex_);
                    auto it = polymarket_clob_tickers_.find(pm.asset_id);
                    if (it != polymarket_clob_tickers_.end()) {
                        for (auto &ticker : it->second) {
                            auto idx_it = ticker->clob_id_to_outcome_index.find(
                                pm.asset_id);
                            if (idx_it !=
                                ticker->clob_id_to_outcome_index.end()) {
                                ticker->update_outcome_best_bid_ask(
                                    idx_it->second, pm.best_bid, pm.best_ask,
                                    pm.timestamp_ms);
                                Core::Engine::getInstance()
                                    .update_polymarket_event_bba(
                                        ticker->condition_id, idx_it->second,
                                        pm.best_bid, pm.best_ask,
                                        pm.timestamp_ms);
                            }
                        }
                    }
                    break;
                }
                case MarketData::MarketEvent::POLYMARKET_L2_SNAPSHOT: {
                    if (!event.polymarket_l2_snapshot.has_value()) break;
                    auto &snap = event.polymarket_l2_snapshot.value();
                    Core::Engine::getInstance().apply_polymarket_l2_snapshot(
                        snap.asset_id, snap.orderbook);
                    break;
                }
                case MarketData::MarketEvent::POLYMARKET_L2_DELTA: {
                    if (!event.polymarket_l2_delta.has_value()) break;
                    auto &delta = event.polymarket_l2_delta.value();
                    Core::Engine::getInstance().apply_polymarket_l2_delta(
                        delta.asset_id, delta.price, delta.size, delta.is_bid,
                        delta.timestamp_ms);

                    // BBA from price_change (replaces old best_bid_ask)
                    std::lock_guard<std::mutex> lock(
                        polymarket_clob_tickers_mutex_);
                    auto it = polymarket_clob_tickers_.find(delta.asset_id);
                    if (it != polymarket_clob_tickers_.end()) {
                        for (auto &ticker : it->second) {
                            auto idx_it = ticker->clob_id_to_outcome_index.find(
                                delta.asset_id);
                            if (idx_it !=
                                ticker->clob_id_to_outcome_index.end()) {
                                ticker->update_outcome_best_bid_ask(
                                    idx_it->second, delta.best_bid,
                                    delta.best_ask, delta.timestamp_ms);
                                Core::Engine::getInstance()
                                    .update_polymarket_event_bba(
                                        ticker->condition_id, idx_it->second,
                                        delta.best_bid, delta.best_ask,
                                        delta.timestamp_ms);
                            }
                        }
                    }
                    break;
                }
                case MarketData::MarketEvent::POLYMARKET_LAST_TRADE: {
                    if (!event.polymarket_last_trade.has_value()) break;
                    auto &trade = event.polymarket_last_trade.value();
                    {
                        std::lock_guard<std::mutex> lock(
                            polymarket_clob_tickers_mutex_);
                        auto it = polymarket_clob_tickers_.find(trade.asset_id);
                        if (it != polymarket_clob_tickers_.end()) {
                            for (auto &ticker : it->second) {
                                auto idx_it =
                                    ticker->clob_id_to_outcome_index.find(
                                        trade.asset_id);
                                if (idx_it !=
                                    ticker->clob_id_to_outcome_index.end()) {
                                    ticker->update_outcome_trade_flow(
                                        idx_it->second, trade.is_buy,
                                        trade.size, trade.price,
                                        trade.timestamp_ms);
                                    Core::Engine::getInstance()
                                        .update_polymarket_event_last_trade(
                                            ticker->condition_id,
                                            idx_it->second, trade.price,
                                            trade.timestamp_ms);
                                }
                            }
                        }
                    }
                    // Forward to the SM dispatcher so PolymarketTradeFlowSM
                    // ingests CLOB trades and computes windowed PM OFI / volume
                    // / trade-rate (read by the strategy via get_flow()).
                    // Enqueued after releasing polymarket_clob_tickers_mutex_ —
                    // never hold it across the dispatcher's own queue lock.
                    MarketData::StateMachineDispatcher::get_instance().enqueue(
                        MarketData::MarketEvent(event));
                    break;
                }
                case MarketData::MarketEvent::POLYMARKET_MARKET_RESOLVED:
                    break;
            }
        }
    }
}

void Live::StateUpdater::reconnect_streams() {
    for (auto &stream : private_streams) {
        if (stream->is_disconnected()) {
            LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_PRIVATE_STREAM_DISCONNECTED,
                     "Reconnecting private stream: " + stream->wsurl_,
                     nlohmann::json{});
            this->system_info_log_callback(
                "Reconnecting private stream: " + stream->wsurl_,
                nlohmann::json::object());
            stream->connect();
        }
    }

    Live::AccountStreamManager::getInstance().reconnect_disconnected();
    Live::AccountStreamManager::getInstance().check_staleness();
}

void Live::StateUpdater::run_extensive_exchange_health_checks() {
    healthcheck_python_proxy();
    check_core_dumps();

    return;

    for (auto &[account_id, account] : this->accounts) {
        const auto &prices = CcxtProxy::instance().fetch_prices(
            account.exchange_name, account.market_type);

        const bool is_binance_spot =
            account.exchange_name == "binance" && account.market_type == "spot";

        const std::string symbol = is_binance_spot ? "DOGEUSDC" : "DOGEUSDT";

        for (const auto &price : prices) {
            if (price.symbol == "DOGEUSDT") {
                Core::CreateOrderParams params{
                    .symbol = symbol,
                    .type = Core::OrderType::LIMIT,
                    .side = Core::OrderSide::BUY,
                    .price = price.price * 0.9875,
                    .prediction = std::nullopt,
                    .quantity = 6 / price.price,
                    .exchange_name = account.exchange_name,
                    .market_type = account.market_type,
                    .strategy_id = -1,
                    .expires_on_ms = 0,
                    .sizing_coeff_delta = 0,
                    .trade_id = std::nullopt,
                    .reduce_only = false,
                    .timestamp = DatetimeUtils::get_current_time_ms(),
                    .quote_asset = is_binance_spot ? "USDC" : "USDT",
                    .base_asset = "DOGE",
                    .allowed_to_bypass_risk_engine = true,
                    .must_pass_min_notional_filter = true,
                    .is_trade_close_order = false,
                    .kv_cache = {},
                    .hedge_for_trade_id = std::nullopt,
                    .position_side = Core::PositionSide::LONG_SIDE};
                params.account_id = account_id;

                auto order = Core::Order::create(
                    params.symbol, params.type, params.side, params.price,
                    params.prediction, params.quantity, params.exchange_name,
                    params.market_type, params.strategy_id,
                    params.expires_on_ms, params.sizing_coeff_delta,
                    params.trade_id, params.reduce_only, params.timestamp,
                    params.quote_asset, params.base_asset,
                    params.allowed_to_bypass_risk_engine,
                    params.is_trade_close_order, params.type_internal,
                    params.is_alt_market_routed,
                    params.route_order_to_alt_symbol,
                    params.routed_order_base_asset,
                    params.routed_order_quote_asset, params.margin_auto_borrow,
                    params.margin_auto_repay,
                    params.derivatives_settle_currency, params.kv_cache,
                    params.hedge_for_trade_id, params.account_id.value(),
                    params.position_side);

                Core::Engine::getInstance().round_quantity_and_price(order);
                const std::string &client_order_id = order.client_order_id;

                const auto &key = Core::Utils::get_exchange_market_account_key(
                    account.exchange_name, account.market_type, account_id);

                CcxtProxy::instance().create_order(order);
                std::this_thread::sleep_for(std::chrono::seconds(15));

                const auto &orders_on_symbol =
                    CcxtProxy::instance().get_orders_on_symbol(
                        account.exchange_name, account.market_type, account_id,
                        symbol);

                const auto &open_orders_on_symbol =
                    CcxtProxy::instance().get_open_orders_on_symbol(
                        account.exchange_name, account.market_type, account_id,
                        symbol);

                const auto query_by_order_id_res =
                    CcxtProxy::instance().query_order(
                        account.exchange_name, account.market_type, account_id,
                        symbol, client_order_id);

                bool order_was_created = false;

                bool found_on_orders_on_symbol = false;
                bool found_on_open_orders_on_symbol = false;
                bool found_on_query_by_order_id = false;

                for (const auto &order : orders_on_symbol) {
                    if (order.client_order_id == client_order_id) {
                        found_on_orders_on_symbol = true;
                        break;
                    }
                }

                for (const auto &order : open_orders_on_symbol) {
                    if (order.client_order_id == client_order_id) {
                        found_on_open_orders_on_symbol = true;
                        break;
                    }
                }

                order_was_created = found_on_orders_on_symbol ||
                                    found_on_open_orders_on_symbol ||
                                    query_by_order_id_res.has_value();

                nlohmann::json metadata = nlohmann::json::object();
                metadata["order"] = order.to_ccxt_proxy_format().dump();

                update_service_health_entry(
                    "trading_program", "create_order_" + key,
                    order_was_created ? "ok" : "error", metadata);

                update_service_health_entry(
                    "trading_program", "get_open_orders_on_symbol_" + key,
                    found_on_open_orders_on_symbol ? "ok" : "error",
                    nlohmann::json::object());

                update_service_health_entry(
                    "trading_program", "get_orders_on_symbol_" + key,
                    found_on_orders_on_symbol ? "ok" : "error",
                    nlohmann::json::object());

                update_service_health_entry(
                    "trading_program", "query_order_" + key,
                    query_by_order_id_res.has_value() ? "ok" : "error",
                    nlohmann::json::object());

                CcxtProxy::instance().cancel_order(order);

                std::this_thread::sleep_for(std::chrono::seconds(15));

                const auto &orders_on_symbol_after_cancel =
                    CcxtProxy::instance().query_order(
                        account.exchange_name, account.market_type, account_id,
                        symbol, client_order_id);

                if (orders_on_symbol_after_cancel.has_value()) {
                    update_service_health_entry(
                        "trading_program", "order_cancel_confirmation_" + key,
                        orders_on_symbol_after_cancel.value().status == "open"
                            ? "error"
                            : "ok",
                        nlohmann::json::object());
                }
            }
        }
    }
}

void Live::StateUpdater::init_stall_orders_sqlite_cache() {
    this->stall_orders_sqlite_cache = std::make_unique<SqliteCache<long long>>(
        CommonUtils::CachePathUtils::get_sqlite_cache_path("stall_orders.db"));

    // Loop through all accounts
    for (auto &[account_id, account] : accounts) {
        // Get active symbols for this account
        const auto &active_symbols = account.get_active_symbols();

        for (const auto &symbol : active_symbols) {
            const auto &key =
                Core::Utils::get_symbol_exchange_market_account_key(
                    symbol, account.exchange_name, account.market_type,
                    account_id);

            if (!this->stall_orders_sqlite_cache->get(key).has_value()) {
                this->stall_orders_sqlite_cache->insert(key, 0);
            }
        }
    }
}

void Live::StateUpdater::create_account_db_records() {
    auto &config = Live::Config::getInstance();

    for (const auto &[account_id, account] : this->accounts) {
        nlohmann::json db_record = nlohmann::json::object();
        db_record["account_id"] = account_id;
        db_record["exchange_name"] = account.exchange_name;
        db_record["market_type"] = account.market_type;

        if (account.exchange_name == "binance") {
            db_record["account_type"] = "crypto";
        } else if (account.exchange_name == "bybit") {
            db_record["account_type"] = "crypto";
        } else if (account.exchange_name == "bitget") {
            db_record["account_type"] = "crypto";
        } else if (account.exchange_name == "gateio") {
            db_record["account_type"] = "crypto";
        } else if (account.exchange_name == "polymarket") {
            db_record["account_type"] = "prediction_market";
        } else if (account.exchange_name == "ibkr") {
            db_record["account_type"] = "broker";
        }

        PQUtils::write_to_postgres(config.get_db_connection_string(), "account",
                                   {db_record});
    }

    this->write_paper_account_records(this->fed_strategy_rows());
}

void Live::StateUpdater::write_paper_account_records(
    const std::vector<StrategyConfig> &strategies) {
    auto &config = Live::Config::getInstance();
    std::set<long long> written;
    for (const auto &row : PQUtils::fetch_from_postgres(
             config.get_db_connection_string(),
             "SELECT account_id FROM account")) {
        written.insert(std::stoll(row["account_id"].get<std::string>()));
    }
    for (const auto &strategy : strategies) {
        if (!strategy.is_paper_trading) continue;
        for (const auto &exchange : strategy.exchanges_list) {
            const long long account_id = exchange.account_id.value_or(-1);
            if (!this->paper_only_account_ids.contains(account_id) ||
                !written.insert(account_id).second) {
                continue;
            }
            PQUtils::write_to_postgres(
                config.get_db_connection_string(), "account",
                {{{"account_id", account_id},
                  {"exchange_name", exchange.exchange_name},
                  {"market_type", exchange.market_type},
                  {"account_type", "paper"}}});
        }
    }
}

void Live::StateUpdater::init_caches() {
    init_stall_orders_sqlite_cache();
    init_inventory_mismatches_sqlite_cache();
    init_nontracked_positions_close_times_cache();
    init_borrowable_inventories_sqlite_cache();
    this->nontracked_positions_close_order_ids_cache =
        std::make_unique<SqliteCache<std::string>>(
            CommonUtils::CachePathUtils::get_sqlite_cache_path(
                "nontracked_positions_close_order_ids.db"));
    this->unacked_orders_failed_to_retrieve_cache =
        std::make_unique<SqliteCache<int>>(
            CommonUtils::CachePathUtils::get_sqlite_cache_path(
                "unacked_orders_failed_to_retrieve.db"));
    this->symbol_last_closed_trade_times_cache =
        std::make_unique<SqliteCache<long long>>(
            CommonUtils::CachePathUtils::get_sqlite_cache_path(
                "symbol_last_closed_tradess.db"));
    this->program_start_time_cache = std::make_unique<SqliteCache<long long>>(
        CommonUtils::CachePathUtils::get_sqlite_cache_path(
            "program_start_time.db"));
    this->heartbeat_cache = std::make_unique<SqliteCache<long long>>(
        CommonUtils::CachePathUtils::get_sqlite_cache_path(
            "heartbeat_cache.db"));
}

void Live::StateUpdater::init_nontracked_positions_close_times_cache() {
    this->nontracked_positions_close_times_cache =
        std::make_unique<SqliteCache<long long>>(
            CommonUtils::CachePathUtils::get_sqlite_cache_path(
                "nontracked_positions_close_times.db"));
}

void Live::StateUpdater::init_inventory_mismatches_sqlite_cache() {
    this->inventory_mismatches_sqlite_cache =
        std::make_unique<SqliteCache<long long>>(
            CommonUtils::CachePathUtils::get_sqlite_cache_path(
                "inventory_mismatches.db"));

    for (auto &[account_id, account] : accounts) {
        const auto &active_symbols = account.get_active_symbols();

        for (const auto &symbol : active_symbols) {
            const auto &key =
                Core::Utils::get_symbol_exchange_market_account_key(
                    symbol, account.exchange_name, account.market_type,
                    account_id);

            if (!this->inventory_mismatches_sqlite_cache->get(key)
                     .has_value()) {
                this->inventory_mismatches_sqlite_cache->insert(key, 0);
            }
        }
    }
}

void Live::StateUpdater::update_account_entries_nav_to_db() {
    static std::chrono::steady_clock::time_point last_update_time =
        std::chrono::steady_clock::now();

    if (std::chrono::steady_clock::now() - last_update_time <
        std::chrono::seconds(60 * 5)) {
        return;
    }
    last_update_time = std::chrono::steady_clock::now();

    auto &config = Live::Config::getInstance();

    for (const auto &[account_id, account] : this->accounts) {
        const auto &nav =
            Core::Engine::getInstance().get_nav_by_account_id(account_id);

        if (nav.has_value()) {
            std::vector<json> existing_records = PQUtils::fetch_from_postgres(
                config.get_db_connection_string(),
                ("SELECT * FROM account WHERE account_id = " +
                 std::to_string(account_id))
                    .c_str());

            if (!existing_records.empty()) {
                nlohmann::json update_record = nlohmann::json::object();
                update_record["nav"] = nav.value().toDouble();
                PQUtils::update_entry_by_field(
                    config.get_db_connection_string(), "account", update_record,
                    "account_id", std::to_string(account_id));
            }
        }
    }
}

void Live::StateUpdater::analytics_infra_misc_db_updates() {
    update_account_entries_nav_to_db();
}

void Live::StateUpdater::poll_stall_orders_watchdog() {
    const auto start_time = std::chrono::steady_clock::now();
    const auto max_duration = std::chrono::seconds(2);

    const auto stall_orders = this->stall_orders_sqlite_cache->query(
        "SELECT key, value FROM cache ORDER BY value asc");

    for (const auto &[key, value] : stall_orders) {
        if (std::chrono::steady_clock::now() - start_time >= max_duration) {
            break;
        }
        const auto &full_key_parts = Core::Utils::get_full_key_parts(key);

        if (!full_key_parts.exchange_name.has_value() ||
            !full_key_parts.market_type.has_value() ||
            !full_key_parts.account_id.has_value()) {
            continue;
        }
        const auto &account_key = Core::Utils::get_exchange_market_account_key(
            full_key_parts.exchange_name.value(),
            full_key_parts.market_type.value(),
            full_key_parts.account_id.value());

        auto it = last_stall_orders_poll_time_by_account_id.find(account_key);
        if (it == last_stall_orders_poll_time_by_account_id.end()) {
            last_stall_orders_poll_time_by_account_id[account_key] =
                std::chrono::steady_clock::now();
        }

        if (std::chrono::steady_clock::now() -
                last_stall_orders_poll_time_by_account_id[account_key] >=
            std::chrono::seconds(10)) {
            last_stall_orders_poll_time_by_account_id[account_key] =
                std::chrono::steady_clock::now();
        } else {
            continue;
        }

        if (full_key_parts.symbol.has_value()) {
            const auto &symbol = full_key_parts.symbol.value();
            const auto &exchange_name = full_key_parts.exchange_name.value();
            const auto &market_type = full_key_parts.market_type.value();
            const auto &account_id = full_key_parts.account_id.value();

            if (exchange_name == "polymarket") {
                continue;
            }

            if (market_type == "spot" &&
                !Core::ExchangeInfo::getInstance()
                     .is_symbol_allowing_margin_trade(symbol, exchange_name,
                                                      market_type)) {
                continue;
            }

            const auto &open_orders =
                CcxtProxy::instance().get_open_orders_on_symbol(
                    exchange_name, market_type, account_id, symbol);

            long long now = DatetimeUtils::get_current_time_ms();

            const auto &engine_orders =
                Core::Engine::getInstance().get_orders(symbol);

            for (const auto &order : open_orders) {
                if (order.timestamp + 60000 * 15 < now) {
                    bool found = false;
                    for (const auto &engine_order : engine_orders) {
                        if (Core::Utils::is_same_order_by_client_and_account(
                                engine_order, order.client_order_id,
                                account_id)) {
                            found = true;
                            break;
                        }

                        if (engine_order.is_alt_market_routed &&
                            engine_order.route_order_to_alt_symbol == symbol) {
                            found = true;
                            break;
                        }
                    }

                    if (!found) {
                        Core::Engine::getInstance().create_system_log(
                            symbol, Core::LOG_LEVEL_ERROR,
                            "poll_stall_orders_watchdog: order not "
                            "found, "
                            "canceling from exchange",
                            Core::TRADING_LOG_EVENT_TYPE_MISC_ORDER_CLEANUP,
                            nlohmann::json::object(), -1);

                        this->cancel_order_by_client_order_id(
                            order.get_order_key(exchange_name), exchange_name,
                            market_type,
                            CCXTUtils::symbol_to_ccxt_format(symbol,
                                                             market_type),
                            account_id);
                    }
                }
            }
        }
        this->stall_orders_sqlite_cache->insert(
            key, DatetimeUtils::get_current_time_ms());
    }
}

void Live::StateUpdater::update_strategy_virtual_balances_to_db(
    bool force_update) {
    if (!force_update) {
        if (std::chrono::steady_clock::now() -
                last_update_strategy_virtual_balances_to_db_time <
            std::chrono::seconds(5)) {
            return;
        }
    }
    last_update_strategy_virtual_balances_to_db_time =
        std::chrono::steady_clock::now();

    std::vector<std::pair<int, double>> balances;
    for (const auto &strategy : Core::Engine::getInstance().get_strategies()) {
        balances.emplace_back(strategy->strategy_config.id,
                              strategy->get_virtual_balance());
    }

    auto &config = Live::Config::getInstance();
    for (const auto &[strategy_id, virtual_balance] : balances) {
        const auto virtual_balance_it =
            this->strategy_virtual_balances.find(strategy_id);
        if (virtual_balance_it != this->strategy_virtual_balances.end()) {
            if (virtual_balance_it->second != virtual_balance) {
                this->strategy_virtual_balances[strategy_id] = virtual_balance;
                PQUtils::update_entry(
                    config.get_db_connection_string(), "strategy",
                    nlohmann::json::object(
                        {{"virtual_balance", virtual_balance}}),
                    strategy_id);
            }
        }
    }
}

bool Live::StateUpdater::is_shutdown_file_found() {
    return std::filesystem::exists("/tmp/live_infra_shutdown");
}

void Live::StateUpdater::check_core_dumps() {
    try {
        const std::string core_dump_dir = "/var/lib/core";
        bool core_dumps_exist = false;

        if (std::filesystem::exists(core_dump_dir)) {
            for (const auto &entry :
                 std::filesystem::directory_iterator(core_dump_dir)) {
                if (entry.is_regular_file()) {
                    core_dumps_exist = true;
                    break;
                }
            }
        }

        update_service_health_entry("trading_program", "core_dump_check",
                                    core_dumps_exist ? "error" : "ok",
                                    nlohmann::json::object());
    } catch (...) {
        update_service_health_entry("core_dumps", "core_dump_check", "error",
                                    nlohmann::json::object());
    }
}

void Live::StateUpdater::initialize_exchange_info() {
    for (const auto &exchange_and_market :
         MarketData::MARKET_DATA_EXCHANGES_OF_INTEREST) {
        if (exchange_and_market.exchange_name == "hyperliquid" &&
            Live::Config::getInstance().hyperliquid_futures_accounts.empty()) {
            continue;
        }
        Core::ExchangeInfo::getInstance()
            .update_symbols_for_exchange_and_market_type(
                exchange_and_market.exchange_name,
                exchange_and_market.market_type);
        const auto &exchange_info =
            Core::ExchangeInfo::getInstance().get_exchange_symbols_info(
                exchange_and_market.exchange_name,
                exchange_and_market.market_type);
        Core::Engine::getInstance().add_symbol_exchange_info(
            exchange_and_market.exchange_name, exchange_and_market.market_type,
            exchange_info);
    }
}

void Live::StateUpdater::poll_exchange_info_update() {
    if (std::chrono::steady_clock::now() - last_exchange_info_update_time >
        std::chrono::minutes(5)) {
        last_exchange_info_update_time = std::chrono::steady_clock::now();
        initialize_exchange_info();
    }
}

void Live::StateUpdater::poll_heartbeat_cache() {
    if (std::chrono::steady_clock::now() - last_heartbeat_update_time <
        std::chrono::minutes(
            Live::Constants::HEARTBEAT_UPDATE_INTERVAL_MINUTES)) {
        return;
    }
    last_heartbeat_update_time = std::chrono::steady_clock::now();
    this->heartbeat_cache->insert("heartbeat",
                                  DatetimeUtils::get_current_time_ms());
}

void Live::StateUpdater::poll_periodic_validate_inventory() {
    if (this->init_shutdown) {
        return;
    }
    if (validate_inventory_running.load()) {
        return;
    }
    if (std::chrono::steady_clock::now() -
            last_periodic_validate_inventory_time <
        std::chrono::minutes(10)) {
        return;
    }
    last_periodic_validate_inventory_time = std::chrono::steady_clock::now();
    validate_inventory_running.store(true);
    std::thread([this]() {
        validate_inventory_against_current_active_trades();
        validate_inventory_running.store(false);
    }).detach();
}

void Live::StateUpdater::poll_periodic_exchange_health_checks() {
    if (std::chrono::steady_clock::now() -
            last_periodic_exchange_health_checks_time <
        std::chrono::minutes(30)) {
        return;
    }
    last_periodic_exchange_health_checks_time =
        std::chrono::steady_clock::now();
    std::thread([&]() { run_extensive_exchange_health_checks(); }).detach();
}

void Live::StateUpdater::liveness_heartbeat_loop() {
    // Resolve the system-health watchdog file path once; ensure its dir exists.
    std::string health_path;
    if (const char *data_root = std::getenv("DATA_DIR_ROOT")) {
        health_path = std::string(data_root) + "/" +
                      Live::Constants::SYSTEM_HEALTH_FILE_RELPATH;
        std::error_code ec;
        std::filesystem::create_directories(
            std::filesystem::path(health_path).parent_path(), ec);
    }

    while (!this->init_shutdown) {
        std::this_thread::sleep_for(std::chrono::milliseconds(
            Live::Constants::LIVENESS_THREAD_INTERVAL_MS));

        long long now_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();

        // Liveness gate: keep the system "alive" (health file + venue
        // heartbeats) only while the main loop is actually ticking. If it is
        // wedged longer than the threshold, let the health file go stale and
        // stop heartbeating — external monitors then alert and the venue
        // cancels our open orders (correct dead-man behavior).
        long long last_tick =
            this->last_main_loop_tick_ms_.load(std::memory_order_relaxed);
        if (last_tick == 0 ||
            now_ms - last_tick > Live::Constants::MAIN_LOOP_LIVENESS_STALE_MS) {
            continue;
        }

        // 1) Multi-purpose system-health file (atomic timestamp write). A
        // separate process can rsync it and abort orders if it goes stale.
        if (!health_path.empty()) {
            std::string tmp = health_path + ".tmp";
            std::ofstream out(tmp, std::ios::trunc);
            if (out) {
                out << now_ms << "\n";
                out.close();
                std::error_code ec;
                std::filesystem::rename(tmp, health_path, ec);
            }
        }

        // 2) Venue session heartbeats — stop CLOB cancel-on-disconnect from
        // auto-cancelling resting orders, per configured polymarket account.
        // Per-request logging (code + rtt) is in
        // PolymarketClient::send_heartbeat.
        for (const auto &[account_id, _] :
             Live::Config::getInstance().polymarket_accounts) {
            CcxtProxy::instance().send_polymarket_heartbeat(account_id);
        }
    }
}

void Live::StateUpdater::poll_periodic_update_misc_orders() {
    if (std::chrono::steady_clock::now() -
            last_periodic_update_misc_orders_time <
        std::chrono::minutes(30)) {
        return;
    }
    last_periodic_update_misc_orders_time = std::chrono::steady_clock::now();
    std::thread([&]() { update_misc_orders(); }).detach();
}

void Live::StateUpdater::poll_symbol_exchange_info_to_db() {
    if (std::chrono::steady_clock::now() -
            last_symbol_exchange_info_db_write_time <
        std::chrono::minutes(5)) {
        return;
    }
    last_symbol_exchange_info_db_write_time = std::chrono::steady_clock::now();
    std::cout << "[poll_symbol_exchange_info_to_db] Starting..." << std::endl;

    auto &config = Live::Config::getInstance();
    auto symbol_infos = Core::Engine::getInstance().get_exchange_symbol_infos();
    std::cout << "[poll_symbol_exchange_info_to_db] Got " << symbol_infos.size()
              << " symbol_infos" << std::endl;

    std::unordered_map<std::string, nlohmann::json> by_exchange_market;
    for (const auto &[key, info] : symbol_infos) {
        size_t first_colon = key.find(':');
        if (first_colon == std::string::npos) continue;
        size_t second_colon = key.find(':', first_colon + 1);
        if (second_colon == std::string::npos) continue;
        std::string exchange_name =
            key.substr(first_colon + 1, second_colon - first_colon - 1);
        std::string market_type = key.substr(second_colon + 1);
        std::string exchange_market_key = exchange_name + "_" + market_type;
        by_exchange_market[exchange_market_key][key] = info.to_json();
    }
    std::cout << "[poll_symbol_exchange_info_to_db] Grouped into "
              << by_exchange_market.size() << " exchange_market groups"
              << std::endl;

    for (const auto &[exchange_market, json_data] : by_exchange_market) {
        size_t underscore = exchange_market.find('_');
        std::string exchange_name = exchange_market.substr(0, underscore);
        std::string market_type = exchange_market.substr(underscore + 1);

        std::vector<nlohmann::json> existing = PQUtils::fetch_from_postgres(
            config.get_db_connection_string(),
            ("SELECT * FROM symbol_exchange_info WHERE exchange_name = '" +
             exchange_name + "' AND market_type = '" + market_type + "'")
                .c_str());

        nlohmann::json db_record;
        db_record["exchange_name"] = exchange_name;
        db_record["market_type"] = market_type;
        std::string data_str = json_data.dump();
        db_record["data"] = data_str;
        db_record["updated_at"] =
            TimeUtils::ms_to_timestamptz(DatetimeUtils::get_current_time_ms());

        std::cout << "[poll_symbol_exchange_info_to_db] " << exchange_name
                  << "_" << market_type << ": data size = " << data_str.size()
                  << " bytes, existing records = " << existing.size()
                  << std::endl;

        bool success = false;
        if (existing.empty()) {
            success =
                PQUtils::write_to_postgres(config.get_db_connection_string(),
                                           "symbol_exchange_info", {db_record});
            std::cout << "[poll_symbol_exchange_info_to_db] INSERT "
                      << (success ? "succeeded" : "FAILED") << std::endl;
        } else {
            success = PQUtils::update_entry_by_field(
                config.get_db_connection_string(), "symbol_exchange_info",
                db_record, "id", existing[0]["id"].get<std::string>());
            std::cout << "[poll_symbol_exchange_info_to_db] UPDATE "
                      << (success ? "succeeded" : "FAILED") << std::endl;
        }
    }
    std::cout << "[poll_symbol_exchange_info_to_db] Done." << std::endl;
}

void Live::StateUpdater::poll_orderbook_full_refresh() {
    static auto last_poll_time = std::chrono::steady_clock::now();
    static std::unordered_map<std::string,
                              std::chrono::steady_clock::time_point>
        failed_symbols;

    auto now = std::chrono::steady_clock::now();
    if (now - last_poll_time < std::chrono::seconds(10)) {
        return;
    }
    last_poll_time = now;

    const auto candidate =
        Core::Engine::getInstance().get_oldest_synced_orderbook();
    if (!candidate.has_value()) {
        return;
    }

    const auto &sym = candidate.value();
    const auto key =
        sym.symbol + ":" + sym.exchange_name + ":" + sym.market_type;

    auto fail_it = failed_symbols.find(key);
    if (fail_it != failed_symbols.end()) {
        if (now - fail_it->second < std::chrono::minutes(5)) {
            return;
        }
    }

    const auto orderbook = CcxtProxy::instance().get_full_orderbook(
        sym.exchange_name, sym.market_type, sym.symbol);

    if (orderbook.has_value()) {
        MarketData::MarketDataStreams::get_instance()
            .enqueue_levels_only_refresh(sym.exchange_name, sym.market_type,
                                         sym.symbol, orderbook.value());
        failed_symbols.erase(key);

        const auto age_ms =
            Core::Engine::getInstance()
                .get_ms_since_last_orderbook_full_refresh(
                    sym.symbol, sym.exchange_name, sym.market_type);
        long long age_seconds = age_ms.value_or(0) / 1000;

        Core::Engine::getInstance().create_system_log(
            sym.symbol, Core::LOG_LEVEL_INFO,
            "Proactive orderbook levels refresh (last refresh " +
                std::to_string(age_seconds) + "s ago)",
            Core::TRADING_LOG_EVENT_TYPE_ORDERBOOK_RESYNCED,
            nlohmann::json::object(), -1, sym.exchange_name, sym.market_type);
    } else {
        failed_symbols[key] = now;
    }
}

void Live::StateUpdater::RunTradeLoop() {
    {
        if (is_prevent_trading_lock_created()) {
            LOG_ERROR(
                Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_SHUTDOWN_REQUESTED,
                "Prevent trading lock created, shutting down",
                nlohmann::json{});
            Core::Engine::getInstance().create_system_log(
                "", Core::LOG_LEVEL_ERROR,
                "Prevent trading lock created, shutting down",
                Core::TRADING_LOG_EVENT_TYPE_PREVENT_TRADING_LOCK_CREATED,
                nlohmann::json::object(), -1);
            return;
        }

        LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_STARTUP_COMPLETE,
                 "Starting trade loop", nlohmann::json{});
        static FileLock file_lock("/tmp/live_infra_state_lock");
        std::unique_ptr<FileLock::ScopedLock> lock;

        LinuxProcess::set_process_priority(
            0);  // not important process until file lock is acquired
        poll_fetch_prices();
        initialize_exchange_info();
        std::thread([&]() { create_new_trade_ticker_entries(); }).detach();

        LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_STARTUP_PHASE,
                 "Waiting for file lock", nlohmann::json{});
        while (!lock_acquired.load()) {
            lock = std::make_unique<FileLock::ScopedLock>(file_lock);
            if (lock->is_locked()) {
                LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_STARTUP_PHASE,
                         "Lock acquired starting trading loop",
                         nlohmann::json{});
                Core::Engine::getInstance().create_system_log(
                    "", Core::LOG_LEVEL_INFO,
                    "Lock acquired starting trading loop",
                    Core::TRADING_LOG_EVENT_TYPE_SYSTEM_STARTUP,
                    nlohmann::json::object(), -1);
                auto start_time_ms = Core::GlobalGeneralCache::get<long long>(
                    "program_start_time_ms");
                if (start_time_ms.has_value()) {
                    this->program_start_time_cache->insert(
                        "program_start_time", start_time_ms.value());
                    // Publish expected end time so cron-spawned standbys can
                    // decide whether to wait or defer to a fresher tick.
                    this->program_start_time_cache->insert(
                        "program_end_time",
                        start_time_ms.value() +
                            Live::Constants::PROGRAM_MAX_RUNTIME_MINUTES *
                                60LL * 1000);
                }
                lock_acquired.store(true);
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
            std::chrono::steady_clock::time_point now =
                std::chrono::steady_clock::now();
            if (now - program_start_time >
                std::chrono::minutes(
                    Live::Constants::STANDBY_LOCK_WAIT_TIMEOUT_MINUTES)) {
                LOG_ERROR(
                    Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_SHUTDOWN_REQUESTED,
                    "Timeout waiting for file lock, shutting down",
                    nlohmann::json{});
                std::_Exit(0);
            }
        }

        long long free_disk_space_mb = 0;
#ifdef __linux__
        free_disk_space_mb = DiskUtils::get_free_disk_space_mb("/");
        LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_STARTUP_PHASE,
                 "Free disk space: " + std::to_string(free_disk_space_mb),
                 nlohmann::json{});
        if (free_disk_space_mb < 500) {
            LOG_ERROR(
                Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_SHUTDOWN_REQUESTED,
                "Not enough disk space, shutting down", nlohmann::json{});
            Core::Engine::getInstance().create_system_log(
                "", Core::LOG_LEVEL_ERROR,
                "Not enough disk space, shutting down",
                Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_SHUTDOWN_REQUESTED,
                nlohmann::json::object(), -1);
            return;
        }
#endif

        if (this->is_shutdown_file_found()) {
            LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_SHUTDOWN_REQUESTED,
                     "Shutdown requested, shutting down", nlohmann::json{});
            std::_Exit(0);
        }

        if (!check_paper_mode_switches()) {
            return;
        }
        LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_STARTUP_PHASE,
                 "Recovering state from disk", nlohmann::json{});
        recover_state_from_disk();
        this->poll_discretionary_accounts(true);
        LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_STARTUP_PHASE,
                 "Setting state to ready", nlohmann::json{});
        Core::Engine::getInstance().state_is_ready();
        LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_STARTUP_PHASE,
                 "Starting state_updater loop", nlohmann::json{});

        std::thread([&]() { run_extensive_exchange_health_checks(); }).detach();

        std::chrono::steady_clock::time_point last_log_time =
            std::chrono::steady_clock::now();

        std::thread([&]() { update_misc_orders(); }).detach();

        while (std::chrono::steady_clock::now() - program_start_time <
               std::chrono::minutes(
                   Live::Constants::PROGRAM_MAX_RUNTIME_MINUTES)) {
            // Liveness beacon for the dedicated heartbeat thread.
            last_main_loop_tick_ms_.store(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count(),
                std::memory_order_relaxed);
            poll_order_state_watchdog();
            poll_paper_fills();
            poll_discretionary_accounts();
            poll_paper_nav();
            poll_strategy_rows();
            flush_strategy_command_reports();
            poll_fetch_prices();
            poll_inventory_state_watchdog();
            poll_sync_state_to_disk();
            poll_check_unacked_orders();
            poll_check_failed_cancels();
            poll_stall_orders_watchdog();
            poll_exchange_info_update();
            poll_symbol_exchange_info_to_db();
            poll_borrowable_inventories();
            poll_orderbook_full_refresh();
            poll_periodic_validate_inventory();
            poll_cancel_orphaned_orders();
            poll_periodic_exchange_health_checks();
            poll_periodic_update_misc_orders();
            update_strategy_virtual_balances_to_db();
            create_state_updater_running_log();
            analytics_infra_misc_db_updates();
            reconnect_streams();
            poll_heartbeat_cache();
            run_polymarket_tasks();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            if (is_shutdown_file_found()) {
                LOG_INFO(
                    Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_SHUTDOWN_REQUESTED,
                    "Shutdown file found, shutting down", nlohmann::json{});
                Core::Engine::getInstance().create_system_log(
                    "", Core::LOG_LEVEL_INFO,
                    "Shutdown file found, shutting "
                    "down",
                    Core::TRADING_LOG_EVENT_TYPE_SYSTEM_STARTUP,
                    nlohmann::json::object(), -1);
                break;
            }

            if (std::chrono::steady_clock::now() - last_log_time >
                std::chrono::seconds(60)) {
                last_log_time = std::chrono::steady_clock::now();
                LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_STATE_UPDATER_RUNNING,
                         "State updater loop running", nlohmann::json{});
            }
        }
        LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_SHUTDOWN_REQUESTED,
                 "Exited trade loop", nlohmann::json{});
        Core::Engine::getInstance().stop_trading();
        LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_SHUTDOWN_REQUESTED,
                 "Stopped trading", nlohmann::json{});
        // Clear the external sink before engine teardown so no log emit from
        // the shutdown path re-enters a half-down engine.
        DiskLogging::clear_external_sink();
        Core::Engine::getInstance().gracefully_stop();
        graceful_shutdown();
        LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_SHUTDOWN_REQUESTED,
                 "Setting process priority to 0", nlohmann::json{});
        LinuxProcess::set_process_priority(0);
        Core::Engine::getInstance().create_system_log(
            "", Core::LOG_LEVEL_INFO, "Shutting down and releasing lock",
            Core::TRADING_LOG_EVENT_TYPE_SYSTEM_STARTUP,
            nlohmann::json::object(), -1);
        LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_SHUTDOWN_REQUESTED,
                 "Shutting down and releasing lock", nlohmann::json{});
        lock.reset();
    }
    std::this_thread::sleep_for(std::chrono::seconds(30));
    std::_Exit(0);
}
