#pragma once

#include <algorithm>
#include <deque>
#include <fstream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "nlohmann/json.hpp"

namespace Core {

inline constexpr const char* CRYPTO_DISCRETIONARY_STRATEGY = "crypto-discretionary";

inline bool accepts_strategy_commands(const std::string& strategy_name) {
    if (strategy_name == CRYPTO_DISCRETIONARY_STRATEGY ||
        strategy_name == "crypto-discretionary-paper" ||
        strategy_name == "crypto-discretionary-realmoney") return true;
    const std::string prefix = std::string(CRYPTO_DISCRETIONARY_STRATEGY) + ":";
    if (!strategy_name.starts_with(prefix)) return false;
    const auto suffix = strategy_name.substr(prefix.size());
    return !suffix.empty() && suffix.front() != '0' &&
           std::all_of(suffix.begin(), suffix.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// One operator command: live, an engine_signal row (`id` is its id); in a
// backtest, a line of the replay file.
struct StrategyCommand {
    long long id = 0;
    int strategy_id = 0;
    std::string op;
    nlohmann::json payload = nlohmann::json::object();
    long long ts_ms = 0;
    // Still open when the engine last stopped; the strategy picks up what is
    // left of it instead of starting over.
    bool resumed = false;
};

inline constexpr const char* STRATEGY_COMMAND_WORKING = "working";
inline constexpr const char* STRATEGY_COMMAND_DONE = "done";
inline constexpr const char* STRATEGY_COMMAND_FAILED = "failed";
inline constexpr const char* STRATEGY_COMMAND_CANCELLED = "cancelled";
inline constexpr const char* STRATEGY_COMMAND_INTERRUPTED = "interrupted";

struct StrategyCommandReport {
    long long command_id = 0;
    int strategy_id = 0;
    std::string status;
    std::string detail;
};

// An order a command sent; its fills in order_fill are what the command did.
struct StrategyCommandOrder {
    long long command_id = 0;
    unsigned long long order_id = 0;
};

// Commands wait here per strategy until its own callback drains them, so
// orders are built on the strategy's thread under the normal locks. Status
// reports go the other way, to whoever persists them.
class StrategyCommandInbox {
   public:
    static StrategyCommandInbox& instance() {
        static StrategyCommandInbox inbox;
        return inbox;
    }

    void push(StrategyCommand command) {
        std::lock_guard<std::mutex> lock(mutex_);
        queues_[command.strategy_id].push_back(std::move(command));
    }

    std::vector<StrategyCommand> drain(int strategy_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = queues_.find(strategy_id);
        if (it == queues_.end()) return {};
        std::vector<StrategyCommand> commands(
            std::make_move_iterator(it->second.begin()),
            std::make_move_iterator(it->second.end()));
        it->second.clear();
        return commands;
    }

    // Reports are dropped unless enabled; a backtest has no one to read them.
    void set_reporting_enabled(bool enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        reporting_enabled_ = enabled;
    }

    void report(StrategyCommandReport report) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (reporting_enabled_) reports_.push_back(std::move(report));
    }

    std::vector<StrategyCommandReport> take_reports() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<StrategyCommandReport> reports;
        reports.swap(reports_);
        return reports;
    }

    // Puts back reports that could not be persisted, ahead of newer ones.
    void restore_reports(std::vector<StrategyCommandReport> reports) {
        std::lock_guard<std::mutex> lock(mutex_);
        reports.insert(reports.end(), std::make_move_iterator(reports_.begin()),
                       std::make_move_iterator(reports_.end()));
        reports_.swap(reports);
    }

    void link_order(StrategyCommandOrder link) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (reporting_enabled_) order_links_.push_back(link);
    }

    std::vector<StrategyCommandOrder> take_order_links() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<StrategyCommandOrder> links;
        links.swap(order_links_);
        return links;
    }

    void restore_order_links(std::vector<StrategyCommandOrder> links) {
        std::lock_guard<std::mutex> lock(mutex_);
        links.insert(links.end(), order_links_.begin(), order_links_.end());
        order_links_.swap(links);
    }

    // Latest book (positions, orders, jobs) of each strategy; a persister
    // takes only the ones that changed.
    void publish_book(int strategy_id, nlohmann::json book) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!reporting_enabled_) return;
        auto& slot = books_[strategy_id];
        if (slot.first == book) return;
        slot = {std::move(book), true};
    }
    std::vector<std::pair<int, nlohmann::json>> take_changed_books() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::pair<int, nlohmann::json>> changed;
        for (auto& [id, slot] : books_) {
            if (!slot.second) continue;
            changed.emplace_back(id, slot.first);
            slot.second = false;
        }
        return changed;
    }
    void mark_books_unsaved(const std::vector<std::pair<int, nlohmann::json>>& books) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [id, _] : books) books_[id].second = true;
    }

    // JSONL of {ts_ms, strategy_id, op, ...}; the remaining fields are the
    // payload. Command ids are line numbers. Throws on a malformed line.
    void load_replay_file(const std::string& path) {
        std::ifstream file(path);
        if (!file) throw std::runtime_error("cannot open " + path);
        std::vector<StrategyCommand> commands;
        std::string line;
        long long line_number = 0;
        while (std::getline(file, line)) {
            ++line_number;
            if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
            const auto json = nlohmann::json::parse(line, nullptr, false);
            if (!json.is_object() || !json.contains("ts_ms") ||
                !json.contains("strategy_id") || !json.contains("op")) {
                throw std::runtime_error(path + ":" +
                                         std::to_string(line_number) +
                                         ": needs ts_ms, strategy_id and op");
            }
            StrategyCommand command;
            command.id = line_number;
            command.ts_ms = json.at("ts_ms").get<long long>();
            command.strategy_id = json.at("strategy_id").get<int>();
            command.op = json.at("op").get<std::string>();
            command.payload = json;
            command.payload.erase("ts_ms");
            commands.push_back(std::move(command));
        }
        std::stable_sort(commands.begin(), commands.end(),
                         [](const auto& a, const auto& b) {
                             return a.ts_ms < b.ts_ms;
                         });
        std::lock_guard<std::mutex> lock(mutex_);
        replay_.assign(std::make_move_iterator(commands.begin()),
                       std::make_move_iterator(commands.end()));
    }

    // Moves replay commands due by `now_ms` into their strategies' queues.
    void release_due(long long now_ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!replay_.empty() && replay_.front().ts_ms <= now_ms) {
            auto command = std::move(replay_.front());
            replay_.pop_front();
            queues_[command.strategy_id].push_back(std::move(command));
        }
    }

   private:
    std::mutex mutex_;
    std::unordered_map<int, std::deque<StrategyCommand>> queues_;
    std::deque<StrategyCommand> replay_;
    std::vector<StrategyCommandReport> reports_;
    std::vector<StrategyCommandOrder> order_links_;
    std::unordered_map<int, std::pair<nlohmann::json, bool>> books_;
    bool reporting_enabled_ = false;
};

}  // namespace Core
