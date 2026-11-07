#pragma once

#include <cmath>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace Core {

struct DiscretionaryAccount {
    long long id = 0;
    bool paper = true;
    bool enabled = false;
    std::string exchange;
    std::string market;
    double capital = 0;
    double max_exposure = 1;
    double max_order_notional = 100000;
    int rate_10s = 20;
    int rate_1m = 120;
    int rate_1h = 2000;
};

class DiscretionaryAccounts {
   public:
    static DiscretionaryAccounts& instance() {
        static DiscretionaryAccounts accounts;
        return accounts;
    }

    void configure(const std::vector<DiscretionaryAccount>& accounts, bool expiring = true) {
        std::lock_guard lock(mutex_);
        expiring_ = expiring;
        last_refresh_ = std::chrono::steady_clock::now();
        accounts_.clear();
        for (const auto& account : accounts) {
            if (!std::isfinite(account.capital) || !std::isfinite(account.max_exposure) ||
                account.max_exposure <= 0 || !std::isfinite(account.max_order_notional) ||
                account.max_order_notional <= 0 || account.rate_10s <= 0 ||
                account.rate_1m <= 0 || account.rate_1h <= 0) {
                throw std::invalid_argument("Invalid discretionary account configuration");
            }
            accounts_[account.id] = account;
        }
    }

    std::optional<DiscretionaryAccount> get(long long id) const {
        std::lock_guard lock(mutex_);
        const auto found = accounts_.find(id);
        if (found == accounts_.end()) return std::nullopt;
        auto account = found->second;
        if (expiring_ && std::chrono::steady_clock::now() - last_refresh_ > std::chrono::seconds(15)) account.enabled = false;
        return account;
    }

    void settle(unsigned long long trade_id, long long account_id, double pnl, unsigned long long signal_id = 0) {
        if (!std::isfinite(pnl)) throw std::invalid_argument("Nonfinite paper settlement");
        std::lock_guard lock(mutex_);
        pending_[std::to_string(trade_id)] = {{"account_id", account_id}, {"pnl", pnl}, {"signal_id", signal_id}};
    }

    nlohmann::json pending() const {
        std::lock_guard lock(mutex_);
        return pending_;
    }

    void restore(const nlohmann::json& pending) {
        if (!pending.is_object()) throw std::invalid_argument("Invalid paper settlement checkpoint");
        std::lock_guard lock(mutex_);
        for (const auto& [id, value] : pending.items()) {
            if (id.empty() || id.size()>20 || id.find_first_not_of("0123456789")!=std::string::npos) throw std::invalid_argument("Invalid settlement trade ID");
            std::stoull(id);
            value.at("account_id").get<long long>();
            if (!std::isfinite(value.at("pnl").get<double>())) throw std::invalid_argument("Nonfinite settlement checkpoint");
            pending_[id] = {{"account_id",value.at("account_id").get<long long>()},
                            {"pnl",value.at("pnl").get<double>()},
                            {"signal_id",value.value("signal_id",0ULL)}};
        }
    }

    void acknowledge(const nlohmann::json& saved) {
        std::lock_guard lock(mutex_);
        for (const auto& [id, value] : saved.items()) {
            if (pending_.contains(id) && pending_[id] == value) pending_.erase(id);
        }
    }

    bool reserve_order(long long id, long long now_ms) {
        std::lock_guard lock(mutex_);
        const auto found = accounts_.find(id);
        if (found == accounts_.end()) return false;
        auto& times = order_times_[id];
        while (!times.empty() && times.front() <= now_ms - 3600000) times.pop_front();
        int count_10s = 0, count_1m = 0;
        for (const auto timestamp : times) {
            count_10s += timestamp > now_ms - 10000;
            count_1m += timestamp > now_ms - 60000;
        }
        if (count_10s >= found->second.rate_10s || count_1m >= found->second.rate_1m ||
            times.size() >= static_cast<size_t>(found->second.rate_1h)) return false;
        times.push_back(now_ms);
        return true;
    }

   private:
    mutable std::mutex mutex_;
    std::chrono::steady_clock::time_point last_refresh_{};
    bool expiring_ = true;
    std::map<long long, DiscretionaryAccount> accounts_;
    std::map<long long, std::deque<long long>> order_times_;
    nlohmann::json pending_ = nlohmann::json::object();
};
}
