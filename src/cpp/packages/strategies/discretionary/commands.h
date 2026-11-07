#pragma once

#include <cmath>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <variant>

#include "nlohmann/json.hpp"

namespace Discretionary {

enum class Side { BUY, SELL };
enum class Style { TAKE, PASSIVE, ACCUMULATE };
enum class OrderFilter { ALL, ENTRY, REDUCE_ONLY };
enum class ExecutionKind { OPEN, ADD, REDUCE, CLOSE };

// Fat-finger bounds. No one command adds more than this much exposure, a
// limit may cross the touch by at most kMaxCrossPct, and a bracket sits
// within kMaxBracketPct of the price.
inline constexpr double kMaxCommandNotionalUsd = 100'000;
inline constexpr double kMaxCrossPct = 2;
inline constexpr double kMaxBracketPct = 50;
inline constexpr int kMaxTpRungs = 500;
// Take-profit rungs resting at once.
inline constexpr int kTpVisibleRungs = 50;
inline constexpr double kMaxCapBps = 200;
inline constexpr double kMaxSlippageBps = 1000;
inline constexpr const char* kNewStopNeedsClip = "a new stop needs clip_usd; send the full stop";
// A close with no style works to flat in taker clips that follow the market,
// with no ttl and no slippage hold: a close never gives up.
inline constexpr double kCloseClipUsd = 5000;
inline constexpr long long kCloseEveryMs = 3000;
inline constexpr double kCloseCrossBps = 30;
inline constexpr long long kUntilFlatTtlMs = 10LL * 365 * 86'400'000;

// Exactly one is set. `fraction` (0, 1] of the trade applies to reductions;
// `risk_pct` of NAV lost at the trade's stop applies to entries.
struct Size {
    std::optional<double> qty;
    std::optional<double> notional;
    std::optional<double> risk_pct;
    std::optional<double> fraction;
};

// Exits over `rungs` resting post-only limits, rung k k * step_bp past
// `price`, each about an even share of the position (+- size_jitter). At most
// kTpVisibleRungs rest at once, nearest first; after the opening batch the
// next rung posts one gap after the last. The defaults rest one order for the
// whole position at `price`.
struct TakeProfitPolicy {
    double price = 0;
    int rungs = 1;
    double step_bp = 0;
    double size_jitter = 0.3;
    long long gap_lo_ms = 5000;
    long long gap_hi_ms = 15000;
};

// Once the last price crosses `price`, the trade leaves in accumulation clips
// of clip_usd (+- clip_jitter) released every every_ms (+- every_jitter)
// until it is flat. While the price is back on the safe side it posts nothing.
struct StopPolicy {
    double price = 0;
    long long every_ms = 3000;
    double every_jitter = 0.5;
    double clip_usd = 0;
    double clip_jitter = 0.3;
    Style clip_style = Style::TAKE;
    double cross_bps = 30;
    long long clip_life_lo_ms = 0;
    long long clip_life_hi_ms = 0;
};

// A bare number moves only the price and keeps the trade's other settings;
// an object replaces them all.
template <class Policy>
struct Bracket {
    Policy policy;
    bool price_only = false;
};

// What every new trade must declare. The setup is a discretionary_setup row;
// the EV is the expected return in percent of entry notional.
struct NewTrade {
    std::string symbol;
    Side side;
    long long setup_id;
    double ev_pct;
    std::optional<long long> account_id;
    std::optional<std::string> exchange;
    std::optional<StopPolicy> stop;
    std::optional<TakeProfitPolicy> tp;
};

// A resting limit: the entry of a new trade, an add to `trade`, or with
// `reduce_only` an exit of `trade`.
struct PlaceOrder {
    std::optional<NewTrade> open;
    std::optional<unsigned long long> trade;
    Size size;
    double price;
    bool reduce_only = false;
    bool post_only = false;
    std::optional<long long> expires_ms;
};

// Targets an order by engine order id or by the command that placed it.
struct OrderTarget {
    std::optional<unsigned long long> order_id;
    std::optional<long long> command_id;
};

struct CancelOrder {
    OrderTarget target;
};

struct CancelAllOrders {
    std::optional<std::string> symbol;
    std::optional<unsigned long long> trade;
    OrderFilter filter = OrderFilter::ALL;
};

struct ReplaceOrder {
    OrderTarget target;
    std::optional<double> price;
    std::optional<double> qty;
};

// Durations are milliseconds from acceptance. OPEN carries `open`; the other
// kinds act on `trade`.
struct Execution {
    ExecutionKind kind;
    std::optional<NewTrade> open;
    std::optional<unsigned long long> trade;
    Size size;
    Style style = Style::TAKE;
    double cap_bps = 50;
    std::optional<double> limit_price;
    std::optional<long long> deadline_ms;
    long long reprice_ms = 5000;
    bool take_at_deadline = true;
    // ACCUMULATE: clips of about clip_usd every about every_ms until the
    // size fills or ttl_ms passes; clip_jitter and every_jitter bound the
    // draws, and are never zero.
    Style clip_style = Style::TAKE;
    double clip_usd = 0;
    double clip_jitter = 0.3;
    long long every_ms = 0;
    double every_jitter = 0.5;
    long long ttl_ms = 3'600'000;
    // No clips while the last price sits beyond arrival moved this far
    // against us; the program keeps running and resumes inside it.
    double max_slippage_bps = 100;
    // Life of a clip, drawn per clip; zero rests until the next clip, which
    // always replaces it.
    long long clip_life_lo_ms = 0;
    long long clip_life_hi_ms = 0;
    // An accumulation that ignores ttl_ms and max_slippage_bps and ends only
    // when the trade is flat.
    bool until_flat = false;
};

struct CancelExecution {
    long long command_id;
};

// An absent bracket is left as it is; a null one is cleared.
struct SetBrackets {
    unsigned long long trade;
    std::optional<std::optional<Bracket<StopPolicy>>> stop;
    std::optional<std::optional<Bracket<TakeProfitPolicy>>> tp;
    std::optional<double> stop_cap_bps;
};

struct PauseEntries {};
struct ResumeEntries {};
struct FlattenAll {
    double cap_bps = 50;
};

using Command = std::variant<PlaceOrder, CancelOrder, CancelAllOrders,
                             ReplaceOrder, Execution, CancelExecution,
                             SetBrackets, PauseEntries, ResumeEntries,
                             FlattenAll>;

class CommandParser {
   public:
    // Throws std::invalid_argument with a message fit for the operator.
    static Command parse(const std::string& op, const nlohmann::json& p) {
        if (op == "place_order") {
            PlaceOrder c{.size = size(p, false, flag(p, "reduce_only")),
                         .price = positive(p, "price")};
            c.reduce_only = flag(p, "reduce_only");
            c.post_only = flag(p, "post_only");
            c.expires_ms = duration(p, "expires_ms");
            if (p.contains("signal_id")) {
                c.trade = signal_id(p);
            } else if (c.reduce_only) {
                fail("a reduce-only order needs signal_id");
            } else {
                c.open = new_trade(p);
            }
            if (c.size.risk_pct && c.open && !c.open->stop) {
                fail("risk_pct needs stop");
            }
            return c;
        }
        if (op == "cancel_order") return CancelOrder{target(p)};
        if (op == "cancel_all_orders") {
            CancelAllOrders c;
            if (p.contains("symbol")) c.symbol = symbol(p);
            if (p.contains("signal_id")) c.trade = signal_id(p);
            const auto filter = text(p, "filter", "all");
            if (filter == "entry") c.filter = OrderFilter::ENTRY;
            else if (filter == "reduce_only") c.filter = OrderFilter::REDUCE_ONLY;
            else if (filter != "all") fail("filter must be all, entry or reduce_only");
            return c;
        }
        if (op == "replace_order") {
            ReplaceOrder c{.target = target(p)};
            if (p.contains("price")) c.price = positive(p, "price");
            if (p.contains("qty")) c.qty = positive(p, "qty");
            if (!c.price && !c.qty) fail("replace_order needs price or qty");
            return c;
        }
        if (op == "open_trade" || op == "add_to_trade" || op == "reduce_trade" ||
            op == "close_trade") {
            return execution(op, p);
        }
        if (op == "cancel_execution") {
            return CancelExecution{integer(p, "command_id")};
        }
        if (op == "set_brackets") {
            SetBrackets c{.trade = signal_id(p)};
            if (p.contains("stop")) {
                c.stop = p.at("stop").is_null() ? std::optional<Bracket<StopPolicy>>{}
                                                : std::optional{stop_bracket(p.at("stop"))};
            }
            if (p.contains("tp")) {
                c.tp = p.at("tp").is_null() ? std::optional<Bracket<TakeProfitPolicy>>{}
                                            : std::optional{tp_bracket(p.at("tp"))};
            }
            if (p.contains("stop_cap_bps")) c.stop_cap_bps = cross(p, "stop_cap_bps");
            if (!c.stop && !c.tp && !c.stop_cap_bps) {
                fail("set_brackets needs stop, tp or stop_cap_bps (null clears)");
            }
            return c;
        }
        if (op == "pause_entries") return PauseEntries{};
        if (op == "resume_entries") return ResumeEntries{};
        if (op == "flatten_all") {
            FlattenAll c;
            if (p.contains("cap_bps")) c.cap_bps = cross(p, "cap_bps");
            return c;
        }
        fail("unknown op " + op);
    }

   private:
    [[noreturn]] static void fail(const std::string& message) {
        throw std::invalid_argument(message);
    }

    static double number(const nlohmann::json& p, const char* key) {
        if (!p.contains(key) || !p.at(key).is_number()) {
            fail(std::string(key) + " must be a number");
        }
        const double value = p.at(key).get<double>();
        if (!std::isfinite(value)) fail(std::string(key) + " must be finite");
        return value;
    }

    static double positive(const nlohmann::json& p, const char* key) {
        const double value = number(p, key);
        if (value <= 0) fail(std::string(key) + " must be positive");
        return value;
    }

    static double cross(const nlohmann::json& p, const char* key) {
        const double value = positive(p, key);
        if (value > kMaxCapBps) fail(std::string(key) + " must be at most 200");
        return value;
    }

    // Randomization is not optional: a fixed size or rhythm is readable.
    static double jitter(const nlohmann::json& p, const char* key) {
        const double value = number(p, key);
        if (value < 0.1 || value > 0.9) fail(std::string(key) + " must be 0.1 to 0.9");
        return value;
    }

    static long long integer(const nlohmann::json& p, const char* key) {
        if (!p.contains(key) || !p.at(key).is_number_integer()) {
            fail(std::string(key) + " must be an integer");
        }
        return p.at(key).get<long long>();
    }

    static std::optional<long long> duration(const nlohmann::json& p,
                                             const char* key) {
        if (!p.contains(key) || p.at(key).is_null()) return std::nullopt;
        const auto value = integer(p, key);
        if (value <= 0) fail(std::string(key) + " must be positive");
        return value;
    }

    static bool flag(const nlohmann::json& p, const char* key) {
        if (!p.contains(key)) return false;
        if (!p.at(key).is_boolean()) fail(std::string(key) + " must be true or false");
        return p.at(key).get<bool>();
    }

    static std::string text(const nlohmann::json& p, const char* key,
                            const std::string& fallback) {
        if (!p.contains(key)) return fallback;
        if (!p.at(key).is_string()) fail(std::string(key) + " must be a string");
        return p.at(key).get<std::string>();
    }

    static std::string symbol(const nlohmann::json& p) {
        const auto value = text(p, "symbol", "");
        if (value.empty()) fail("symbol is required");
        return value;
    }

    static Side side(const nlohmann::json& p) {
        const auto value = text(p, "side", "");
        if (value == "buy") return Side::BUY;
        if (value == "sell") return Side::SELL;
        fail("side must be buy or sell");
    }

    // A trade's signal_id is the id of the command that opened it.
    static unsigned long long signal_id(const nlohmann::json& p) {
        const auto value = integer(p, "signal_id");
        if (value <= 0) fail("signal_id must be positive");
        return static_cast<unsigned long long>(value);
    }

    // [lo, hi], or one number for both.
    static std::pair<double, double> range(const nlohmann::json& p, const char* key,
                                           double scale) {
        const auto& v = p.at(key);
        const auto bound = [&](const nlohmann::json& x) {
            if (!x.is_number() || !std::isfinite(x.get<double>()) || x.get<double>() < 0) {
                fail(std::string(key) + " must be non-negative numbers");
            }
            return x.get<double>() * scale;
        };
        if (v.is_number()) return {bound(v), bound(v)};
        if (!v.is_array() || v.size() != 2) fail(std::string(key) + " must be [low, high]");
        const auto lo = bound(v[0]);
        const auto hi = bound(v[1]);
        if (lo > hi) fail(std::string(key) + " low must not exceed high");
        return {lo, hi};
    }

    static Bracket<StopPolicy> stop_bracket(const nlohmann::json& v) {
        if (v.is_number()) {
            return {.policy = {.price = positive(nlohmann::json{{"stop", v}}, "stop")}, .price_only = true};
        }
        if (!v.is_object()) fail("stop must be a price or an object");
        for (const char* gone : {"cap_bps", "gap_s", "escalate_s", "deadline_s"}) {
            if (v.contains(gone)) {
                fail(std::string("stop ") + gone +
                     " was replaced: stop takes price, clip_usd, clip_jitter, every_s, every_jitter, clip_style, "
                     "cross_bps, clip_life_s");
            }
        }
        if (!v.contains("clip_usd")) fail("a stop needs clip_usd");
        StopPolicy s{.price = positive(v, "price")};
        if (v.contains("every_s")) {
            const double every = positive(v, "every_s");
            if (every < 1) fail("every_s must be at least 1");
            s.every_ms = static_cast<long long>(every * 1000);
        }
        if (v.contains("every_jitter")) s.every_jitter = jitter(v, "every_jitter");
        if (!v.at("clip_usd").is_number()) fail("stop clip_usd is one number, with clip_jitter");
        s.clip_usd = positive(v, "clip_usd");
        if (v.contains("clip_jitter")) s.clip_jitter = jitter(v, "clip_jitter");
        s.clip_style = style(v, "clip_style", Style::TAKE);
        if (s.clip_style == Style::ACCUMULATE) fail("clip_style must be take or passive");
        if (v.contains("cross_bps")) s.cross_bps = cross(v, "cross_bps");
        if (v.contains("clip_life_s")) {
            const auto [lo, hi] = range(v, "clip_life_s", 1000);
            if (lo < 1000) fail("clip_life_s must be at least 1");
            s.clip_life_lo_ms = static_cast<long long>(lo);
            s.clip_life_hi_ms = static_cast<long long>(hi);
        }
        return {.policy = s};
    }

    static Bracket<TakeProfitPolicy> tp_bracket(const nlohmann::json& v) {
        if (v.is_number()) {
            return {.policy = {.price = positive(nlohmann::json{{"tp", v}}, "tp")}, .price_only = true};
        }
        if (!v.is_object()) fail("tp must be a price or an object");
        for (const char* gone : {"levels", "clip_usd", "ttl_s"}) {
            if (v.contains(gone)) {
                fail(std::string("tp ") + gone + " was replaced: tp takes price, rungs, step_bp, size_jitter, gap_s");
            }
        }
        TakeProfitPolicy t{.price = positive(v, "price")};
        if (v.contains("rungs")) {
            t.rungs = static_cast<int>(integer(v, "rungs"));
            if (t.rungs < 1 || t.rungs > kMaxTpRungs) fail("rungs must be 1 to " + std::to_string(kMaxTpRungs));
        }
        if (v.contains("step_bp")) t.step_bp = positive(v, "step_bp");
        if (t.rungs > 1 && t.step_bp <= 0) fail("a take-profit of several rungs needs step_bp");
        if (v.contains("size_jitter")) t.size_jitter = jitter(v, "size_jitter");
        if (v.contains("gap_s")) {
            const auto [lo, hi] = range(v, "gap_s", 1000);
            t.gap_lo_ms = static_cast<long long>(lo);
            t.gap_hi_ms = static_cast<long long>(hi);
        }
        return {.policy = t};
    }

    static NewTrade new_trade(const nlohmann::json& p) {
        NewTrade t{.symbol = symbol(p), .side = side(p),
                   .setup_id = integer(p, "setup_id"),
                   .ev_pct = number(p, "ev_expectation")};
        if (t.setup_id <= 0) fail("setup_id must be positive");
        if (p.contains("account_id")) t.account_id = integer(p, "account_id");
        if (p.contains("exchange")) t.exchange = text(p, "exchange", "");
        if (p.contains("stop") && !p.at("stop").is_null()) {
            const auto stop = stop_bracket(p.at("stop"));
            if (stop.price_only) fail(kNewStopNeedsClip);
            t.stop = stop.policy;
        }
        if (p.contains("tp") && !p.at("tp").is_null()) t.tp = tp_bracket(p.at("tp")).policy;
        if (p.contains("stop_cap_bps")) {
            if (!t.stop) fail("stop_cap_bps needs stop");
            t.stop->cross_bps = cross(p, "stop_cap_bps");
        }
        return t;
    }

    static Style style(const nlohmann::json& p, const char* key, Style fallback) {
        if (!p.contains(key)) return fallback;
        const auto value = text(p, key, "");
        if (value == "take") return Style::TAKE;
        if (value == "passive") return Style::PASSIVE;
        if (value == "accumulate") return Style::ACCUMULATE;
        if (value == "twap") fail("twap was replaced by accumulate (clip_usd, every_s, ttl_s)");
        fail(std::string(key) + " must be take, passive or accumulate");
    }

    static Size size(const nlohmann::json& p, bool required_none, bool reducing) {
        Size s;
        int count = 0;
        for (const char* key : {"qty", "notional", "risk_pct", "fraction"}) {
            if (!p.contains(key)) continue;
            ++count;
            const double value = positive(p, key);
            if (std::string(key) == "qty") s.qty = value;
            if (std::string(key) == "notional") s.notional = value;
            if (std::string(key) == "risk_pct") s.risk_pct = value;
            if (std::string(key) == "fraction") s.fraction = value;
        }
        if (required_none) {
            if (count != 0) fail("close_trade closes the whole trade; give no size");
            return s;
        }
        if (count != 1) fail("give exactly one of qty, notional, risk_pct, fraction");
        if (s.fraction && (!reducing || *s.fraction > 1)) {
            fail("fraction is a share (0, 1] of the trade when reducing");
        }
        if (s.risk_pct && (reducing || *s.risk_pct > 100)) {
            fail("risk_pct is a percent of NAV for entries");
        }
        return s;
    }

    static OrderTarget target(const nlohmann::json& p) {
        OrderTarget t;
        if (p.contains("order_id")) t.order_id = integer(p, "order_id");
        if (p.contains("command_id")) t.command_id = integer(p, "command_id");
        if (t.order_id.has_value() == t.command_id.has_value()) {
            fail("give exactly one of order_id, command_id");
        }
        return t;
    }

    static Execution execution(const std::string& op, const nlohmann::json& p) {
        Execution c{.kind = op == "open_trade"     ? ExecutionKind::OPEN
                            : op == "add_to_trade" ? ExecutionKind::ADD
                            : op == "reduce_trade" ? ExecutionKind::REDUCE
                                                   : ExecutionKind::CLOSE};
        const bool reducing = c.kind == ExecutionKind::REDUCE || c.kind == ExecutionKind::CLOSE;
        if (c.kind == ExecutionKind::OPEN) {
            c.open = new_trade(p);
        } else {
            c.trade = signal_id(p);
        }
        c.size = size(p, c.kind == ExecutionKind::CLOSE, reducing);
        c.until_flat = c.kind == ExecutionKind::CLOSE && !p.contains("style") && !p.contains("cap_bps") &&
                       !p.contains("limit_price") && !p.contains("deadline_ms");
        c.style = style(p, "style", c.until_flat ? Style::ACCUMULATE : Style::TAKE);
        if (p.contains("cap_bps")) {
            c.cap_bps = positive(p, "cap_bps");
            if (c.cap_bps > kMaxCapBps) fail("cap_bps must be at most 200");
        }
        if (p.contains("limit_price")) c.limit_price = positive(p, "limit_price");
        c.deadline_ms = duration(p, "deadline_ms");
        if (const auto reprice = duration(p, "reprice_ms")) {
            if (*reprice < 1000) fail("reprice_ms must be at least 1000");
            c.reprice_ms = *reprice;
        }
        if (p.contains("take_at_deadline")) {
            c.take_at_deadline = flag(p, "take_at_deadline");
        }
        if (c.style == Style::ACCUMULATE) {
            if (p.contains("cap_bps")) fail("accumulate prices clips by cross_bps");
            if (c.deadline_ms) fail("accumulate ends by ttl_s, not deadline_ms");
            c.cap_bps = p.contains("cross_bps") ? positive(p, "cross_bps") : c.until_flat ? kCloseCrossBps : 10;
            if (c.cap_bps > kMaxCapBps) fail("cross_bps must be at most 200");
            c.clip_style = style(p, "clip_style", Style::TAKE);
            if (c.clip_style == Style::ACCUMULATE) fail("clip_style must be take or passive");
            if (!p.contains("clip_usd") && !c.until_flat) fail("accumulate needs clip_usd");
            c.clip_usd = p.contains("clip_usd") ? positive(p, "clip_usd") : kCloseClipUsd;
            if (!p.contains("every_s") && !c.until_flat) fail("accumulate needs every_s");
            const double every = p.contains("every_s") ? positive(p, "every_s") : kCloseEveryMs / 1000.0;
            if (every < 2) fail("every_s must be at least 2");
            c.every_ms = static_cast<long long>(every * 1000);
            if (p.contains("clip_jitter")) c.clip_jitter = jitter(p, "clip_jitter");
            if (p.contains("every_jitter")) c.every_jitter = jitter(p, "every_jitter");
            if (c.until_flat && (p.contains("ttl_s") || p.contains("max_slippage_bps"))) {
                fail("a close works until flat; give a style to bound it by ttl_s or max_slippage_bps");
            }
            if (c.until_flat) c.ttl_ms = kUntilFlatTtlMs;
            if (p.contains("ttl_s")) {
                const double ttl = positive(p, "ttl_s");
                if (ttl < 10 || ttl > 86'400) fail("ttl_s must be 10 to 86400");
                c.ttl_ms = static_cast<long long>(ttl * 1000);
            }
            if (p.contains("max_slippage_bps")) {
                c.max_slippage_bps = positive(p, "max_slippage_bps");
                if (c.max_slippage_bps < 1 || c.max_slippage_bps > kMaxSlippageBps) {
                    fail("max_slippage_bps must be 1 to 1000");
                }
            }
            if (p.contains("clip_life_s")) {
                const auto [lo, hi] = range(p, "clip_life_s", 1000);
                if (lo < 1000) fail("clip_life_s must be at least 1");
                c.clip_life_lo_ms = static_cast<long long>(lo);
                c.clip_life_hi_ms = static_cast<long long>(hi);
            }
            for (const char* gone : {"duration_ms", "slices", "slice_style", "size_jitter", "time_jitter",
                                     "max_slippage_pct", "slice_ttl_s"}) {
                if (p.contains(gone)) fail(std::string(gone) + " is a twap field; accumulate takes clip_usd, every_s, ttl_s");
            }
        }
        if (c.size.risk_pct && c.kind == ExecutionKind::OPEN && !c.open->stop) {
            fail("risk_pct needs stop");
        }
        return c;
    }
};

}  // namespace Discretionary
