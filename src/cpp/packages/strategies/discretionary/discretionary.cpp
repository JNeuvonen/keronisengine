#include "market_making/strategy_registry.h"
#include "strategies/discretionary/discretionary.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>

#include "market_data/bar_types_constant.h"
#include "market_making/strategy_commands.h"
#include "market_making/discretionary_accounts.h"
#include "market_making/ticker.h"
#include "strategies/common/order_posting_policy.h"
#include "strategies/discretionary/commands.h"
#include "strategies/discretionary/execution.h"

namespace AlgorithmDiscretionary {
namespace {

using namespace Discretionary;
using OrderPosting::OrderPostingPolicy;
using Kv = std::unordered_map<std::string, double>;

constexpr long long kQuoteMaxAgeMs = 60000;
constexpr long long kPassMinIntervalMs = 100;
constexpr long long kBacktestBarMs = 60000;
constexpr std::uint64_t kClipLifeSalt = 23;
// The engine never expires orders; ours without a ttl rest until cancelled.
constexpr long long kNeverExpiresMs = 10LL * 365 * 86400000;
constexpr double kQtyEpsilon = 1e-12;
constexpr int kMaxTakeProfitFailures = 3;
constexpr const char* kStoppingRefusal = "the stop has triggered; close_trade closes the rest";

// A trade's kv_cache is its state: metadata, take-profit and stop policies,
// the stop-out marker and the running execution. The engine persists it with
// the trade. Entry orders carry the whole trade kv so a first fill seeds the
// trade; later entry fills leave it alone. Orders also carry tags naming what
// sent them. Engine ids do not fit a double and never go in.
constexpr const char* kSignal = "disc_signal";
constexpr const char* kCmd = "disc_cmd";
constexpr const char* kRole = "disc_role";
constexpr const char* kLevel = "disc_level";
constexpr const char* kSetup = "disc_setup";
constexpr const char* kEv = "disc_ev_pct";
constexpr const char* kTp = "disc_tp";
constexpr const char* kTpRungs = "disc_tp_rungs";
constexpr const char* kTpStepBp = "disc_tp_step_bp";
constexpr const char* kTpSizeJitter = "disc_tp_size_jitter";
// The first rung not yet posted or skipped; the rungs before it are done.
constexpr const char* kTpNext = "disc_tp_next";
// Written by the ladder before rungs; read for trades set then.
constexpr const char* kTpLegacyLevels = "disc_tp_levels";
constexpr const char* kTpGapLoMs = "disc_tp_gap_lo_ms";
constexpr const char* kTpGapHiMs = "disc_tp_gap_hi_ms";
constexpr const char* kTpLastPostMs = "disc_tp_last_post_ms";
constexpr const char* kStop = "disc_stop";
constexpr const char* kStopEveryMs = "disc_stop_every_ms";
constexpr const char* kStopEveryJitter = "disc_stop_every_jitter";
constexpr const char* kStopClipUsd = "disc_stop_clip_usd";
constexpr const char* kStopClipJitter = "disc_stop_clip_jitter";
constexpr const char* kStopPassive = "disc_stop_passive";
constexpr const char* kStopCrossBps = "disc_stop_cross_bps";
constexpr const char* kStopLifeLoMs = "disc_stop_life_lo_ms";
constexpr const char* kStopLifeHiMs = "disc_stop_life_hi_ms";
// Written by the one-order stop before clips; read for trades set then.
constexpr const char* kStopLegacyCapBps = "disc_stop_cap_bps";
constexpr const char* kStopLegacyClipLo = "disc_stop_clip_lo";
constexpr const char* kStopLegacyClipHi = "disc_stop_clip_hi";
constexpr const char* kStopLegacyGapLoMs = "disc_stop_gap_lo_ms";
constexpr const char* kStopLegacyGapHiMs = "disc_stop_gap_hi_ms";
constexpr const char* kStopLegacyEscalateMs = "disc_stop_escalate_ms";
// Written by the deadline-paced stop; erased on the next write.
constexpr const char* kStopLegacyDeadlineMs = "disc_stop_deadline_ms";
// A stop stored without a clip size: clips of its trigger quantity over this many.
constexpr int kLegacyStopClips = 20;
// The trigger: when, and the quantity open then. The last clip posted or skipped.
constexpr const char* kStopHitMs = "disc_stop_hit_ms";
constexpr const char* kStopStartQty = "disc_stop_start_qty";
constexpr const char* kStopClip = "disc_stop_clip";
constexpr const char* kStopLastPostMs = "disc_stop_last_post_ms";
constexpr const char* kJobCmd = "disc_job_cmd";
constexpr const char* kJobTarget = "disc_job_target";
constexpr const char* kJobTargetUsd = "disc_job_target_usd";
constexpr const char* kJobArrival = "disc_job_arrival";
constexpr const char* kJobTaking = "disc_job_taking";
constexpr const char* kJobClip = "disc_job_clip";

enum class Role { TAKE_PROFIT = 1, STOP = 2, EXECUTION = 3, RESTING = 4 };

struct Refused : std::runtime_error {
    using std::runtime_error::runtime_error;
};

std::optional<double> read(const Kv& kv, const char* key) {
    const auto it = kv.find(key);
    if (it == kv.end()) return std::nullopt;
    return it->second;
}

long long read_ms(const Kv& kv, const char* key) {
    return static_cast<long long>(read(kv, key).value_or(0));
}

// Writing a take-profit lays its rungs out afresh from the first.
void write_tp(Kv& kv, const std::optional<TakeProfitPolicy>& tp) {
    for (const char* key : {kTp, kTpRungs, kTpStepBp, kTpSizeJitter, kTpNext, kTpLegacyLevels, kTpGapLoMs,
                            kTpGapHiMs, "disc_tp_clip_lo", "disc_tp_clip_hi", "disc_tp_ttl_lo_ms",
                            "disc_tp_ttl_hi_ms"}) {
        kv.erase(key);
    }
    if (!tp) return;
    kv[kTp] = tp->price;
    kv[kTpRungs] = tp->rungs;
    kv[kTpStepBp] = tp->step_bp;
    kv[kTpSizeJitter] = tp->size_jitter;
    kv[kTpGapLoMs] = static_cast<double>(tp->gap_lo_ms);
    kv[kTpGapHiMs] = static_cast<double>(tp->gap_hi_ms);
}

std::optional<TakeProfitPolicy> read_tp(const Kv& kv) {
    const auto price = read(kv, kTp);
    if (!price) return std::nullopt;
    const auto rungs = read(kv, kTpRungs);
    return TakeProfitPolicy{.price = *price,
                            .rungs = static_cast<int>(rungs.value_or(read(kv, kTpLegacyLevels).value_or(1))),
                            .step_bp = read(kv, kTpStepBp).value_or(0),
                            .size_jitter = rungs ? read(kv, kTpSizeJitter).value_or(0) : 0,
                            .gap_lo_ms = read_ms(kv, kTpGapLoMs),
                            .gap_hi_ms = read_ms(kv, kTpGapHiMs)};
}

void write_stop(Kv& kv, const std::optional<StopPolicy>& stop) {
    for (const char* key : {kStop, kStopEveryMs, kStopEveryJitter, kStopLegacyDeadlineMs, kStopClipUsd, kStopClipJitter,
                            kStopPassive, kStopCrossBps, kStopLifeLoMs, kStopLifeHiMs, kStopLegacyCapBps,
                            kStopLegacyClipLo, kStopLegacyClipHi, kStopLegacyGapLoMs, kStopLegacyGapHiMs,
                            kStopLegacyEscalateMs, "disc_stop_anchor"}) {
        kv.erase(key);
    }
    if (!stop) return;
    kv[kStop] = stop->price;
    kv[kStopEveryMs] = static_cast<double>(stop->every_ms);
    kv[kStopEveryJitter] = stop->every_jitter;
    kv[kStopClipUsd] = stop->clip_usd;
    kv[kStopClipJitter] = stop->clip_jitter;
    kv[kStopPassive] = stop->clip_style == Style::PASSIVE ? 1 : 0;
    kv[kStopCrossBps] = stop->cross_bps;
    kv[kStopLifeLoMs] = static_cast<double>(stop->clip_life_lo_ms);
    kv[kStopLifeHiMs] = static_cast<double>(stop->clip_life_hi_ms);
}

// A legacy [lo, hi] band as its middle and jitter.
std::optional<std::pair<double, double>> legacy_band(const Kv& kv, const char* lo_key, const char* hi_key) {
    const double lo = read(kv, lo_key).value_or(0);
    const double hi = read(kv, hi_key).value_or(0);
    if (hi <= 0) return std::nullopt;
    return std::pair{(lo + hi) / 2, std::clamp((hi - lo) / (hi + lo), 0.1, 0.9)};
}

std::optional<StopPolicy> read_stop(const Kv& kv) {
    const auto price = read(kv, kStop);
    if (!price) return std::nullopt;
    StopPolicy stop{.price = *price};
    if (read(kv, kStopEveryMs)) {
        stop.every_ms = read_ms(kv, kStopEveryMs);
        stop.every_jitter = read(kv, kStopEveryJitter).value_or(stop.every_jitter);
        stop.clip_usd = read(kv, kStopClipUsd).value_or(0);
        stop.clip_jitter = read(kv, kStopClipJitter).value_or(stop.clip_jitter);
        stop.clip_style = read(kv, kStopPassive).value_or(0) > 0 ? Style::PASSIVE : Style::TAKE;
        stop.cross_bps = read(kv, kStopCrossBps).value_or(stop.cross_bps);
        stop.clip_life_lo_ms = read_ms(kv, kStopLifeLoMs);
        stop.clip_life_hi_ms = read_ms(kv, kStopLifeHiMs);
        return stop;
    }
    stop.cross_bps = std::min(read(kv, kStopLegacyCapBps).value_or(stop.cross_bps), kMaxCapBps);
    if (const auto clip = legacy_band(kv, kStopLegacyClipLo, kStopLegacyClipHi)) {
        std::tie(stop.clip_usd, stop.clip_jitter) = *clip;
    }
    if (const auto gap = legacy_band(kv, kStopLegacyGapLoMs, kStopLegacyGapHiMs)) {
        stop.every_ms = std::max(1000LL, static_cast<long long>(gap->first));
        stop.every_jitter = gap->second;
    }
    return stop;
}

// A triggered stop's clip schedule: from the trigger, never ending.
AccumulationPlan stop_plan(const StopPolicy& stop, long long hit_ms, unsigned long long signal) {
    return {.start_ms = hit_ms,
            .ttl_ms = kNeverExpiresMs,
            .clip_usd = stop.clip_usd,
            .clip_jitter = stop.clip_jitter,
            .every_ms = stop.every_ms,
            .every_jitter = stop.every_jitter,
            .seed = signal};
}

// The take-profit over the rungs not yet done, to be laid out afresh from the
// first: rungs before kTpNext that are no longer resting have filled.
TakeProfitPolicy remaining_ladder(const Kv& kv, TakeProfitPolicy tp, int resting) {
    const int done = static_cast<int>(read(kv, kTpNext).value_or(0)) - resting;
    tp.rungs = std::max(1, tp.rungs - std::max(0, done));
    return tp;
}

nlohmann::json pair_json(double lo, double hi, double scale) {
    return nlohmann::json::array({lo / scale, hi / scale});
}

nlohmann::json tp_json(const std::optional<TakeProfitPolicy>& tp, const Kv& kv) {
    if (!tp) return nullptr;
    return {{"price", tp->price}, {"rungs", tp->rungs}, {"step_bp", tp->step_bp},
            {"size_jitter", tp->size_jitter}, {"gap_s", pair_json(tp->gap_lo_ms, tp->gap_hi_ms, 1000)},
            {"next", static_cast<int>(read(kv, kTpNext).value_or(0))}};
}

nlohmann::json stop_json(const std::optional<StopPolicy>& stop, const Kv& kv, unsigned long long signal,
                         const std::string& error) {
    if (!stop) return nullptr;
    nlohmann::json out{{"price", stop->price}, {"every_s", stop->every_ms / 1000.0},
                       {"every_jitter", stop->every_jitter},
                       {"clip_usd", stop->clip_usd}, {"clip_jitter", stop->clip_jitter},
                       {"clip_style", stop->clip_style == Style::PASSIVE ? "passive" : "take"},
                       {"cross_bps", stop->cross_bps},
                       {"clip_life_s", stop->clip_life_hi_ms > 0
                                           ? pair_json(stop->clip_life_lo_ms, stop->clip_life_hi_ms, 1000)
                                           : nlohmann::json(nullptr)},
                       {"triggered_ms", nullptr}, {"clip", 0}, {"next_ms", nullptr}, {"start_qty", nullptr},
                       {"error", error.empty() ? nlohmann::json(nullptr) : nlohmann::json(error)}};
    if (const auto hit = read(kv, kStopHitMs)) {
        const int clip = static_cast<int>(read(kv, kStopClip).value_or(-1));
        out["triggered_ms"] = static_cast<long long>(*hit);
        out["clip"] = clip + 1;
        out["next_ms"] = ExecutionMath::accumulate_release_ms(stop_plan(*stop, static_cast<long long>(*hit), signal),
                                                              clip + 1);
        out["start_qty"] = read(kv, kStopStartQty).value_or(0);
    }
    return out;
}

double remaining_of(const Core::Trade& trade) {
    return (trade.open_quantity - trade.closed_quantity).toDouble();
}

std::optional<unsigned long long> signal_in(const Kv& kv) {
    const auto value = read(kv, kSignal);
    if (!value || *value <= 0) return std::nullopt;
    return static_cast<unsigned long long>(*value);
}

bool is_ours(const Core::Trade& trade) {
    return trade.type_internal == Core::TradeTypeInternal::DIRECTIONAL_TRADE &&
           signal_in(trade.kv_cache) && remaining_of(trade) > kQtyEpsilon;
}

std::optional<Core::Trade> trade_of(Core::TradeTicker& ticker, unsigned long long signal) {
    for (const auto& trade : ticker.get_active_trades_ref()) {
        if (is_ours(trade) && trade.signal_id == signal) return trade;
    }
    return std::nullopt;
}

Side side_of(const Core::Trade& trade) {
    return trade.type == Core::TradeType::LONG ? Side::BUY : Side::SELL;
}

Side opposite(Side side) { return side == Side::BUY ? Side::SELL : Side::BUY; }

std::string side_text(Side side) { return side == Side::BUY ? "buy" : "sell"; }

OrderPosting::Side posting_side(Side side) {
    return side == Side::BUY ? OrderPosting::Side::BUY : OrderPosting::Side::SELL;
}

bool retriable(Core::OrderConstructionResult result) {
    switch (result) {
        case Core::STRATEGY_EXIT_RATE_LIMITED:
        case Core::TICKER_EXIT_RATE_LIMITED:
        case Core::STRATEGY_ENTER_RATE_LIMITED:
        case Core::TICKER_ENTER_RATE_LIMITED:
        case Core::FAILED_ORDER_COOLDOWN_EXIT:
        case Core::FAILED_ORDER_COOLDOWN_ENTER:
        case Core::MAX_UNACKED_EXIT_ORDERS_EXCEEDED:
        case Core::MAX_UNACKED_ENTER_ORDERS_EXCEEDED:
            return true;
        default:
            return false;
    }
}

// One of our orders, copied out of the ticker: posting may grow its vector.
struct OurOrder {
    unsigned long long id = 0;
    unsigned long long signal = 0;
    long long command_id = 0;
    Role role = Role::RESTING;
    int level = 0;
    Side side = Side::BUY;
    bool closing = false;
    bool post_only = false;
    double price = 0;
    double qty = 0;
    double filled = 0;
    long long placed_ms = 0;
    long long expires_ms = 0;
    bool cancel_sent = false;
    Core::ExchangeAndMarket venue;
    Kv kv;

    double qty_left() const { return qty - filled; }
    OrderPosting::LiveOrder live() const {
        return {id, price, qty_left(), level, placed_ms, expires_ms, cancel_sent};
    }
};

// A new trade before its first fill: where it trades and the kv that fill
// seeds it with.
struct Opening {
    std::string symbol;
    Side side = Side::BUY;
    Core::ExchangeAndMarket venue;
    Kv kv;
};

// One trade an execution works; flatten_all works several.
struct Job {
    unsigned long long signal = 0;
    std::string symbol;
    Side side = Side::BUY;
    double target = 0;
    // A $-sized accumulation works dollars, not the quantity at arrival, so
    // a move does not change what it commits; zero means `target` rules.
    double target_usd = 0;
    double arrival = 0;
    double filled = 0;
    double filled_usd = 0;
    bool taking = false;
    // An accumulation holding its clips while the price is beyond max slippage.
    bool held = false;
    // The accumulation's last clip posted or skipped, and when the next is due.
    int clip = -1;
    long long next_clip_ms = 0;
    long long last_post_ms = 0;
    std::optional<std::pair<std::string, std::string>> ending;
    bool finished = false;
};

struct OrderRequest {
    unsigned long long signal = 0;
    Side trade_side = Side::BUY;
    Core::ExchangeAndMarket venue;
    // Entries carry the trade's kv so the first fill seeds the trade.
    Kv trade_kv;
    long long command_id = 0;
    Role role = Role::RESTING;
    int level = 0;
    bool closing = false;
    bool post_only = false;
    double qty = 0;
    double price = 0;
    long long expires_ms = 0;
    bool stop_loss = false;
};

struct Replacement {
    unsigned long long order_id = 0;
    OrderRequest request;
    std::optional<double> qty;
    double qty_left = 0;
};

enum class Activity { EXECUTION, RESTING, PAUSE };

// The strategy's only state: commands still at work. Everything they act on
// is read from the tickers each pass; a restart rebuilds them from the
// command rows and the trades' kv.
struct ActiveCommand {
    long long id = 0;
    Activity activity = Activity::EXECUTION;
    long long start_ms = 0;
    Execution exec;
    std::vector<Job> jobs;
    std::string symbol;
    unsigned long long signal = 0;
    std::optional<Opening> opening;
    std::optional<Replacement> replacement;
    std::optional<std::string> cancelled;
    bool done = false;
    std::optional<long long> account_id;

    bool reducing() const {
        return exec.kind == ExecutionKind::REDUCE || exec.kind == ExecutionKind::CLOSE;
    }
};

struct State {
    std::mutex mutex;
    long long last_pass_ms = 0;
    bool scanned = false;
    // Symbols with our trades, orders or commands; the rest are not visited.
    std::set<std::string> symbols;
    std::vector<ActiveCommand> active;
    // Take-profit post failures by trade; a restart retries from zero.
    std::map<unsigned long long, std::pair<int, std::string>> tp_failures;
    // A stopping trade's last refused clip; the next clip tries again.
    std::map<unsigned long long, std::string> stop_errors;
};

State& state_for(int id) {
    static std::mutex mutex;
    static std::map<int, std::unique_ptr<State>> states;
    std::lock_guard lock(mutex);
    auto& state = states[id];
    if (!state) state = std::make_unique<State>();
    return *state;
}

struct Sent {
    std::optional<unsigned long long> id;
    std::optional<Core::OrderConstructionResult> failure;
    // True when the trade is already covered by exit orders on the book.
    bool covered = false;
};

// A trade by its signal: open on a ticker, or a new one not yet filled.
struct TradeView {
    unsigned long long signal = 0;
    std::string symbol;
    Side side = Side::BUY;
    Core::ExchangeAndMarket venue;
    Kv kv;
    std::optional<Core::Trade> trade;

    bool is_long() const { return side == Side::BUY; }
};

class Runner {
   public:
    Runner(Core::Engine& engine, Core::Strategy& strategy, State& state)
        : engine_(engine), strategy_(strategy), state_(state), now_(engine.get_time()) {
        std::shared_lock lock(strategy.trade_tickers_mutex);
        tickers_ = {strategy.trade_tickers.begin(), strategy.trade_tickers.end()};
    }

    void handle(const Core::StrategyCommand& command) {
        if (command.resumed && active_by_id(command.id)) return;
        struct ResetScope {
            std::optional<long long>& scope;
            ~ResetScope() { scope.reset(); }
        } reset{scope_};
        try {
            scope_ = command.payload.contains("account_id") ? std::optional(command.payload.at("account_id").get<long long>()) : std::nullopt;
            if (strategy_.strategy_config.discretionary_multi_account) {
                if (!scope_) throw Refused("account_id is required");
                const auto account = Core::DiscretionaryAccounts::instance().get(*scope_);
                if (!account || account->paper != strategy_.strategy_config.is_paper_trading) throw Refused("Account execution mode does not match executor");
            }
            const auto parsed = CommandParser::parse(command.op, command.payload);
            if (command.resumed) {
                resume(command, parsed);
            } else {
                std::visit([&](const auto& c) { apply(command.id, command.ts_ms, c); }, parsed);
            }
            if (auto* active = active_by_id(command.id)) active->account_id = scope_;
        } catch (const std::invalid_argument& e) {
            report(command.id, Core::STRATEGY_COMMAND_FAILED, e.what());
        } catch (const Refused& e) {
            report(command.id, Core::STRATEGY_COMMAND_FAILED, e.what());
        } catch (const nlohmann::json::exception& e) {
            report(command.id, Core::STRATEGY_COMMAND_FAILED, e.what());
        }
    }

    void step() {
        if (!state_.scanned) {
            for (const auto& [symbol, _] : tickers_) state_.symbols.insert(symbol);
            state_.scanned = true;
        }
        for (const auto& command : state_.active) {
            if (!command.symbol.empty()) state_.symbols.insert(command.symbol);
            for (const auto& job : command.jobs) state_.symbols.insert(job.symbol);
        }
        for (auto it = state_.symbols.begin(); it != state_.symbols.end();) {
            const auto ticker = tickers_.find(*it);
            const bool busy = ticker != tickers_.end() && step_symbol(*it, *ticker->second);
            it = busy ? std::next(it) : state_.symbols.erase(it);
        }
        std::erase_if(state_.active, [](const ActiveCommand& c) { return c.done; });
    }

    nlohmann::json book_state() const {
        // The largest venue minimum per symbol, so the console can size clips.
        nlohmann::json min_notional = nlohmann::json::object();
        for (const auto& [symbol, ticker] : tickers_) {
            double most = 0;
            for (const auto& venue : ticker->exchange_and_markets) {
                most = std::max(most, engine_.get_min_notional(symbol, venue.exchange_name, venue.market_type));
            }
            min_notional[symbol] = most;
        }
        nlohmann::json paused_accounts = nlohmann::json::array();
        for (const auto& command : state_.active) {
            if (command.activity == Activity::PAUSE && !command.done && command.account_id) paused_accounts.push_back(*command.account_id);
        }
        return {{"paused", paused()}, {"paused_accounts", paused_accounts}, {"trades", trades_}, {"jobs", jobs_}, {"orders", orders_},
                {"limits", {{"max_command_usd", kMaxCommandNotionalUsd}, {"max_cross_pct", kMaxCrossPct},
                            {"max_bracket_pct", kMaxBracketPct}, {"min_notional", min_notional}}}};
    }

    std::vector<Core::StrategyDecision> decisions() {
        std::vector<Core::StrategyDecision> result;
        for (auto& [_, decision] : decided_) {
            if (!decision.orders.empty() || !decision.cancelled_orders.empty()) {
                result.push_back(std::move(decision));
            }
        }
        return result;
    }

   private:
    Core::Engine& engine_;
    Core::Strategy& strategy_;
    State& state_;
    long long now_;
    std::optional<long long> scope_;

    bool in_scope(long long account) const { return !scope_ || *scope_ == account; }
    std::map<std::string, std::shared_ptr<Core::TradeTicker>> tickers_;
    std::map<std::string, Core::StrategyDecision> decided_;
    nlohmann::json trades_ = nlohmann::json::array();
    nlohmann::json jobs_ = nlohmann::json::array();
    nlohmann::json orders_ = nlohmann::json::array();

    // --- lookups ---

    void report(long long command_id, const std::string& status, const std::string& detail) {
        if (command_id <= 0) return;
        Core::StrategyCommandInbox::instance().report(
            {command_id, strategy_.strategy_config.id, status, detail});
    }

    bool paused(std::optional<long long> account = std::nullopt) const {
        return std::any_of(state_.active.begin(), state_.active.end(), [account](const ActiveCommand& c) {
            return c.activity == Activity::PAUSE && !c.done && (!account || !c.account_id || c.account_id == account);
        });
    }

    ActiveCommand* active_by_id(long long id) {
        for (auto& command : state_.active) {
            if (command.id == id && !command.done) return &command;
        }
        return nullptr;
    }

    Core::StrategyDecision& decision_for(const std::string& symbol) {
        auto& decision = decided_[symbol];
        decision.symbol = symbol;
        return decision;
    }

    Core::TradeTicker& ticker_for(const std::string& symbol) {
        const auto it = tickers_.find(symbol);
        if (it == tickers_.end()) throw Refused(symbol + " is not traded by this strategy");
        return *it->second;
    }

    Quote quote_of(const std::string& symbol, const Core::ExchangeAndMarket& venue) {
        Quote q;
        const auto bba = engine_.get_bba(symbol, venue.exchange_name, venue.market_type);
        if (bba && now_ - bba->timestamp_ms <= kQuoteMaxAgeMs && bba->best_bid_price > 0 &&
            bba->best_ask_price > 0) {
            q.bid = bba->best_bid_price;
            q.ask = bba->best_ask_price;
        }
        q.last = engine_.get_price(symbol, venue.exchange_name, venue.market_type);
        if (!q.last && q.bid) q.last = (*q.bid + *q.ask) / 2;
        return q;
    }

    static OrderPosting::Quote posting_quote(const Quote& q) { return {q.bid, q.ask, q.last}; }

    std::function<double(double)> rounding(const std::string& symbol,
                                           const Core::ExchangeAndMarket& venue) {
        if (engine_.is_backtest) return [](double price) { return price; };
        return [this, symbol, venue](double price) {
            return engine_.round_price(symbol, venue.exchange_name, venue.market_type, price).toDouble();
        };
    }

    double nav(const Core::ExchangeAndMarket& venue) {
        if (strategy_.strategy_config.discretionary_multi_account) {
            return venue.account_id ? strategy_.get_nav_by_account_id(*venue.account_id).toDouble() : 0;
        }
        return strategy_.get_strategy_trade_size(engine_, venue.exchange_name, venue.market_type);
    }

    std::vector<OurOrder> ours(Core::TradeTicker& ticker) {
        std::vector<OurOrder> result;
        for (const auto& order : ticker.get_active_orders_ref()) {
            const auto signal = signal_in(order.kv_cache);
            if (!signal || !in_scope(order.account_id)) continue;
            result.push_back(
                {.id = order.id,
                 .signal = *signal,
                 .command_id = static_cast<long long>(read(order.kv_cache, kCmd).value_or(0)),
                 .role = static_cast<Role>(static_cast<int>(
                     read(order.kv_cache, kRole).value_or(static_cast<double>(Role::RESTING)))),
                 .level = static_cast<int>(read(order.kv_cache, kLevel).value_or(0)),
                 .side = order.side == Core::OrderSide::BUY ? Side::BUY : Side::SELL,
                 .closing = order.is_trade_close_order,
                 .post_only = order.post_only,
                 .price = order.price.toDouble(),
                 .qty = order.quantity.toDouble(),
                 .filled = order.filled_quantity.toDouble(),
                 .placed_ms = order.create_timestamp,
                 .expires_ms = order.expires_on_ms - now_ > kNeverExpiresMs / 2 ? 0 : order.expires_on_ms,
                 .cancel_sent = ticker.is_cancel_already_sent(order),
                 .venue = {order.exchange_name, order.market_type, order.account_id},
                 .kv = order.kv_cache});
        }
        return result;
    }

    std::vector<Core::Trade> our_trades(Core::TradeTicker& ticker) {
        std::vector<Core::Trade> result;
        for (const auto& trade : ticker.get_active_trades_ref()) {
            if (is_ours(trade) && in_scope(trade.account_id)) result.push_back(trade);
        }
        return result;
    }

    const Opening* opening_of(unsigned long long signal) const {
        for (const auto& command : state_.active) {
            if (!command.done && command.opening && command.id == static_cast<long long>(signal)) {
                return &*command.opening;
            }
        }
        return nullptr;
    }

    std::optional<TradeView> find(unsigned long long signal) {
        for (const auto& [symbol, ticker] : tickers_) {
            std::lock_guard lock(ticker->trading_mutex);
            if (auto trade = trade_of(*ticker, signal); trade && in_scope(trade->account_id)) {
                return TradeView{signal, symbol, side_of(*trade),
                                 {trade->exchange_name, trade->market_type, trade->account_id},
                                 trade->kv_cache, std::move(trade)};
            }
        }
        if (const auto* opening = opening_of(signal); opening && (!scope_ || opening->venue.account_id == scope_)) {
            return TradeView{signal, opening->symbol, opening->side, opening->venue, opening->kv, std::nullopt};
        }
        return std::nullopt;
    }

    TradeView view_of(unsigned long long signal) {
        auto view = find(signal);
        if (!view) throw Refused("no open trade with signal_id " + std::to_string(signal));
        return *view;
    }

    // Writes a trade's kv: onto the trade, or before its first fill onto the
    // opening and the entry orders that will seed it.
    void write_kv(Core::TradeTicker& ticker, const TradeView& view, const Kv& kv) {
        if (view.trade) {
            ticker.set_trade_kv_cache(*view.trade, kv);
            return;
        }
        for (auto& command : state_.active) {
            if (command.opening && command.id == static_cast<long long>(view.signal)) {
                command.opening->kv = kv;
            }
        }
        for (auto& order : ticker.get_active_orders_mut_ref()) {
            if (signal_in(order.kv_cache) != view.signal || order.is_trade_close_order) continue;
            Kv stamped = kv;
            for (const char* tag : {kCmd, kRole, kLevel}) {
                if (const auto value = read(order.kv_cache, tag)) stamped[tag] = *value;
            }
            order.kv_cache = stamped;
        }
    }

    // The ticker holds every exchanges_list venue that lists the symbol; the
    // account or exchange picks one, and a single venue needs neither.
    Core::ExchangeAndMarket venue_of(const Core::TradeTicker& ticker, const NewTrade& t) {
        if (strategy_.strategy_config.discretionary_multi_account &&
            (strategy_.strategy_config.strategy_name == "crypto-discretionary-paper" ||
             strategy_.strategy_config.strategy_name == "crypto-discretionary-realmoney")) {
            if (!t.account_id || !in_scope(*t.account_id)) throw Refused("Choose an account for this trade");
            const auto account = Core::DiscretionaryAccounts::instance().get(*t.account_id);
            if (!account || account->paper != strategy_.strategy_config.is_paper_trading ||
                (t.exchange && *t.exchange != account->exchange)) throw Refused("Account is incompatible with executor");
            return {account->exchange, account->market, account->id};
        }
        std::vector<Core::ExchangeAndMarket> matches;
        for (const auto& venue : ticker.exchange_and_markets) {
            if (t.account_id && venue.account_id != t.account_id) continue;
            if (t.exchange && venue.exchange_name != *t.exchange) continue;
            matches.push_back(venue);
        }
        if (matches.size() == 1) return matches.front();
        if (matches.empty()) {
            std::string wanted = t.account_id ? "account " + std::to_string(*t.account_id) : "";
            if (t.exchange) wanted += (wanted.empty() ? "" : " on ") + *t.exchange;
            throw Refused(wanted + " is not in this strategy's exchanges_list for " + t.symbol);
        }
        throw Refused(t.symbol + " trades on several venues; name an account_id or exchange");
    }

    static bool same_venue(const Core::ExchangeAndMarket& a, const Core::ExchangeAndMarket& b) {
        return a.exchange_name == b.exchange_name && a.market_type == b.market_type && a.account_id == b.account_id;
    }

    // Opposite-side trades on one venue's symbol would net or hold both
    // hedge-mode sides.
    void require_entry(Core::TradeTicker& ticker, const std::string& symbol, Side side,
                       const Core::ExchangeAndMarket& venue) {
        if (paused(venue.account_id)) throw Refused("entries are paused");
        if (strategy_.strategy_config.discretionary_multi_account &&
            (strategy_.strategy_config.is_disabled || strategy_.strategy_config.is_in_reduce_only)) throw Refused("Executor is disabled or draining");
        if (strategy_.strategy_config.discretionary_multi_account) {
            const auto account = venue.account_id ? Core::DiscretionaryAccounts::instance().get(*venue.account_id) : std::nullopt;
            if (!account || !account->enabled || account->paper != strategy_.strategy_config.is_paper_trading ||
                account->exchange != venue.exchange_name || account->market != venue.market_type || !in_scope(account->id)) {
                throw Refused("Account is disabled or incompatible with executor");
            }
        }
        for (const auto& trade : our_trades(ticker)) {
            if (side_of(trade) != side &&
                same_venue({trade.exchange_name, trade.market_type, trade.account_id}, venue)) {
                throw Refused("trade " + std::to_string(trade.signal_id) + " on " + symbol +
                              " is on the other side; close it first");
            }
        }
        for (const auto& command : state_.active) {
            if (!command.done && command.opening && command.opening->symbol == symbol &&
                command.opening->side != side && same_venue(command.opening->venue, venue)) {
                throw Refused("trade " + std::to_string(command.id) + " on " + symbol +
                              " is on the other side; close it first");
            }
        }
        for (const auto& order : ticker.get_active_orders_ref()) {
            if (!order.is_trade_close_order &&
                same_venue({order.exchange_name, order.market_type, order.account_id}, venue) &&
                (order.side == Core::OrderSide::BUY) != (side == Side::BUY)) {
                throw Refused("opposite-side entry orders are resting on " + symbol);
            }
        }
    }

    static void check_brackets(bool is_long, const std::optional<StopPolicy>& stop_policy,
                               const std::optional<TakeProfitPolicy>& take_profit, double price,
                               double min_notional) {
        const auto stop = stop_policy ? std::optional(stop_policy->price) : std::nullopt;
        const auto tp = take_profit ? std::optional(take_profit->price) : std::nullopt;
        if (stop_policy && stop_policy->clip_usd > 0) {
            if (const auto problem = ExecutionMath::clip_problem(stop_policy->clip_usd, stop_policy->clip_jitter,
                                                                  min_notional)) {
                throw Refused("the stop's " + *problem);
            }
        }
        if (stop && !ExecutionMath::stop_valid(is_long, *stop, price)) {
            throw Refused(std::string("stop must be ") + (is_long ? "below" : "above") +
                          " the price for a " + (is_long ? "long" : "short"));
        }
        if (tp && !ExecutionMath::stop_valid(!is_long, *tp, price)) {
            throw Refused(std::string("take-profit must be ") + (is_long ? "above" : "below") +
                          " the price for a " + (is_long ? "long" : "short"));
        }
        for (const auto& [name, level] : {std::pair{"stop", stop}, std::pair{"take-profit", tp}}) {
            if (level && !ExecutionMath::bracket_near(*level, price)) {
                throw Refused(std::string(name) + " is more than " + std::to_string(static_cast<int>(kMaxBracketPct)) +
                              "% from the price");
            }
        }
        if (take_profit) {
            const double offset = (take_profit->rungs - 1) * take_profit->step_bp / 1e4;
            const double farthest = take_profit->price * (is_long ? 1 + offset : 1 - offset);
            if (farthest <= 0 || !ExecutionMath::bracket_near(farthest, price)) {
                throw Refused("the take-profit's farthest rung is more than " +
                              std::to_string(static_cast<int>(kMaxBracketPct)) + "% from the price");
            }
        }
    }

    // Fat-finger bounds a command passes before anything posts: what it adds
    // in notional and, for a limit, how far it crosses the market.
    static void check_bounds(Side side, double qty, double price, const Quote& quote, bool adds, bool limit) {
        if (adds) {
            if (const auto problem = ExecutionMath::notional_problem(qty, price)) throw Refused(*problem);
        }
        if (limit) {
            if (const auto problem = ExecutionMath::cross_problem(side, price, quote)) throw Refused(*problem);
        }
    }

    static double usd_target(const Execution& c) {
        return c.style == Style::ACCUMULATE && c.size.notional ? *c.size.notional : 0;
    }

    double min_notional_of(const std::string& symbol, const Core::ExchangeAndMarket& venue) {
        return engine_.get_min_notional(symbol, venue.exchange_name, venue.market_type);
    }

    bool dust(double qty, const std::string& symbol, const Core::ExchangeAndMarket& venue) {
        return ExecutionMath::is_dust(qty, engine_.get_step_size(symbol, venue.exchange_name, venue.market_type));
    }

    void check_clips(const Execution& c, const std::string& symbol, const Core::ExchangeAndMarket& venue) {
        if (c.style != Style::ACCUMULATE) return;
        if (const auto problem = ExecutionMath::clip_problem(c.clip_usd, c.clip_jitter, min_notional_of(symbol, venue))) {
            throw Refused(*problem);
        }
    }

    static Kv new_trade_kv(long long id, const NewTrade& t) {
        Kv kv{{kSignal, static_cast<double>(id)},
              {kSetup, static_cast<double>(t.setup_id)},
              {kEv, t.ev_pct}};
        write_stop(kv, t.stop);
        write_tp(kv, t.tp);
        return kv;
    }

    static std::optional<double> stop_price(const Kv& kv) {
        const auto stop = read_stop(kv);
        return stop ? std::optional(stop->price) : std::nullopt;
    }

    Core::Signal signal_of(unsigned long long signal, bool is_long, const std::string& symbol,
                           double price) {
        Core::Signal s{};
        s.signal_strength = is_long ? 1 : -1;
        s.prediction = 0;
        s.price = price;
        s.event_time_ms = now_;
        s.bar_close_time_ms = now_;
        s.signal_id = signal;
        s.strategy_id = strategy_.strategy_config.id;
        s.symbol = symbol;
        return s;
    }

    // Entries carry the trade's signal so their fills join it; exits close
    // that trade only.
    Sent send(Core::TradeTicker& ticker, const OrderRequest& r, const std::optional<Core::Trade>& trade) {
        Sent sent;
        double qty = r.qty;
        double sizing = 0;
        if (r.closing) {
            if (!trade) {
                sent.covered = true;
                return sent;
            }
            const double available = ticker.get_trade_remaining_qty(*trade).toDouble();
            if (dust(available, ticker.symbol, r.venue)) {
                sent.covered = true;
                return sent;
            }
            qty = std::min(qty, available);
            sizing = (trade->sizing_coeff_delta * qty / trade->open_quantity).toDouble();
        } else {
            const double size = nav(r.venue);
            if (size <= 0) {
                sent.failure = Core::INVALID_SIZING_COEFF_DELTA;
                return sent;
            }
            sizing = qty * r.price / size;
        }
        Kv kv = r.closing ? Kv{{kSignal, static_cast<double>(r.signal)}} : r.trade_kv;
        kv[kCmd] = static_cast<double>(r.command_id);
        kv[kRole] = static_cast<double>(r.role);
        kv[kLevel] = r.level;
        const bool is_long = r.trade_side == Side::BUY;
        const Side side = r.closing ? opposite(r.trade_side) : r.trade_side;
        const auto fresh = signal_of(r.signal, is_long, ticker.symbol, r.price);
        // Hedge mode: positionSide alone makes an exit reduce-only, and the
        // venue rejects an explicit reduceOnly with it.
        Core::CreateOrderParams params{
            .symbol = ticker.symbol,
            .type = Core::OrderType::LIMIT,
            .side = side == Side::BUY ? Core::OrderSide::BUY : Core::OrderSide::SELL,
            .price = r.price,
            .prediction = trade && r.closing ? trade->prediction.value_or(fresh) : fresh,
            .quantity = qty,
            .exchange_name = r.venue.exchange_name,
            .market_type = r.venue.market_type,
            .strategy_id = strategy_.strategy_config.id,
            .expires_on_ms = r.expires_ms > 0 ? r.expires_ms : now_ + kNeverExpiresMs,
            .sizing_coeff_delta = sizing,
            .trade_id = r.closing ? std::optional<unsigned long long>(trade->id) : std::nullopt,
            .reduce_only = false,
            .timestamp = now_,
            .quote_asset = ticker.quote_asset,
            .base_asset = ticker.base_asset,
            .allowed_to_bypass_risk_engine = false,
            .must_pass_min_notional_filter = true,
            .is_trade_close_order = r.closing,
            .type_internal = Core::OrderTypeInternal::DISCRETIONARY_ORDER,
            .kv_cache = kv,
            .account_id = r.venue.account_id,
            .position_side = is_long ? Core::PositionSide::LONG_SIDE : Core::PositionSide::SHORT_SIDE,
            .is_stop_loss_order = r.stop_loss};
        if (strategy_.strategy_config.discretionary_multi_account) {
            const auto account = r.venue.account_id ? Core::DiscretionaryAccounts::instance().get(*r.venue.account_id) : std::nullopt;
            if (!account || account->paper != strategy_.strategy_config.is_paper_trading ||
                account->exchange != r.venue.exchange_name || account->market != r.venue.market_type ||
                (!r.closing && !account->enabled) ||
                !Core::DiscretionaryAccounts::instance().reserve_order(account->id, now_)) {
                sent.failure = Core::ORDER_NOT_ALLOWED_TO_PASS_RISK_ENGINE;
                return sent;
            }
        }
        auto [result, order] = ticker.construct_order(engine_, strategy_, params);
        if (result != Core::OrderConstructionResult::VALID || !order) {
            sent.failure = result;
            return sent;
        }
        order->post_only = r.post_only;
        ticker.mark_order_to_be_created(engine_, *order, decision_for(ticker.symbol), strategy_);
        if (r.command_id > 0) {
            Core::StrategyCommandInbox::instance().link_order({r.command_id, order->id});
        }
        sent.id = order->id;
        return sent;
    }

    static std::string failure_text(const Sent& sent) {
        if (sent.covered) return "the trade is already covered by exit orders";
        return Core::order_construction_result_to_str(
            sent.failure.value_or(Core::ENGINE_CONSTRUCT_ORDER_FAILED));
    }

    void cancel(Core::TradeTicker& ticker, unsigned long long order_id) {
        for (auto& order : ticker.get_active_orders_mut_ref()) {
            if (order.id != order_id || ticker.is_cancel_already_sent(order)) continue;
            ticker.mark_order_to_be_cancelled(engine_, order, decision_for(ticker.symbol));
            return;
        }
    }

    // Ends what works a trade besides its stop: executions and resting
    // orders, with their commands told why.
    void end_trade_work(unsigned long long signal, bool entries, bool exits,
                        const std::string& detail) {
        for (auto& command : state_.active) {
            if (command.done) continue;
            if (command.activity == Activity::EXECUTION) {
                if (!(command.reducing() ? exits : entries)) continue;
                for (auto& job : command.jobs) {
                    if (job.signal == signal && !job.ending) {
                        job.ending = {Core::STRATEGY_COMMAND_CANCELLED, detail};
                    }
                }
            } else if (command.activity == Activity::RESTING && command.signal == signal) {
                const bool closing = command.opening ? false : command.replacement
                    ? command.replacement->request.closing
                    : false;
                (void)closing;
                if (!command.cancelled) command.cancelled = detail;
            }
        }
    }

    // --- commands ---

    void apply(long long id, long long, const PlaceOrder& c) {
        if (c.open) {
            auto& ticker = ticker_for(c.open->symbol);
            std::lock_guard lock(ticker.trading_mutex);
            const auto venue = venue_of(ticker, *c.open);
            require_entry(ticker, c.open->symbol, c.open->side, venue);
            const bool is_long = c.open->side == Side::BUY;
            check_brackets(is_long, c.open->stop, c.open->tp, c.price, min_notional_of(c.open->symbol, venue));
            if (c.post_only && ExecutionMath::crosses(c.open->side, c.price, quote_of(c.open->symbol, venue))) {
                throw Refused("post-only order would cross the book");
            }
            const auto kv = new_trade_kv(id, *c.open);
            const auto qty = ExecutionMath::entry_qty(c.size, c.price, nav(venue), stop_price(kv));
            if (!qty) throw Refused("risk_pct needs a stop on the trade");
            check_bounds(c.open->side, *qty, c.price, quote_of(c.open->symbol, venue), true, true);
            OrderRequest r{.signal = static_cast<unsigned long long>(id), .trade_side = c.open->side,
                           .venue = venue, .trade_kv = kv, .command_id = id, .role = Role::RESTING,
                           .post_only = c.post_only, .qty = *qty, .price = c.price,
                           .expires_ms = c.expires_ms ? now_ + *c.expires_ms : 0};
            const auto sent = send(ticker, r, std::nullopt);
            if (!sent.id) throw Refused(failure_text(sent));
            state_.active.push_back({.id = id, .activity = Activity::RESTING, .start_ms = now_,
                                     .symbol = c.open->symbol, .signal = r.signal,
                                     .opening = Opening{c.open->symbol, c.open->side, venue, kv}});
            report(id, Core::STRATEGY_COMMAND_WORKING, "resting");
            return;
        }
        const auto view = view_of(*c.trade);
        auto& ticker = ticker_for(view.symbol);
        std::lock_guard lock(ticker.trading_mutex);
        const auto trade = trade_of(ticker, view.signal);
        const Side side = c.reduce_only ? opposite(view.side) : view.side;
        if (c.post_only && ExecutionMath::crosses(side, c.price, quote_of(view.symbol, view.venue))) {
            throw Refused("post-only order would cross the book");
        }
        double qty = 0;
        if (c.reduce_only) {
            if (!trade) throw Refused("trade " + std::to_string(view.signal) + " has no open quantity");
            if (c.size.risk_pct) throw Refused("size a reduce-only order by qty, notional or fraction");
            qty = c.size.fraction ? remaining_of(*trade) * *c.size.fraction
                                  : c.size.qty.value_or(c.size.notional.value_or(0) / c.price);
        } else {
            require_entry(ticker, view.symbol, view.side, view.venue);
            const auto entry = ExecutionMath::entry_qty(c.size, c.price, nav(view.venue), stop_price(view.kv));
            if (!entry) throw Refused("risk_pct needs a stop on the trade");
            qty = *entry;
        }
        check_bounds(side, qty, c.price, quote_of(view.symbol, view.venue), !c.reduce_only, true);
        OrderRequest r{.signal = view.signal, .trade_side = view.side, .venue = view.venue,
                       .trade_kv = view.kv, .command_id = id, .role = Role::RESTING,
                       .closing = c.reduce_only, .post_only = c.post_only, .qty = qty, .price = c.price,
                       .expires_ms = c.expires_ms ? now_ + *c.expires_ms : 0};
        const auto sent = send(ticker, r, trade);
        if (!sent.id) throw Refused(failure_text(sent));
        state_.active.push_back({.id = id, .activity = Activity::RESTING, .start_ms = now_,
                                 .symbol = view.symbol, .signal = view.signal});
        report(id, Core::STRATEGY_COMMAND_WORKING, "resting");
    }

    // Orders a cancel or replace may touch: resting ones the operator placed.
    std::vector<std::pair<std::string, OurOrder>> targets(const OrderTarget& target) {
        std::vector<std::pair<std::string, OurOrder>> found;
        for (const auto& [symbol, ticker] : tickers_) {
            std::lock_guard lock(ticker->trading_mutex);
            for (const auto& order : ours(*ticker)) {
                const bool match = target.order_id ? order.id == *target.order_id
                                                   : order.command_id == *target.command_id &&
                                                         order.role == Role::RESTING;
                if (!match) continue;
                if (order.role == Role::EXECUTION) {
                    throw Refused("order belongs to an execution; use cancel_execution");
                }
                if (order.role == Role::TAKE_PROFIT || order.role == Role::STOP) {
                    throw Refused("take-profit and stop orders follow set_brackets");
                }
                found.emplace_back(symbol, order);
            }
        }
        if (found.empty()) throw Refused("no open order matches");
        return found;
    }

    void apply(long long id, long long, const CancelOrder& c) {
        const std::string detail = "cancelled by command " + std::to_string(id);
        for (const auto& [symbol, order] : targets(c.target)) {
            auto& ticker = ticker_for(symbol);
            std::lock_guard lock(ticker.trading_mutex);
            cancel(ticker, order.id);
            if (auto* command = active_by_id(order.command_id)) {
                if (!command->cancelled) command->cancelled = detail;
            }
        }
        report(id, Core::STRATEGY_COMMAND_DONE, "cancel sent");
    }

    void apply(long long id, long long, const CancelAllOrders& c) {
        if (c.trade) view_of(*c.trade);
        const std::string detail = "cancelled by command " + std::to_string(id);
        const bool entries = c.filter != OrderFilter::REDUCE_ONLY;
        const bool exits = c.filter != OrderFilter::ENTRY;
        int count = 0;
        std::set<unsigned long long> signals;
        for (const auto& [symbol, ticker] : tickers_) {
            if (c.symbol && symbol != *c.symbol) continue;
            std::lock_guard lock(ticker->trading_mutex);
            for (const auto& order : ours(*ticker)) {
                if (c.trade && order.signal != *c.trade) continue;
                if (order.role == Role::STOP || (order.closing ? !exits : !entries)) continue;
                cancel(*ticker, order.id);
                signals.insert(order.signal);
                ++count;
            }
            // A cancelled take-profit stays cancelled.
            for (const auto& trade : our_trades(*ticker)) {
                if (c.trade && trade.signal_id != *c.trade) continue;
                signals.insert(trade.signal_id);
                if (!exits || !read(trade.kv_cache, kTp)) continue;
                auto kv = trade.kv_cache;
                write_tp(kv, std::nullopt);
                ticker->set_trade_kv_cache(trade, kv);
            }
        }
        for (const auto signal : signals) end_trade_work(signal, entries, exits, detail);
        report(id, Core::STRATEGY_COMMAND_DONE, std::to_string(count) + " orders cancelled");
    }

    void apply(long long id, long long, const ReplaceOrder& c) {
        const auto found = targets(c.target);
        const auto& [symbol, order] = found.front();
        auto* command = active_by_id(order.command_id);
        if (!command || command->activity != Activity::RESTING) {
            throw Refused("only orders placed by a running place_order can be replaced");
        }
        auto& ticker = ticker_for(symbol);
        std::lock_guard lock(ticker.trading_mutex);
        if (order.post_only && c.price && ExecutionMath::crosses(order.side, *c.price, quote_of(symbol, order.venue))) {
            throw Refused("post-only order would cross the book");
        }
        check_bounds(order.side, c.qty.value_or(order.qty_left()), c.price.value_or(order.price),
                     quote_of(symbol, order.venue), !order.closing, true);
        const auto trade = trade_of(ticker, order.signal);
        OrderRequest r{.signal = order.signal,
                       .trade_side = order.closing ? opposite(order.side) : order.side,
                       .venue = order.venue, .trade_kv = order.kv, .command_id = order.command_id,
                       .role = Role::RESTING, .closing = order.closing, .post_only = order.post_only,
                       .price = c.price.value_or(order.price), .expires_ms = order.expires_ms};
        command->replacement = Replacement{order.id, r, c.qty, order.qty_left()};
        cancel(ticker, order.id);
        report(id, Core::STRATEGY_COMMAND_DONE, "replacing order " + std::to_string(order.id));
    }

    void apply(long long id, long long ts_ms, const Execution& c) { start_execution(id, ts_ms, c, nullptr); }

    // `saved` is the kv a resumed execution left on its trade or orders.
    void start_execution(long long id, long long start_ms, const Execution& c, const Kv* saved) {
        const auto restored = [&](Job& job) {
            if (!saved || read(*saved, kJobCmd) != static_cast<double>(id)) return false;
            job.target = read(*saved, kJobTarget).value_or(job.target);
            job.target_usd = read(*saved, kJobTargetUsd).value_or(0);
            job.arrival = read(*saved, kJobArrival).value_or(job.arrival);
            job.taking = read(*saved, kJobTaking).value_or(0) > 0;
            job.clip = static_cast<int>(read(*saved, kJobClip).value_or(-1));
            return true;
        };
        ActiveCommand command{.id = id, .activity = Activity::EXECUTION, .start_ms = start_ms, .exec = c};
        if (c.kind == ExecutionKind::OPEN) {
            auto& ticker = ticker_for(c.open->symbol);
            std::lock_guard lock(ticker.trading_mutex);
            const auto signal = static_cast<unsigned long long>(id);
            const auto venue = venue_of(ticker, *c.open);
            Job job{.signal = signal, .symbol = c.open->symbol, .side = c.open->side};
            const auto kv = new_trade_kv(id, *c.open);
            if (!restored(job)) {
                require_entry(ticker, c.open->symbol, c.open->side, venue);
                const auto arrival = ExecutionMath::touch(c.open->side, quote_of(c.open->symbol, venue));
                if (!arrival) throw Refused("no price for " + c.open->symbol + " on " + venue.exchange_name);
                check_brackets(c.open->side == Side::BUY, c.open->stop, c.open->tp, *arrival,
                               min_notional_of(c.open->symbol, venue));
                const auto qty = ExecutionMath::entry_qty(c.size, *arrival, nav(venue), stop_price(kv));
                if (!qty) throw Refused("cannot size the entry");
                check_bounds(c.open->side, *qty, *arrival, {}, true, false);
                check_clips(c, c.open->symbol, venue);
                job.target = *qty;
                job.target_usd = usd_target(c);
                job.arrival = *arrival;
            }
            if (!trade_of(ticker, signal)) command.opening = Opening{c.open->symbol, c.open->side, venue, kv};
            command.symbol = c.open->symbol;
            command.jobs.push_back(job);
        } else {
            const auto view = view_of(*c.trade);
            if (!saved && c.kind != ExecutionKind::CLOSE) {
                for (const auto& other : state_.active) {
                    if (other.done || other.activity != Activity::EXECUTION) continue;
                    for (const auto& job : other.jobs) {
                        if (job.signal == view.signal && !job.finished) {
                            throw Refused("execution " + std::to_string(other.id) +
                                          " is already running on trade " + std::to_string(view.signal));
                        }
                    }
                }
                if (read(view.kv, kStopHitMs)) throw Refused(kStoppingRefusal);
            }
            auto& ticker = ticker_for(view.symbol);
            std::lock_guard lock(ticker.trading_mutex);
            const auto quote = quote_of(view.symbol, view.venue);
            const bool reducing = c.kind != ExecutionKind::ADD;
            Job job{.signal = view.signal, .symbol = view.symbol,
                    .side = reducing ? opposite(view.side) : view.side};
            if (!restored(job)) {
                check_clips(c, view.symbol, view.venue);
                const auto arrival = ExecutionMath::touch(job.side, quote);
                if (!arrival) throw Refused("no price for " + view.symbol);
                job.arrival = *arrival;
                if (!reducing) {
                    require_entry(ticker, view.symbol, view.side, view.venue);
                    const auto qty = ExecutionMath::entry_qty(c.size, *arrival, nav(view.venue), stop_price(view.kv));
                    if (!qty) throw Refused("risk_pct needs a stop on the trade");
                    check_bounds(job.side, *qty, *arrival, quote, true, false);
                    job.target = *qty;
                } else {
                    if (!view.trade) throw Refused("trade " + std::to_string(view.signal) + " has no open quantity");
                    const double open_qty = remaining_of(*view.trade);
                    job.target = c.kind == ExecutionKind::CLOSE ? open_qty
                        : c.size.fraction ? open_qty * *c.size.fraction
                                          : std::min(c.size.qty.value_or(c.size.notional.value_or(0) / *arrival), open_qty);
                }
                job.target_usd = usd_target(c);
            }
            // A close takes over the trade: its entries, other exits, take-profit
            // rungs and stop clips stand down while it works to flat.
            if (c.kind == ExecutionKind::CLOSE) {
                for (const auto& order : ours(ticker)) {
                    if (order.signal == view.signal) cancel(ticker, order.id);
                }
                end_trade_work(view.signal, true, true, "closed by command " + std::to_string(id));
            }
            command.symbol = view.symbol;
            command.jobs.push_back(job);
        }
        state_.active.push_back(std::move(command));
        report(id, Core::STRATEGY_COMMAND_WORKING, saved ? "resumed" : "started");
    }

    void apply(long long id, long long, const CancelExecution& c) {
        auto* command = active_by_id(c.command_id);
        if (!command || command->activity != Activity::EXECUTION) {
            throw Refused("no running execution " + std::to_string(c.command_id));
        }
        const std::string detail = "cancelled by command " + std::to_string(id);
        command->cancelled = detail;
        for (auto& job : command->jobs) {
            if (!job.ending) job.ending = {Core::STRATEGY_COMMAND_CANCELLED, detail};
        }
        report(id, Core::STRATEGY_COMMAND_DONE, "execution cancelled");
    }

    void apply(long long id, long long, const SetBrackets& c) {
        const auto view = view_of(c.trade);
        auto& ticker = ticker_for(view.symbol);
        std::lock_guard lock(ticker.trading_mutex);
        if (read(view.kv, kStopHitMs)) throw Refused(kStoppingRefusal);
        const auto last = quote_of(view.symbol, view.venue).last;
        if (!last) throw Refused("no price for " + view.symbol);
        auto stop = read_stop(view.kv);
        auto tp = read_tp(view.kv);
        if (c.stop) {
            if (!*c.stop) {
                stop.reset();
            } else if ((*c.stop)->price_only) {
                if (!stop) throw Refused(kNewStopNeedsClip);
                stop->price = (*c.stop)->policy.price;
            } else {
                stop = (*c.stop)->policy;
            }
        }
        if (c.stop_cap_bps) {
            if (!stop) throw Refused("stop_cap_bps needs a stop");
            stop->cross_bps = *c.stop_cap_bps;
        }
        if (c.tp) {
            if (!*c.tp) {
                tp.reset();
            } else if ((*c.tp)->price_only && tp) {
                int resting = 0;
                for (const auto& order : ours(ticker)) {
                    resting += order.signal == view.signal && order.role == Role::TAKE_PROFIT && !order.cancel_sent;
                }
                tp = remaining_ladder(view.kv, *tp, resting);
                tp->price = (*c.tp)->policy.price;
            } else {
                tp = (*c.tp)->policy;
            }
        }
        check_brackets(view.is_long(), c.stop || c.stop_cap_bps ? stop : std::nullopt, c.tp ? tp : std::nullopt,
                       *last, min_notional_of(view.symbol, view.venue));
        auto kv = view.kv;
        write_stop(kv, stop);
        write_tp(kv, tp);
        write_kv(ticker, view, kv);
        if (c.tp) state_.tp_failures.erase(view.signal);
        report(id, Core::STRATEGY_COMMAND_DONE, stop || tp ? "brackets set" : "brackets cleared");
    }

    void cancel_entries_everywhere(const std::string& detail) {
        std::set<unsigned long long> signals;
        for (const auto& [symbol, ticker] : tickers_) {
            std::lock_guard lock(ticker->trading_mutex);
            for (const auto& order : ours(*ticker)) {
                if (order.closing) continue;
                cancel(*ticker, order.id);
                signals.insert(order.signal);
            }
            for (const auto& trade : our_trades(*ticker)) signals.insert(trade.signal_id);
        }
        for (const auto& command : state_.active) {
            if (scope_ && command.account_id != scope_) continue;
            for (const auto& job : command.jobs) signals.insert(job.signal);
        }
        for (const auto signal : signals) end_trade_work(signal, true, false, detail);
    }

    void apply(long long id, long long, const PauseEntries&) {
        if (paused(scope_)) {
            report(id, Core::STRATEGY_COMMAND_DONE, "entries were already paused");
            return;
        }
        state_.active.push_back({.id = id, .activity = Activity::PAUSE, .start_ms = now_});
        cancel_entries_everywhere("entries paused");
        report(id, Core::STRATEGY_COMMAND_WORKING, "entries paused");
    }

    void apply(long long id, long long, const ResumeEntries&) {
        for (auto& command : state_.active) {
            if (command.activity != Activity::PAUSE || command.done || (scope_ && command.account_id != scope_)) continue;
            report(command.id, Core::STRATEGY_COMMAND_DONE, "resumed by command " + std::to_string(id));
            command.done = true;
        }
        report(id, Core::STRATEGY_COMMAND_DONE, "entries resumed");
    }

    void apply(long long id, long long, const FlattenAll& c) {
        const std::string detail = "flatten_all " + std::to_string(id);
        ActiveCommand command{.id = id, .activity = Activity::EXECUTION, .start_ms = now_,
                              .exec = Execution{.kind = ExecutionKind::CLOSE,
                                                .style = Style::ACCUMULATE,
                                                .cap_bps = c.cap_bps,
                                                .clip_usd = kCloseClipUsd,
                                                .every_ms = kCloseEveryMs,
                                                .ttl_ms = kUntilFlatTtlMs,
                                                .until_flat = true}};
        std::set<unsigned long long> signals;
        for (const auto& [symbol, ticker] : tickers_) {
            std::lock_guard lock(ticker->trading_mutex);
            for (const auto& order : ours(*ticker)) {
                cancel(*ticker, order.id);
                signals.insert(order.signal);
            }
            for (const auto& trade : our_trades(*ticker)) {
                signals.insert(trade.signal_id);
                auto kv = trade.kv_cache;
                write_tp(kv, std::nullopt);
                if (kv != trade.kv_cache) ticker->set_trade_kv_cache(trade, kv);
                const Core::ExchangeAndMarket venue{trade.exchange_name, trade.market_type, trade.account_id};
                const Side exit = opposite(side_of(trade));
                const auto quote = quote_of(symbol, venue);
                const auto arrival = ExecutionMath::touch(exit, quote);
                if (!arrival) continue;
                command.jobs.push_back({.signal = trade.signal_id, .symbol = symbol, .side = exit,
                                        .target = remaining_of(trade), .arrival = *arrival});
            }
        }
        for (const auto signal : signals) end_trade_work(signal, true, true, detail);
        const auto closing = command.jobs.size();
        if (closing == 0) {
            report(id, Core::STRATEGY_COMMAND_DONE, "nothing to close");
            return;
        }
        state_.active.push_back(std::move(command));
        report(id, Core::STRATEGY_COMMAND_WORKING, "closing " + std::to_string(closing) + " trades");
    }

    // A command still open when the engine stopped: work that spans time
    // picks up from what the tickers kept; one-shot commands may or may not
    // have run, so they are not run again.
    void resume(const Core::StrategyCommand& command, const Command& parsed) {
        const long long id = command.id;
        if (const auto* c = std::get_if<Execution>(&parsed)) {
            if (c->deadline_ms && now_ >= command.ts_ms + *c->deadline_ms) {
                report(id, Core::STRATEGY_COMMAND_DONE, "deadline passed while the engine was down");
                return;
            }
            const auto signal = c->kind == ExecutionKind::OPEN ? static_cast<unsigned long long>(id) : *c->trade;
            std::optional<Kv> saved;
            for (const auto& [symbol, ticker] : tickers_) {
                std::lock_guard lock(ticker->trading_mutex);
                if (const auto trade = trade_of(*ticker, signal)) saved = trade->kv_cache;
                for (const auto& order : ours(*ticker)) {
                    if (!saved && order.command_id == id && order.role == Role::EXECUTION) saved = order.kv;
                }
            }
            if (!saved && c->kind != ExecutionKind::OPEN) {
                report(id, Core::STRATEGY_COMMAND_DONE,
                       c->kind == ExecutionKind::ADD ? "trade closed" : "trade is flat");
                return;
            }
            start_execution(id, command.ts_ms, *c, saved ? &*saved : nullptr);
            return;
        }
        if (std::holds_alternative<PlaceOrder>(parsed)) {
            for (const auto& [symbol, ticker] : tickers_) {
                std::lock_guard lock(ticker->trading_mutex);
                for (const auto& order : ours(*ticker)) {
                    if (order.command_id != id || order.role != Role::RESTING) continue;
                    ActiveCommand resting{.id = id, .activity = Activity::RESTING, .start_ms = command.ts_ms,
                                          .symbol = symbol, .signal = order.signal};
                    if (!order.closing && !trade_of(*ticker, order.signal) &&
                        order.signal == static_cast<unsigned long long>(id)) {
                        Kv kv = order.kv;
                        for (const char* tag : {kCmd, kRole, kLevel}) kv.erase(tag);
                        resting.opening = Opening{symbol, order.side, order.venue, kv};
                    }
                    state_.active.push_back(std::move(resting));
                    return;
                }
            }
            report(id, Core::STRATEGY_COMMAND_DONE, "order closed while the engine was down");
            return;
        }
        if (std::holds_alternative<PauseEntries>(parsed)) {
            state_.active.push_back({.id = id, .activity = Activity::PAUSE, .start_ms = command.ts_ms});
            return;
        }
        if (const auto* c = std::get_if<FlattenAll>(&parsed)) {
            apply(id, command.ts_ms, *c);
            return;
        }
        report(id, Core::STRATEGY_COMMAND_INTERRUPTED, "the engine restarted before it ran");
    }

    // --- per-symbol pass ---

    // Returns false once the symbol holds nothing of ours.
    bool step_symbol(const std::string& symbol, Core::TradeTicker& ticker) {
        std::lock_guard lock(ticker.trading_mutex);
        for (const auto& order : ours(ticker)) {
            if (order.expires_ms > 0 && now_ >= order.expires_ms && !order.cancel_sent) cancel(ticker, order.id);
        }
        std::set<unsigned long long> exiting;
        std::set<unsigned long long> closing;
        // Entry quantity still to fill per trade; its take-profit lays it out ahead.
        std::map<unsigned long long, double> incoming;
        for (const auto& command : state_.active) {
            if (command.done || command.activity != Activity::EXECUTION) continue;
            for (const auto& job : command.jobs) {
                if (job.finished) continue;
                if (command.reducing()) {
                    exiting.insert(job.signal);
                    if (command.exec.kind == ExecutionKind::CLOSE) closing.insert(job.signal);
                } else if (job.target_usd > 0 && job.arrival > 0) {
                    incoming[job.signal] += std::max(0.0, job.target_usd - job.filled_usd) / job.arrival;
                } else {
                    incoming[job.signal] += std::max(0.0, job.target - job.filled);
                }
            }
        }
        for (const auto& trade : our_trades(ticker)) {
            const auto pending = incoming.find(trade.signal_id);
            work_trade(ticker, trade, exiting.contains(trade.signal_id), closing.contains(trade.signal_id),
                       pending == incoming.end() ? 0 : pending->second);
        }
        bool commands = false;
        for (auto& command : state_.active) {
            if (command.done) continue;
            if (command.activity == Activity::EXECUTION) {
                bool here = false;
                for (auto& job : command.jobs) {
                    if (job.symbol != symbol || job.finished) continue;
                    here = true;
                    step_job(ticker, command, job);
                }
                if (std::all_of(command.jobs.begin(), command.jobs.end(), [](const Job& j) { return j.finished; })) {
                    finish_execution(command);
                }
                commands = commands || here;
            } else if (command.activity == Activity::RESTING && command.symbol == symbol) {
                step_resting(ticker, command);
                commands = true;
            }
            if (command.opening && command.opening->symbol == symbol &&
                trade_of(ticker, static_cast<unsigned long long>(command.id))) {
                command.opening.reset();
            }
        }
        return record(symbol, ticker) || commands;
    }

    // A triggered stop: accumulation clips from the trigger until flat. While
    // the price is back on the safe side it posts nothing and resumes on the
    // next release once crossed again. A refused clip is skipped, never the stop.
    void work_stop(Core::TradeTicker& ticker, const Core::Trade& trade, const StopPolicy& stop, Kv& kv, Side exit,
                   const Core::ExchangeAndMarket& venue, const Quote& quote, double open_qty,
                   const std::vector<OurOrder>& resting, OrderRequest r) {
        if (quote.last && !ExecutionMath::stop_hit(exit == Side::SELL, stop.price, *quote.last)) {
            for (const auto& order : resting) cancel(ticker, order.id);
            return;
        }
        const long long hit = read_ms(kv, kStopHitMs);
        const auto plan = stop_plan(stop, hit, trade.signal_id);
        const int k = ExecutionMath::accumulate_clip_at(plan, now_);
        if (k <= static_cast<int>(read(kv, kStopClip).value_or(-1))) return;
        const auto clip = next_clip(
            ticker, {.plan = plan, .style = stop.clip_style, .cross_bps = stop.cross_bps,
                     .life_lo_ms = stop.clip_life_lo_ms, .life_hi_ms = stop.clip_life_hi_ms,
                     .ends_ms = now_ + kNeverExpiresMs},
            k, exit, ticker.symbol, venue, quote, open_qty, resting, [&](double price) {
                const double usd = stop.clip_usd > 0
                    ? stop.clip_usd
                    : read(kv, kStopStartQty).value_or(open_qty) * price / kLegacyStopClips;
                const double qty = usd * ExecutionMath::clip_factor(plan, k) / price;
                return std::max(qty, 1.1 * min_notional_of(ticker.symbol, venue) / price);
            });
        if (!clip) return;
        r.qty = clip->qty;
        r.price = clip->price;
        r.expires_ms = clip->expires_ms;
        r.post_only = clip->post_only;
        r.stop_loss = true;
        const auto sent = send(ticker, r, trade);
        if (sent.id) {
            kv[kStopClip] = k;
            kv[kStopLastPostMs] = static_cast<double>(now_);
            state_.stop_errors.erase(trade.signal_id);
            return;
        }
        if (sent.covered || !sent.failure || retriable(*sent.failure)) return;
        kv[kStopClip] = k;
        state_.stop_errors[trade.signal_id] = failure_text(sent);
    }

    // The trade's stop, else its take-profit ladder, from the policies in its kv.
    // While a close works the trade, the stop neither triggers nor clips; it
    // stays stored, so a cancelled close hands the trade back to it.
    void work_trade(Core::TradeTicker& ticker, const Core::Trade& trade, bool exiting, bool closing,
                    double incoming) {
        const auto signal = trade.signal_id;
        const Core::ExchangeAndMarket venue{trade.exchange_name, trade.market_type, trade.account_id};
        const Side exit = opposite(side_of(trade));
        const auto quote = quote_of(ticker.symbol, venue);
        const double open_qty = remaining_of(trade);
        const auto orders = ours(ticker);
        auto kv = trade.kv_cache;
        for (const char* tag : {kCmd, kRole, kLevel}) kv.erase(tag);
        const auto request = [&](Role role) {
            return OrderRequest{.signal = signal, .trade_side = side_of(trade), .venue = venue,
                                .role = role, .closing = true};
        };

        const auto stop = read_stop(kv);
        if (closing) {
            for (const auto& order : orders) {
                if (order.signal == signal && order.role == Role::STOP) cancel(ticker, order.id);
            }
        } else if (stop && !read(kv, kStopHitMs) && quote.last &&
            ExecutionMath::stop_hit(exit == Side::SELL, stop->price, *quote.last)) {
            kv[kStopHitMs] = static_cast<double>(now_);
            kv[kStopStartQty] = open_qty;
        }
        if (!closing && stop && read(kv, kStopHitMs)) {
            std::vector<OurOrder> resting;
            for (const auto& order : orders) {
                if (order.signal != signal) continue;
                if (order.role == Role::STOP) {
                    resting.push_back(order);
                } else {
                    cancel(ticker, order.id);
                }
            }
            end_trade_work(signal, true, true, "stop triggered");
            work_stop(ticker, trade, *stop, kv, exit, venue, quote, open_qty, resting, request(Role::STOP));
        } else {
            auto tp = read_tp(kv);
            // A reduce takes the rungs down; what is left lays out afresh after it.
            if (tp && exiting && read(kv, kTpNext).value_or(0) > 0) {
                int resting = 0;
                for (const auto& order : orders) {
                    resting += order.signal == signal && order.role == Role::TAKE_PROFIT && !order.cancel_sent;
                }
                tp = remaining_ladder(kv, *tp, resting);
                write_tp(kv, tp);
            }
            std::vector<OrderPosting::LiveOrder> live;
            double other_exits = 0;
            for (const auto& order : orders) {
                if (order.signal != signal || !order.closing) continue;
                if (order.role != Role::TAKE_PROFIT) {
                    other_exits += order.qty_left();
                } else if (!tp || exiting) {
                    cancel(ticker, order.id);
                } else {
                    live.push_back(order.live());
                }
            }
            if (tp && !exiting) {
                auto& [failures, error] = state_.tp_failures[signal];
                OrderPosting::Policy policy{
                    .price = OrderPosting::Ladder{.first = tp->price,
                                                  .step_bp = tp->step_bp,
                                                  .rungs = tp->rungs,
                                                  .visible = kTpVisibleRungs,
                                                  .size_jitter = tp->size_jitter},
                    .gap_lo_ms = tp->gap_lo_ms,
                    .gap_hi_ms = tp->gap_hi_ms};
                const double min_notional =
                    engine_.get_min_notional(ticker.symbol, venue.exchange_name, venue.market_type);
                const auto plan = OrderPostingPolicy::plan(
                    policy, {.side = posting_side(exit), .remaining_qty = open_qty - other_exits,
                             .incoming_qty = incoming, .next_rung = static_cast<int>(read(kv, kTpNext).value_or(0)),
                             .min_qty = min_notional / tp->price, .quote = posting_quote(quote), .now_ms = now_,
                             .last_post_ms = read_ms(kv, kTpLastPostMs), .seed = signal, .live = live,
                             .round = rounding(ticker.symbol, venue)});
                kv[kTpNext] = plan.next_rung;
                for (const auto id : plan.cancels) cancel(ticker, id);
                for (const auto& post : plan.posts) {
                    auto r = request(Role::TAKE_PROFIT);
                    r.level = post.level;
                    r.qty = post.qty;
                    r.price = post.price;
                    r.expires_ms = post.expires_ms;
                    r.post_only = true;
                    const auto sent = send(ticker, r, trade);
                    if (sent.id) {
                        failures = 0;
                        kv[kTpLastPostMs] = static_cast<double>(now_);
                        continue;
                    }
                    if (sent.covered || (sent.failure && retriable(*sent.failure))) break;
                    error = failure_text(sent);
                    if (++failures >= kMaxTakeProfitFailures) write_tp(kv, std::nullopt);
                    break;
                }
            }
        }
        if (kv != trade.kv_cache) ticker.set_trade_kv_cache(trade, kv);
    }

    // Quantity and notional the command has filled.
    static std::pair<double, double> command_fills(const std::optional<Core::Trade>& trade, long long command_id,
                                                   bool reducing) {
        if (!trade) return {0, 0};
        double qty = 0;
        double notional = 0;
        for (const auto& fill : reducing ? trade->close_fills : trade->open_fills) {
            if (read(fill.kv_cache, kCmd) != static_cast<double>(command_id)) continue;
            qty += fill.fill_amount.toDouble();
            notional += fill.fill_amount.toDouble() * fill.price.toDouble();
        }
        return {qty, notional};
    }

    static double filled_share(const Job& job) {
        return job.target_usd > 0 ? job.filled_usd / job.target_usd : job.filled / job.target;
    }

    void end_job(Job& job, const std::string& status, const std::string& detail) {
        if (!job.ending) job.ending = {status, detail};
    }

    void step_job(Core::TradeTicker& ticker, ActiveCommand& command, Job& job) {
        const auto trade = trade_of(ticker, job.signal);
        const bool reducing = command.reducing();
        std::vector<OurOrder> mine;
        for (const auto& order : ours(ticker)) {
            if (order.command_id == command.id && order.role == Role::EXECUTION && order.signal == job.signal) {
                mine.push_back(order);
            }
        }
        const auto clear_job = [&] {
            if (!trade || !read(trade->kv_cache, kJobCmd)) return;
            auto kv = trade->kv_cache;
            for (const char* key : {kJobCmd, kJobTarget, kJobTargetUsd, kJobArrival, kJobTaking, kJobClip}) kv.erase(key);
            ticker.set_trade_kv_cache(*trade, kv);
        };
        if (!job.ending) drive(ticker, command, job, trade, mine);
        if (job.ending) {
            for (const auto& order : mine) cancel(ticker, order.id);
            if (std::all_of(mine.begin(), mine.end(), [](const OurOrder& o) { return o.cancel_sent; })) {
                clear_job();
                job.finished = true;
            }
            return;
        }
        if (trade) {
            auto kv = trade->kv_cache;
            kv[kJobCmd] = static_cast<double>(command.id);
            kv[kJobTarget] = job.target;
            kv[kJobTargetUsd] = job.target_usd;
            kv[kJobArrival] = job.arrival;
            kv[kJobTaking] = job.taking ? 1 : 0;
            kv[kJobClip] = job.clip;
            if (kv != trade->kv_cache) ticker.set_trade_kv_cache(*trade, kv);
        } else if (command.opening) {
            command.opening->kv[kJobCmd] = static_cast<double>(command.id);
            command.opening->kv[kJobTarget] = job.target;
            command.opening->kv[kJobTargetUsd] = job.target_usd;
            command.opening->kv[kJobArrival] = job.arrival;
            command.opening->kv[kJobTaking] = job.taking ? 1 : 0;
            command.opening->kv[kJobClip] = job.clip;
        }
    }

    void drive(Core::TradeTicker& ticker, ActiveCommand& command, Job& job,
               const std::optional<Core::Trade>& trade, const std::vector<OurOrder>& mine) {
        const auto& spec = command.exec;
        const bool reducing = command.reducing();
        if (!trade && (reducing || !command.opening)) {
            end_job(job, Core::STRATEGY_COMMAND_DONE, reducing ? "trade is flat" : "trade closed");
            return;
        }
        std::tie(job.filled, job.filled_usd) = command_fills(trade, command.id, reducing);
        const auto venue = trade ? Core::ExchangeAndMarket{trade->exchange_name, trade->market_type, trade->account_id}
                                 : command.opening->venue;
        const auto quote = quote_of(job.symbol, venue);
        const double open_qty = trade ? remaining_of(*trade) : 0;
        double remaining = job.target - job.filled;
        if (job.target_usd > 0) {
            // The dollars left, at today's price. Clips fill at better prices
            // than they were sized at, so a crumb under the venue minimum or
            // 0.1% of the target counts as done.
            const double left_usd = job.target_usd - job.filled_usd;
            const double crumb = std::max(
                engine_.get_min_notional(job.symbol, venue.exchange_name, venue.market_type), 0.001 * job.target_usd);
            const auto price = ExecutionMath::touch(job.side, quote);
            remaining = left_usd < crumb ? 0 : left_usd / price.value_or(job.arrival);
        }
        if (reducing) remaining = std::min(remaining, open_qty);
        if (reducing && dust(open_qty, job.symbol, venue)) {
            end_job(job, Core::STRATEGY_COMMAND_DONE, "trade is flat");
            return;
        }
        if (remaining <= kQtyEpsilon) {
            end_job(job, Core::STRATEGY_COMMAND_DONE, "filled");
            return;
        }
        const Style base_style = spec.style == Style::ACCUMULATE ? spec.clip_style : spec.style;
        if (spec.deadline_ms && now_ >= command.start_ms + *spec.deadline_ms && !job.taking) {
            if (base_style == Style::PASSIVE && spec.take_at_deadline) {
                job.taking = true;
            } else {
                end_job(job, Core::STRATEGY_COMMAND_DONE, "deadline reached");
                return;
            }
        }
        const Style style = job.taking ? Style::TAKE : base_style;
        const bool accumulate = spec.style == Style::ACCUMULATE;
        if (accumulate && !spec.until_flat && now_ >= command.start_ms + spec.ttl_ms) {
            const auto pct = static_cast<int>(std::round(100 * filled_share(job)));
            end_job(job, Core::STRATEGY_COMMAND_DONE, "ttl ended " + std::to_string(pct) + "% filled");
            return;
        }
        if (accumulate) {
            drive_accumulate(ticker, command, job, trade, mine, venue, quote, remaining);
            return;
        }
        OrderPosting::Policy policy;
        if (style == Style::TAKE) {
            policy.price = OrderPosting::Marketable{job.arrival, spec.cap_bps, false, spec.limit_price};
        } else {
            policy.price = OrderPosting::Touch{spec.reprice_ms, spec.limit_price};
        }
        std::vector<OrderPosting::LiveOrder> live;
        for (const auto& order : mine) live.push_back(order.live());
        const auto plan = OrderPostingPolicy::plan(
            policy, {.side = posting_side(job.side), .remaining_qty = remaining, .quote = posting_quote(quote),
                     .now_ms = now_, .last_post_ms = job.last_post_ms,
                     .seed = static_cast<std::uint64_t>(command.id), .live = live,
                     .round = rounding(job.symbol, venue)});
        if (plan.beyond_cap) {
            end_job(job, job.filled > 0 ? Core::STRATEGY_COMMAND_DONE : Core::STRATEGY_COMMAND_FAILED,
                    "price moved beyond cap_bps from arrival");
            return;
        }
        for (const auto id : plan.cancels) cancel(ticker, id);
        for (const auto& post : plan.posts) {
            OrderRequest r{.signal = job.signal,
                           .trade_side = reducing ? opposite(job.side) : job.side,
                           .venue = venue,
                           .trade_kv = trade ? trade->kv_cache : command.opening->kv,
                           .command_id = command.id,
                           .role = Role::EXECUTION,
                           .closing = reducing,
                           .post_only = style == Style::PASSIVE,
                           .qty = post.qty,
                           .price = post.price,
                           .expires_ms = post.expires_ms};
            const auto sent = send(ticker, r, trade);
            if (sent.id) {
                job.last_post_ms = now_;
                continue;
            }
            if (sent.covered || !sent.failure || retriable(*sent.failure)) return;
            const bool below_min = *sent.failure == Core::MIN_NOTIONAL_FILTER_FAILED;
            const std::string reason = Core::order_construction_result_to_str(*sent.failure);
            if (job.filled > 0) {
                end_job(job, Core::STRATEGY_COMMAND_DONE,
                        below_min ? std::string("filled; the rest is below the venue's minimum notional")
                                  : "filled; the rest was not sent: " + reason);
            } else {
                end_job(job, Core::STRATEGY_COMMAND_FAILED, reason);
            }
            return;
        }
    }

    struct ClipSpec {
        AccumulationPlan plan;
        Style style = Style::TAKE;
        double cross_bps = 0;
        std::optional<double> limit;
        long long life_lo_ms = 0;
        long long life_hi_ms = 0;
        long long ends_ms = 0;
    };
    struct Clip {
        double price = 0;
        double qty = 0;
        long long expires_ms = 0;
        bool post_only = false;
    };

    // Clip k of a program once its resting clips are gone: taking at most
    // cross_bps through the touch, or joining it. `want` sizes it in base at a
    // price; a remainder too small to post on its own joins it. Nullopt while
    // a resting clip is being cancelled or the book gives no price.
    std::optional<Clip> next_clip(Core::TradeTicker& ticker, const ClipSpec& spec, int k, Side side,
                                  const std::string& symbol, const Core::ExchangeAndMarket& venue, const Quote& quote,
                                  double remaining, const std::vector<OurOrder>& resting,
                                  const std::function<double(double)>& want) {
        for (const auto& order : resting) cancel(ticker, order.id);
        if (!resting.empty()) return std::nullopt;

        const auto touch = ExecutionMath::touch(side, quote);
        if (!touch) return std::nullopt;
        double price = ExecutionMath::bound(side, *touch, spec.cross_bps, spec.limit);
        if (spec.style == Style::PASSIVE) {
            const auto join = ExecutionMath::join(side, quote);
            if (!join) return std::nullopt;
            price = *join;
            if (spec.limit && !ExecutionMath::within(side, price, *spec.limit)) price = *spec.limit;
        }
        price = rounding(symbol, venue)(price);

        double qty = std::min(want(price), remaining);
        if ((remaining - qty) * price < 1.1 * min_notional_of(symbol, venue)) qty = remaining;

        long long expires = std::min(ExecutionMath::accumulate_release_ms(spec.plan, k + 1), spec.ends_ms);
        if (spec.life_hi_ms > 0) {
            const auto life = OrderPosting::OrderPostingPolicy::draw(
                spec.plan.seed, static_cast<std::uint64_t>(k), kClipLifeSalt, static_cast<double>(spec.life_lo_ms),
                static_cast<double>(spec.life_hi_ms));
            expires = std::min(expires, now_ + static_cast<long long>(life));
        }
        // Backtest bars come a minute apart; a shorter life would cancel the
        // clip before the next bar could fill it.
        if (engine_.is_backtest) expires = std::max(expires, now_ + kBacktestBarMs + 1000);
        return Clip{.price = price, .qty = qty, .expires_ms = expires, .post_only = spec.style == Style::PASSIVE};
    }

    // An accumulation posts one clip per release, as soon as the randomized
    // rhythm allows, until the size fills or the ttl ends. A clip rests at its
    // price until its life ends or the next clip replaces it; what it leaves
    // unfilled is dropped, never added to a later clip.
    void drive_accumulate(Core::TradeTicker& ticker, const ActiveCommand& command, Job& job,
                          const std::optional<Core::Trade>& trade, const std::vector<OurOrder>& mine,
                          const Core::ExchangeAndMarket& venue, const Quote& quote, double remaining) {
        const auto& spec = command.exec;
        const bool reducing = command.reducing();
        const AccumulationPlan plan{.start_ms = command.start_ms,
                                    .ttl_ms = spec.ttl_ms,
                                    .clip_usd = spec.clip_usd,
                                    .clip_jitter = spec.clip_jitter,
                                    .every_ms = spec.every_ms,
                                    .every_jitter = spec.every_jitter,
                                    .seed = static_cast<std::uint64_t>(command.id)};
        const int k = ExecutionMath::accumulate_clip_at(plan, now_);
        job.next_clip_ms = ExecutionMath::accumulate_release_ms(plan, k + 1);

        const auto last = quote.last ? quote.last : ExecutionMath::touch(job.side, quote);
        const double bound = job.side == Side::BUY ? job.arrival * (1 + spec.max_slippage_bps / 1e4)
                                                   : job.arrival * (1 - spec.max_slippage_bps / 1e4);
        job.held = !spec.until_flat && (!last || !ExecutionMath::within(job.side, *last, bound));
        if (job.held) {
            // Clips due while the price is out of bounds are skipped, not saved.
            for (const auto& order : mine) cancel(ticker, order.id);
            job.clip = std::max(job.clip, k);
            return;
        }
        if (k <= job.clip) return;
        const auto clip = next_clip(
            ticker, {.plan = plan, .style = spec.clip_style, .cross_bps = spec.cap_bps, .limit = spec.limit_price,
                     .life_lo_ms = spec.clip_life_lo_ms, .life_hi_ms = spec.clip_life_hi_ms,
                     .ends_ms = command.start_ms + spec.ttl_ms},
            k, job.side, job.symbol, venue, quote, remaining, mine,
            [&](double price) { return ExecutionMath::accumulate_clip_usd(plan, k) / price; });
        if (!clip) return;

        OrderRequest r{.signal = job.signal,
                       .trade_side = reducing ? opposite(job.side) : job.side,
                       .venue = venue,
                       .trade_kv = trade ? trade->kv_cache : command.opening->kv,
                       .command_id = command.id,
                       .role = Role::EXECUTION,
                       .closing = reducing,
                       .post_only = clip->post_only,
                       .qty = clip->qty,
                       .price = clip->price,
                       .expires_ms = clip->expires_ms};
        const auto sent = send(ticker, r, trade);
        if (sent.id) {
            job.clip = k;
            job.last_post_ms = now_;
            return;
        }
        if (sent.covered || !sent.failure || retriable(*sent.failure)) return;
        job.clip = k;
        const std::string reason = Core::order_construction_result_to_str(*sent.failure);
        if (*sent.failure == Core::MIN_NOTIONAL_FILTER_FAILED) {
            // Acceptance checked the clips, so only a remainder falls under the minimum.
            end_job(job, job.filled > 0 ? Core::STRATEGY_COMMAND_DONE : Core::STRATEGY_COMMAND_FAILED,
                    job.filled > 0 ? "filled; the rest is below the venue's minimum notional" : reason);
            return;
        }
        end_job(job, job.filled > 0 ? Core::STRATEGY_COMMAND_DONE : Core::STRATEGY_COMMAND_FAILED,
                job.filled > 0 ? "filled in part; a clip was refused: " + reason : reason);
    }

    void finish_execution(ActiveCommand& command) {
        if (command.done) return;
        command.done = true;
        if (command.cancelled) {
            report(command.id, Core::STRATEGY_COMMAND_CANCELLED, *command.cancelled);
            return;
        }
        if (command.jobs.size() == 1 && command.jobs.front().ending) {
            const auto& [status, detail] = *command.jobs.front().ending;
            report(command.id, status, detail);
            return;
        }
        report(command.id, Core::STRATEGY_COMMAND_DONE,
               "closed " + std::to_string(command.jobs.size()) + " trades");
    }

    void step_resting(Core::TradeTicker& ticker, ActiveCommand& command) {
        std::vector<OurOrder> mine;
        for (const auto& order : ours(ticker)) {
            if (order.command_id == command.id && order.role == Role::RESTING) mine.push_back(order);
        }
        if (command.replacement) {
            auto& replacement = *command.replacement;
            const auto old = std::find_if(mine.begin(), mine.end(), [&](const OurOrder& o) {
                return o.id == replacement.order_id;
            });
            if (old != mine.end()) {
                replacement.qty_left = old->qty_left();
                if (!old->cancel_sent) cancel(ticker, old->id);
                return;
            }
            auto r = replacement.request;
            r.qty = replacement.qty.value_or(replacement.qty_left);
            command.replacement.reset();
            if (command.cancelled || r.qty <= kQtyEpsilon) return;
            const auto sent = send(ticker, r, trade_of(ticker, r.signal));
            if (!sent.id) command.cancelled = "replacement failed: " + failure_text(sent);
            return;
        }
        if (!mine.empty()) {
            if (command.cancelled) {
                for (const auto& order : mine) cancel(ticker, order.id);
            }
            return;
        }
        command.done = true;
        if (command.cancelled) {
            report(command.id, Core::STRATEGY_COMMAND_CANCELLED, *command.cancelled);
        } else {
            report(command.id, Core::STRATEGY_COMMAND_DONE, "order closed");
        }
    }

    // --- console book ---

    static const char* role_text(Role role) {
        switch (role) {
            case Role::TAKE_PROFIT: return "take_profit";
            case Role::STOP: return "stop";
            case Role::EXECUTION: return "execution";
            case Role::RESTING: return "resting";
        }
        return "resting";
    }

    nlohmann::json trade_json(unsigned long long signal, const std::string& symbol, Side side,
                              const Core::ExchangeAndMarket& venue, const Kv& kv,
                              const std::optional<Core::Trade>& trade) const {
        const auto failure = state_.tp_failures.find(signal);
        const bool tp_failed = failure != state_.tp_failures.end() && !failure->second.second.empty();
        const auto hit = read(kv, kStopHitMs);
        return {{"signal_id", signal},
                {"symbol", symbol},
                {"side", side == Side::BUY ? "long" : "short"},
                {"exchange", venue.exchange_name},
                {"market_type", venue.market_type},
                {"account_id", venue.account_id ? nlohmann::json(*venue.account_id) : nlohmann::json(nullptr)},
                {"setup_id", static_cast<long long>(read(kv, kSetup).value_or(0))},
                {"ev_expectation", read(kv, kEv).value_or(0)},
                {"trade_id", trade ? nlohmann::json(std::to_string(trade->id)) : nlohmann::json(nullptr)},
                {"qty", trade ? remaining_of(*trade) : 0.0},
                {"avg_price", trade ? nlohmann::json(trade->get_average_open_price().toDouble())
                                    : nlohmann::json(nullptr)},
                {"stop", stop_json(read_stop(kv), kv, signal,
                                   state_.stop_errors.contains(signal) ? state_.stop_errors.at(signal) : "")},
                {"tp", tp_json(read_tp(kv), kv)},
                {"stop_hit_ms", hit ? nlohmann::json(static_cast<long long>(*hit)) : nlohmann::json(nullptr)},
                {"tp_error", tp_failed ? nlohmann::json(failure->second.second) : nlohmann::json(nullptr)}};
    }

    // Returns true when the symbol holds trades or orders of ours.
    bool record(const std::string& symbol, Core::TradeTicker& ticker) {
        const auto trades = our_trades(ticker);
        std::set<unsigned long long> open;
        for (const auto& trade : trades) {
            open.insert(trade.signal_id);
            trades_.push_back(trade_json(trade.signal_id, symbol, side_of(trade),
                                         {trade.exchange_name, trade.market_type, trade.account_id},
                                         trade.kv_cache, trade));
        }
        for (const auto& command : state_.active) {
            const auto signal = static_cast<unsigned long long>(command.id);
            if (command.done || !command.opening || command.opening->symbol != symbol || open.contains(signal)) {
                continue;
            }
            trades_.push_back(trade_json(signal, symbol, command.opening->side, command.opening->venue,
                                         command.opening->kv, std::nullopt));
        }
        const auto orders = ours(ticker);
        for (const auto& o : orders) {
            orders_.push_back({{"order_id", std::to_string(o.id)}, {"command_id", o.command_id},
                               {"signal_id", o.signal}, {"symbol", symbol}, {"side", side_text(o.side)},
                               {"qty", o.qty}, {"filled", o.filled}, {"price", o.price},
                               {"reduce_only", o.closing}, {"post_only", o.post_only},
                               {"role", role_text(o.role)}, {"level", o.level},
                               {"expires_ms", o.expires_ms > 0 ? nlohmann::json(o.expires_ms)
                                                               : nlohmann::json(nullptr)}});
        }
        for (const auto& command : state_.active) {
            if (command.done || command.activity != Activity::EXECUTION) continue;
            const auto& spec = command.exec;
            const char* kind = spec.kind == ExecutionKind::OPEN    ? "open"
                             : spec.kind == ExecutionKind::ADD     ? "add"
                             : spec.kind == ExecutionKind::REDUCE  ? "reduce"
                                                                   : "close";
            const char* style = spec.style == Style::TAKE ? "take" : spec.style == Style::PASSIVE ? "passive" : "accumulate";
            const bool accumulate = spec.style == Style::ACCUMULATE;
            for (const auto& job : command.jobs) {
                if (job.symbol != symbol || job.finished) continue;
                jobs_.push_back({{"command_id", command.id}, {"signal_id", job.signal}, {"symbol", symbol},
                                 {"kind", kind}, {"style", style}, {"side", side_text(job.side)},
                                 {"target", job.target}, {"filled", job.filled}, {"held", job.held},
                                 {"target_usd", job.target_usd > 0 ? nlohmann::json(job.target_usd) : nlohmann::json(nullptr)},
                                 {"filled_usd", job.filled_usd},
                                 {"clip", job.clip + 1},
                                 {"next_ms", accumulate && job.next_clip_ms < command.start_ms + spec.ttl_ms
                                                 ? nlohmann::json(job.next_clip_ms)
                                                 : nlohmann::json(nullptr)},
                                 {"until_flat", spec.until_flat},
                                 {"ends_ms", accumulate && !spec.until_flat ? nlohmann::json(command.start_ms + spec.ttl_ms)
                                                        : nlohmann::json(nullptr)},
                                 {"deadline_ms", spec.deadline_ms ? nlohmann::json(command.start_ms + *spec.deadline_ms)
                                                                  : nlohmann::json(nullptr)}});
            }
        }
        return !trades.empty() || !orders.empty();
    }
};

// A backtest replays a command file and may write the strategy's reports to
// a JSONL trace; live, StateUpdater feeds and persists both.
void run_backtest_io(long long now_ms) {
    static std::once_flag loaded;
    static std::mutex trace_mutex;
    static std::ofstream trace;
    std::call_once(loaded, [] {
        if (const char* path = std::getenv("DISCRETIONARY_COMMANDS_FILE")) {
            Core::StrategyCommandInbox::instance().load_replay_file(path);
        }
        if (const char* path = std::getenv("DISCRETIONARY_STATUS_FILE")) {
            trace.open(path);
            Core::StrategyCommandInbox::instance().set_reporting_enabled(true);
        }
    });
    auto& inbox = Core::StrategyCommandInbox::instance();
    inbox.release_due(now_ms);
    if (!trace.is_open()) return;
    std::lock_guard lock(trace_mutex);
    for (const auto& link : inbox.take_order_links()) {
        trace << nlohmann::json{{"ts_ms", now_ms}, {"command_id", link.command_id},
                                {"order_id", std::to_string(link.order_id)}}
                     .dump()
              << '\n';
    }
    for (const auto& r : inbox.take_reports()) {
        trace << nlohmann::json{{"ts_ms", now_ms}, {"command_id", r.command_id}, {"strategy_id", r.strategy_id},
                                {"status", r.status}, {"detail", r.detail}}
                     .dump()
              << '\n';
    }
    trace.flush();
}

}  // namespace

std::vector<MarketData::BarClosePolicy> get_strategy_required_bar_types() {
    return {MarketData::ONE_MIN_EXCHANGE_BAR};
}

void compute_predictions(Core::Engine&, Core::Strategy&) {}

std::vector<Core::StrategyDecision> on_market_event_callback(
    Core::Engine& engine, Core::Strategy& strategy, std::optional<Core::MarketEvent>) {
    auto& state = state_for(strategy.strategy_config.id);
    std::lock_guard lock(state.mutex);
    const auto now = engine.get_time();
    if (now - state.last_pass_ms < kPassMinIntervalMs) return {};
    state.last_pass_ms = now;
    if (engine.is_backtest) run_backtest_io(now);
    Runner runner(engine, strategy, state);
    for (const auto& command :
         Core::StrategyCommandInbox::instance().drain(strategy.strategy_config.id)) {
        runner.handle(command);
    }
    runner.step();
    Core::StrategyCommandInbox::instance().publish_book(strategy.strategy_config.id,
                                                        runner.book_state());
    if (engine.is_backtest) run_backtest_io(now);
    return runner.decisions();
}

}  // namespace AlgorithmDiscretionary

namespace {
void bind_discretionary(Core::Strategy& strategy) {
    strategy.strategy_callback = AlgorithmDiscretionary::on_market_event_callback;
    strategy.compute_predictions_callback =
        AlgorithmDiscretionary::compute_predictions;
    strategy.required_bar_types =
        AlgorithmDiscretionary::get_strategy_required_bar_types();
}
}  // namespace

KERONIS_REGISTER_STRATEGY_MATCHER(Core::accepts_strategy_commands,
                                  bind_discretionary);
