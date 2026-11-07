#include <chrono>
#include <filesystem>
#include <fstream>

#include "common_types/log_event_types.h"
#include "disk_logging/log.h"
#include "live_infra/config.h"
#include "live_infra/state_updater.h"
#include "market_making/engine.h"
#include "market_making/ticker.h"
#include "postgre_utils.h"

namespace {

constexpr auto kStrategySyncInterval = std::chrono::seconds(10);
constexpr auto kQuiesceTimeout = std::chrono::milliseconds(1000);

// Engine-written columns are left out, or every poll would see a change.
// Prediction-market rows get a constant: their tickers are rewritten while
// trading and their changes apply only on restart.
constexpr const char *kRowStatesQuery = R"SQL(
SELECT -1 AS id, '' AS fingerprint, '{}' AS columns
UNION ALL
SELECT s.id,
       CASE WHEN s.is_trading_on_prediction_market THEN 'prediction-market'
       ELSE md5((to_jsonb(s) - 'virtual_balance' - 'updated_at')::text
           || coalesce((SELECT jsonb_agg(to_jsonb(t) - 'prediction'
                                         - 'processed_kline_open_time_ms'
                                         - 'active_trade_id' - 'updated_at'
                                         ORDER BY t.id)::text
                        FROM trade_ticker t WHERE t.strategy_id = s.id), '')
           || coalesce((SELECT jsonb_agg(m.model_id ORDER BY m.model_id)::text
                        FROM strategy_model m WHERE m.strategy_id = s.id), ''))
       END,
       (to_jsonb(s) - 'virtual_balance' - 'updated_at')::text
FROM strategy s WHERE s.version = '2.0'
)SQL";

bool is_flat(Core::TradeTicker &ticker) {
    return ticker.get_orders().empty() && ticker.get_trades().empty();
}

// Fields a LiveTradeTicker takes at construction.
bool same_ticker_config(const StrategyConfig &a, const StrategyConfig &b) {
    return a.exchanges_list == b.exchanges_list &&
           a.num_max_orders_10_sec == b.num_max_orders_10_sec &&
           a.num_max_orders_1_min == b.num_max_orders_1_min &&
           a.num_max_orders_1_hour == b.num_max_orders_1_hour &&
           a.account_mode == b.account_mode &&
           a.is_market_making_strategy == b.is_market_making_strategy;
}

bool same_models(const StrategyConfig &a, const StrategyConfig &b) {
    if (a.model_schedule_path != b.model_schedule_path ||
        a.strategy_models.size() != b.strategy_models.size()) {
        return false;
    }
    for (size_t i = 0; i < a.strategy_models.size(); ++i) {
        if (a.strategy_models[i].path_to_model !=
            b.strategy_models[i].path_to_model) {
            return false;
        }
    }
    return true;
}

const ::TradeTicker *find_row_ticker(const StrategyConfig &row,
                                     const std::string &symbol) {
    for (const auto &ticker : row.trade_tickers) {
        if (ticker.symbol == symbol) return &ticker;
    }
    return nullptr;
}

nlohmann::json changed_columns(const nlohmann::json &before,
                               const nlohmann::json &after) {
    nlohmann::json changed = nlohmann::json::array();
    for (const auto &[key, value] : after.items()) {
        if (!before.contains(key) || before[key] != value) changed.push_back(key);
    }
    return changed;
}

// The pointer stays valid: only this thread changes the list, and replaced
// strategies are retired, never destroyed.
Core::Strategy *find_engine_strategy(int id) {
    for (auto &strategy : Core::Engine::getInstance().get_strategies()) {
        if (strategy->strategy_config.id == id) return strategy.get();
    }
    return nullptr;
}

}  // namespace

std::vector<StrategyConfig> Live::StateUpdater::fed_strategy_rows() const {
    std::lock_guard lock(this->fed_strategies_mutex);
    std::vector<StrategyConfig> rows;
    rows.reserve(this->fed_strategies.size());
    for (const auto &[id, fed] : this->fed_strategies) rows.push_back(fed.row);
    return rows;
}

void Live::StateUpdater::feed_strategy(const StrategyConfig &row,
                                       const StrategyRowState &state) {
    std::lock_guard lock(this->fed_strategies_mutex);
    this->fed_strategies[row.id] = {state.fingerprint, state.columns, row};
}

std::optional<std::unordered_map<int, Live::StateUpdater::StrategyRowState>>
Live::StateUpdater::fetch_strategy_row_states() {
    const auto rows = PQUtils::fetch_from_postgres(
        Live::Config::getInstance().get_db_connection_string(),
        kRowStatesQuery);
    // fetch_from_postgres returns nothing on any failure; the sentinel row
    // tells that apart from a table with no strategies.
    if (rows.empty()) return std::nullopt;
    std::unordered_map<int, StrategyRowState> states;
    for (const auto &row : rows) {
        const int id = std::stoi(row["id"].get<std::string>());
        if (id < 0) continue;
        states[id] = {row["fingerprint"].get<std::string>(),
                      nlohmann::json::parse(row["columns"].get<std::string>(),
                                            nullptr, false)};
    }
    return states;
}

void Live::StateUpdater::report_strategy_pending(
    int id, const std::string &fingerprint, const std::string &message,
    const nlohmann::json &metadata, bool skip) {
    if (skip) this->skipped_fingerprints[id] = fingerprint;
    auto &reported = this->reported_pending_fingerprints[id];
    if (reported == fingerprint) return;
    reported = fingerprint;
    Core::Engine::getInstance().create_system_log(
        "", Core::LOG_LEVEL_WARNING, message,
        Core::TRADING_LOG_EVENT_TYPE_STRATEGY_ACTIVATED, metadata, id);
}

void Live::StateUpdater::record_paper_mode(int id, bool is_paper) {
    const auto path = paper_strategy_modes_path();
    nlohmann::json modes = nlohmann::json::object();
    if (std::filesystem::exists(path)) {
        std::ifstream file(path);
        modes = nlohmann::json::parse(file, nullptr, false);
        if (!modes.is_object()) modes = nlohmann::json::object();
    }
    modes[std::to_string(id)] = is_paper;
    std::ofstream(path) << modes.dump(1);
}

void Live::StateUpdater::poll_strategy_rows() {
    if (std::chrono::steady_clock::now() - last_strategy_rows_poll_time <
        kStrategySyncInterval) {
        return;
    }
    last_strategy_rows_poll_time = std::chrono::steady_clock::now();

    const auto states = this->fetch_strategy_row_states();
    if (!states) return;

    std::vector<int> removed;
    std::vector<int> changed;
    {
        std::lock_guard lock(this->fed_strategies_mutex);
        for (const auto &[id, fed] : this->fed_strategies) {
            if (!states->contains(id)) removed.push_back(id);
        }
        for (const auto &[id, state] : *states) {
            const auto fed = this->fed_strategies.find(id);
            if (fed != this->fed_strategies.end() &&
                fed->second.fingerprint == state.fingerprint) {
                continue;
            }
            const auto skipped = this->skipped_fingerprints.find(id);
            if (skipped != this->skipped_fingerprints.end() &&
                skipped->second == state.fingerprint) {
                continue;
            }
            changed.push_back(id);
        }
    }
    for (const int id : removed) this->sync_removed_strategy(id);
    for (const int id : changed) this->sync_strategy_row(id, states->at(id));
}

void Live::StateUpdater::sync_strategy_row(int id,
                                           const StrategyRowState &state) {
    auto &engine = Core::Engine::getInstance();
    const auto rows = PQUtils::fetch_strategies(
        Live::Config::getInstance().get_db_connection_string(), "2.0", id);
    if (rows.empty()) return;
    const StrategyConfig &row = rows.front();
    const auto restart_only = [&](const std::string &reason) {
        this->report_strategy_pending(
            id, state.fingerprint,
            "strategy row change applies on the next engine restart",
            {{"reason", reason}}, true);
    };

    Core::Strategy *current = find_engine_strategy(id);
    std::optional<FedStrategy> fed;
    {
        std::lock_guard lock(this->fed_strategies_mutex);
        const auto it = this->fed_strategies.find(id);
        if (it != this->fed_strategies.end()) fed = it->second;
    }
    const StrategyConfig &previous = current ? current->strategy_config : row;

    if (row.is_trading_on_prediction_market ||
        previous.is_trading_on_prediction_market) {
        return restart_only("prediction-market strategy");
    }
    if (!same_models(previous, row) ||
        (!current && (!row.strategy_models.empty() ||
                      !row.model_schedule_path.empty()))) {
        return restart_only("models changed");
    }
    if (current) {
        // A queued order of the old object could otherwise go out under the
        // new mode; the startup check guards the switch instead.
        if (row.is_paper_trading != previous.is_paper_trading) {
            return restart_only("paper/live switch");
        }
        if (row.strategy_name != previous.strategy_name) {
            return restart_only("strategy_name changed");
        }
        if (Core::Strategy::registers_own_state_machine(row.strategy_name)) {
            return restart_only("strategy owns a state machine");
        }
    }

    // A ticker whose construction-time config changed, or whose symbol left
    // the row, is replaced or dropped, so it must hold nothing.
    std::vector<std::string> shared_symbols;
    std::vector<std::string> released_symbols;
    if (current) {
        const bool same_config = same_ticker_config(previous, row);
        for (const auto &[symbol, ticker] : current->trade_tickers) {
            const auto *before = find_row_ticker(previous, symbol);
            const auto *after = find_row_ticker(row, symbol);
            const bool keep = same_config && before && after &&
                              before->base_asset == after->base_asset &&
                              before->quote_asset == after->quote_asset;
            (keep ? shared_symbols : released_symbols).push_back(symbol);
        }
    }
    const auto busy_symbols = [&] {
        nlohmann::json busy = nlohmann::json::array();
        for (const auto &symbol : released_symbols) {
            if (!is_flat(*current->trade_tickers.at(symbol))) {
                busy.push_back(symbol);
            }
        }
        return busy;
    };
    if (const auto busy = busy_symbols(); !busy.empty()) {
        this->report_strategy_pending(
            id, state.fingerprint,
            "strategy row change waits until these symbols are flat",
            {{"symbols", busy}}, false);
        return;
    }

    nlohmann::json metadata;
    std::unique_ptr<Core::Strategy> next;
    try {
        next = this->build_strategy(row, metadata);
    } catch (const SymbolInfoUnavailable &e) {
        this->report_strategy_pending(id, state.fingerprint,
                                      "strategy waits for venue symbol info",
                                      {{"error", e.what()}}, false);
        return;
    } catch (const std::exception &e) {
        this->report_strategy_pending(id, state.fingerprint,
                                      "strategy row failed to build",
                                      {{"error", e.what()}}, true);
        return;
    }
    if (!next) {
        this->feed_strategy(row, state);
        return;
    }

    if (!current) {
        // Queued before the add, so they run ahead of any new command.
        this->resume_open_strategy_commands({id});
        const auto result = engine.add_strategy_while_trading(next);
        if (result == Core::Engine::SyncResult::BUSY) return;
        if (result != Core::Engine::SyncResult::APPLIED) {
            return restart_only("strategy has models");
        }
    } else {
        const bool was_disabled = current->is_disabled_now();
        if (!engine.quiesce_strategy(*current, kQuiesceTimeout)) return;
        // Queued orders of the last pass may have reached a released ticker.
        if (const auto busy = busy_symbols(); !busy.empty()) {
            current->set_disabled(was_disabled);
            return;
        }
        next->adopt_runtime_state(*current, shared_symbols);
        if (engine.replace_strategy(next) !=
            Core::Engine::SyncResult::APPLIED) {
            current->set_disabled(was_disabled);
            return;
        }
        metadata["changed_columns"] =
            changed_columns(fed ? fed->columns : nlohmann::json::object(),
                            state.columns);
        metadata["released_symbols"] = released_symbols;
    }

    this->feed_strategy(row, state);
    this->reported_pending_fingerprints.erase(id);
    this->skipped_fingerprints.erase(id);
    this->record_paper_mode(id, row.is_paper_trading);
    if (row.is_paper_trading) this->write_paper_account_records({row});
    metadata["applied_while_trading"] = true;
    engine.create_system_log(
        "", Core::LOG_LEVEL_INFO,
        current ? "strategy updated" : "strategy activated",
        Core::TRADING_LOG_EVENT_TYPE_STRATEGY_ACTIVATED, metadata, id);
}

void Live::StateUpdater::sync_removed_strategy(int id) {
    auto &engine = Core::Engine::getInstance();
    Core::Strategy *current = find_engine_strategy(id);
    if (current) {
        current->set_disabled(true);
        {
            // A row that comes back must count as changed.
            std::lock_guard lock(this->fed_strategies_mutex);
            this->fed_strategies[id].fingerprint.clear();
        }
        nlohmann::json busy = nlohmann::json::array();
        for (const auto &[symbol, ticker] : current->trade_tickers) {
            if (!is_flat(*ticker)) busy.push_back(symbol);
        }
        if (!busy.empty()) {
            this->report_strategy_pending(
                id, "removed",
                "strategy row removed; disabled, leaves the engine once flat",
                {{"symbols", busy}}, false);
            return;
        }
        if (!engine.quiesce_strategy(*current, kQuiesceTimeout) ||
            engine.remove_strategy(id) != Core::Engine::SyncResult::APPLIED) {
            return;
        }
    }
    {
        std::lock_guard lock(this->fed_strategies_mutex);
        this->fed_strategies.erase(id);
    }
    this->reported_pending_fingerprints.erase(id);
    this->skipped_fingerprints.erase(id);
    if (current) {
        engine.create_system_log(
            "", Core::LOG_LEVEL_INFO, "strategy removed",
            Core::TRADING_LOG_EVENT_TYPE_STRATEGY_ACTIVATED,
            nlohmann::json::object(), id);
    }
}
