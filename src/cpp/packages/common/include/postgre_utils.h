#pragma once

#include <libpq-fe.h>

#include <optional>
#include <vector>

#include "common_types/active_trade.h"
#include "common_types/strategy.h"
#include "common_types/trade_ticker.h"
#include "common_types/trade_types.h"
#include "common_types/trading_log.h"
#include "db_types/dataset.h"
#include "market_making/dtypes/engine_state_snapshot.h"
#include "nlohmann/json.hpp"

namespace PQUtils {

static const std::string TRADE_TICKER_TABLE = "trade_ticker";
std::optional<std::string> fetch_limit_order_filled_qty_by_id(
    const char* conninfo, long long order_id, int active_trade_id);
std::vector<json> fetch_from_postgres(const char* conninfo, const char* query);
// A strategy row with its trade_ticker and model rows, as the JSON that
// StrategyConfig::from_json reads. Throws on a format other than 3.0.
json strategy_rows_to_config(const json& strategy_row,
                             const std::vector<json>& ticker_rows,
                             const std::vector<json>& model_rows);
std::vector<json> fetch_strategy_configs(
    const char* conninfo, const std::string& version,
    std::optional<int> strategy_id = std::nullopt);
std::vector<StrategyConfig> fetch_strategies(
    const char* conninfo, const std::string& version,
    std::optional<int> strategy_id = std::nullopt);
std::vector<Db::Dataset> fetch_datasets(const char* conninfo,
                                        bool fetch_only_ohlcv);

bool write_to_postgres(const char* conninfo, const std::string& table_name,
                       const std::vector<json>& entries,
                       bool print_errors = true);

bool delete_by_id(const char* conninfo, const std::string& table_name, int id);

bool batch_update_entries(const char* conninfo, const std::string& table_name,
                          const std::vector<json>& entries,
                          const std::string& id_column);


std::optional<long> create_entry_return_id(const char* conninfo,
                                           const std::string& table_name,
                                           const json& entry,
                                           const std::string& id_column);

void update_trade_tickers_processed_kline(
    const char* conninfo, const std::vector<std::pair<int, double>>& ticker_ids,
    long long processed_kline);


long long write_to_postgres_get_id(const char* conninfo,
                                   const std::string& table_name,
                                   const json& entry);

bool delete_multiple_by_id(const char* conninfo, const std::string& table_name,
                           const std::vector<int>& ids);
bool update_entry(const char* conninfo, const std::string& table_name,
                  const json& updates, int id);

bool update_entry_by_field(const char* conninfo, const std::string& table_name,
                           const json& updates, const std::string& field_name,
                           const std::string& field_value);



bool write_trading_log(const char* conninfo, const TradingLog& log);
bool write_trading_logs(const char* conninfo,
                        const std::vector<TradingLog>& logs,
                        long long program_start_time_ms);
bool write_engine_state_snapshot(const char* conninfo,
                                 const Core::EngineStateSnapshot& snapshot);

}  // namespace PQUtils
