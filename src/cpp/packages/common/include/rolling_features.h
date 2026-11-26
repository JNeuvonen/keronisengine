#pragma once
//
// Time-windowed rolling feature primitives.
//
// Each class maintains internal state across calls and exposes O(amortized 1)
// `update(...)` + `value()`. Windows are specified in MILLISECONDS and refer
// to a sliding time-based window over input timestamps, not a fixed count of
// pushes — this is what matches polars `rolling_*_by(ts, "<W>s")` semantics
// used in the training-side transformations under
// `data_library/transformations/stored/`.
//
// Boundary semantics: polars' default `closed="right"` includes
// `(now - window_ms, now]`. So we evict entries where
// `front.ts <= now_ts - window_ms` after each push.
//
// Pair with the stateless span-based queries in `technical_analysis.h` for
// one-shot lookups; this header is the streaming counterpart used inside a
// stateful SM that ticks once per bar.
//
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <deque>
#include <optional>
#include <utility>

namespace RollingFeatures {

// ---------------------------------------------------------------------------
// RollingSum<T> — sum over the trailing `window_ms` of pushed values.
//
// Emits a partial sum from the first push (matches polars
// `rolling_sum_by(ts, period)` which returns a partial value before the
// window is fully covered). Returns nullopt only when no push has happened.
// ---------------------------------------------------------------------------
template <typename T>
class RollingSum {
   public:
    explicit RollingSum(long long window_ms) : window_ms_(window_ms) {}

    void push(long long ts_ms, T x) {
        buf_.push_back({ts_ms, x});
        sum_ += x;
        long long cutoff = ts_ms - window_ms_;
        while (!buf_.empty() && buf_.front().first <= cutoff) {
            sum_ -= buf_.front().second;
            buf_.pop_front();
        }
    }

    std::optional<double> value() const {
        if (buf_.empty()) return std::nullopt;
        return static_cast<double>(sum_);
    }

    std::size_t count() const { return buf_.size(); }
    bool empty() const { return buf_.empty(); }

   private:
    long long window_ms_;
    std::deque<std::pair<long long, T>> buf_;
    T sum_ = T(0);
};

// ---------------------------------------------------------------------------
// RollingMonotonicMinMax — sliding-window min and max over a trailing
// time-window via two monotonic deques. Amortized O(1) per push.
// ---------------------------------------------------------------------------
class RollingMonotonicMinMax {
   public:
    explicit RollingMonotonicMinMax(long long window_ms) : window_ms_(window_ms) {}

    void push(long long ts_ms, double x) {
        // Min deque — monotonically increasing front-to-back. Drop stale tail
        // entries that this new sample dominates, then evict by ts.
        while (!min_dq_.empty() && min_dq_.back().value > x) min_dq_.pop_back();
        min_dq_.push_back({ts_ms, x});
        long long cutoff = ts_ms - window_ms_;
        while (!min_dq_.empty() && min_dq_.front().ts <= cutoff)
            min_dq_.pop_front();

        // Max deque — monotonically decreasing front-to-back.
        while (!max_dq_.empty() && max_dq_.back().value < x) max_dq_.pop_back();
        max_dq_.push_back({ts_ms, x});
        while (!max_dq_.empty() && max_dq_.front().ts <= cutoff)
            max_dq_.pop_front();
    }

    std::optional<double> min() const {
        if (min_dq_.empty()) return std::nullopt;
        return min_dq_.front().value;
    }
    std::optional<double> max() const {
        if (max_dq_.empty()) return std::nullopt;
        return max_dq_.front().value;
    }

   private:
    struct Entry {
        long long ts;
        double value;
    };
    long long window_ms_;
    std::deque<Entry> min_dq_;
    std::deque<Entry> max_dq_;
};

// ---------------------------------------------------------------------------
// RollingMeanStd — running mean and (sample, ddof=1) std over the trailing
// time-window. Matches polars `rolling_mean_by` and default `rolling_std_by`.
// std() returns nullopt until 2+ samples are present.
// ---------------------------------------------------------------------------
class RollingMeanStd {
   public:
    explicit RollingMeanStd(long long window_ms) : window_ms_(window_ms) {}

    void push(long long ts_ms, double x) {
        buf_.push_back({ts_ms, x});
        sum_ += x;
        sum_sq_ += x * x;
        long long cutoff = ts_ms - window_ms_;
        while (!buf_.empty() && buf_.front().first <= cutoff) {
            double v = buf_.front().second;
            sum_ -= v;
            sum_sq_ -= v * v;
            buf_.pop_front();
        }
    }

    std::optional<double> mean() const {
        if (buf_.empty()) return std::nullopt;
        return sum_ / static_cast<double>(buf_.size());
    }
    // Sample std (ddof=1). Requires at least 2 samples; polars returns null
    // for n < 2 with default ddof.
    std::optional<double> std() const {
        if (buf_.size() < 2) return std::nullopt;
        double n = static_cast<double>(buf_.size());
        double m = sum_ / n;
        double var_pop = std::max(sum_sq_ / n - m * m, 0.0);
        // ddof=1 correction.
        return std::sqrt(var_pop * n / (n - 1.0));
    }

    std::size_t count() const { return buf_.size(); }

   private:
    long long window_ms_;
    std::deque<std::pair<long long, double>> buf_;
    double sum_ = 0.0;
    double sum_sq_ = 0.0;
};

// ---------------------------------------------------------------------------
// RollingClosePast — asof-backward close lookup at `last_ts - window_ms`.
// Matches polars `join_asof(strategy="backward")` semantics used by the ROCP
// transformation: at ts=t, return the close from the most recent bar with
// ts' <= t - window_ms. Returns nullopt until such a past exists.
//
// Internally we keep at most ONE entry with ts <= target plus all younger
// entries (ts > target), so memory is bounded at ~window_ms / typical_step.
// ---------------------------------------------------------------------------
class RollingClosePast {
   public:
    explicit RollingClosePast(long long window_ms) : window_ms_(window_ms) {}

    void push(long long ts_ms, double close) {
        last_ts_ = ts_ms;
        buf_.push_back({ts_ms, close});
        long long target = ts_ms - window_ms_;
        // Advance head: while the *next* entry is also still <= target,
        // pop the current head (the next one is a better, more-recent past).
        while (buf_.size() >= 2 && buf_[1].first <= target) {
            buf_.pop_front();
        }
    }

    std::optional<double> past() const {
        if (buf_.empty()) return std::nullopt;
        long long target = last_ts_ - window_ms_;
        if (buf_.front().first <= target) {
            return buf_.front().second;
        }
        return std::nullopt;
    }

   private:
    long long window_ms_;
    long long last_ts_ = 0;
    std::deque<std::pair<long long, double>> buf_;
};

// ---------------------------------------------------------------------------
// Composed feature accumulators — purpose-built to mirror the python
// transformations under `data_library/transformations/stored/`. Each
// `update(ts_ms, ...)` takes the bar's open-time in ms (matches python's
// `ts_col = kline_open_time_ms`).
// ---------------------------------------------------------------------------

// OFI(W) = rolling_sum_by(signed_vol, ts, W) / rolling_sum_by(volume, ts, W)
// where signed_vol = taker_buy_base * 2 - volume.
class RollingOFI {
   public:
    explicit RollingOFI(long long window_ms)
        : signed_(window_ms), vol_(window_ms) {}
    void update(long long ts_ms, double volume, double taker_buy_base_volume) {
        signed_.push(ts_ms, taker_buy_base_volume * 2.0 - volume);
        vol_.push(ts_ms, volume);
    }
    std::optional<double> value() const {
        auto s = signed_.value();
        auto v = vol_.value();
        if (!s || !v) return std::nullopt;
        return *s / std::max(*v, 1e-12);
    }

   private:
    RollingSum<double> signed_;
    RollingSum<double> vol_;
};

// Parkinson(W) = sqrt(sum_by(log_hl_sq, ts, W) / (4*ln2 * n_by(ts, W)))
// where n_by is the rolling count of bars in the time-window.
class RollingParkinson {
   public:
    explicit RollingParkinson(long long window_ms) : log_hl_sq_(window_ms) {}
    void update(long long ts_ms, double high, double low) {
        double hi = std::max(high, 1e-12);
        double lo = std::max(low, 1e-12);
        double r = std::log(hi / lo);
        log_hl_sq_.push(ts_ms, r * r);
    }
    std::optional<double> value() const {
        auto s = log_hl_sq_.value();
        if (!s) return std::nullopt;
        double n =
            std::max(static_cast<double>(log_hl_sq_.count()), 1.0);
        double four_ln2 = 4.0 * std::log(2.0);
        return std::sqrt(*s / (four_ln2 * n));
    }

   private:
    RollingSum<double> log_hl_sq_;
};

// ROCP(W) = log(close[t]) - log(close[asof-backward at t - W]).
class RollingROCP {
   public:
    explicit RollingROCP(long long window_ms) : past_(window_ms) {}
    void update(long long ts_ms, double close) {
        last_close_ = close;
        past_.push(ts_ms, close);
    }
    std::optional<double> value() const {
        auto p = past_.past();
        if (!p) return std::nullopt;
        if (last_close_ <= 0.0 || *p <= 0.0) return std::nullopt;
        return std::log(last_close_) - std::log(*p);
    }

   private:
    double last_close_ = 0.0;
    RollingClosePast past_;
};

// range_pos(W) = (close - rolling_min(close, W)) / (rolling_max - rolling_min)
class RollingRangePos {
   public:
    explicit RollingRangePos(long long window_ms) : minmax_(window_ms) {}
    void update(long long ts_ms, double close) {
        last_close_ = close;
        minmax_.push(ts_ms, close);
    }
    std::optional<double> value() const {
        auto mn = minmax_.min();
        auto mx = minmax_.max();
        if (!mn || !mx) return std::nullopt;
        double denom = std::max(*mx - *mn, 1e-12);
        return (last_close_ - *mn) / denom;
    }

   private:
    double last_close_ = 0.0;
    RollingMonotonicMinMax minmax_;
};

// Burst feature block — 5 outputs per window with a 24h z-score baseline.
// Rolling burst features for swap trade streams.
class RollingBurst {
   public:
    RollingBurst(long long window_ms, int window_sec, long long norm_ms)
        : window_sec_(window_sec),
          ats_window_sum_(window_ms),
          buy_count_(window_ms),
          sell_count_(window_ms),
          bar_count_(window_ms),
          max_trades_(window_ms),
          ats_baseline_(norm_ms),
          nt_baseline_(norm_ms) {}

    void update(long long ts_ms, double volume, long num_trades,
                double taker_buy_base_volume) {
        double ats = volume / std::max(static_cast<double>(num_trades), 1.0);
        double buy_frac = taker_buy_base_volume / std::max(volume, 1e-12);

        ats_window_sum_.push(ts_ms, ats);
        buy_count_.push(ts_ms, buy_frac >= 0.95 ? 1.0 : 0.0);
        sell_count_.push(ts_ms, buy_frac <= 0.05 ? 1.0 : 0.0);
        bar_count_.push(ts_ms, 1.0);
        max_trades_.push(ts_ms, static_cast<double>(num_trades));
        ats_baseline_.push(ts_ms, ats);
        nt_baseline_.push(ts_ms, static_cast<double>(num_trades));
    }

    struct Values {
        double avg_trade_size_z;
        double max_trades_z;
        double one_sided_buy_frac;
        double one_sided_sell_frac;
        double bar_density;
    };

    std::optional<Values> value() const {
        auto ats_sum = ats_window_sum_.value();
        auto buy = buy_count_.value();
        auto sell = sell_count_.value();
        auto bar_count = bar_count_.value();
        auto max_trades = max_trades_.max();
        auto ats_mean_24h = ats_baseline_.mean();
        auto ats_std_24h = ats_baseline_.std();
        auto nt_mean_24h = nt_baseline_.mean();
        auto nt_std_24h = nt_baseline_.std();
        if (!ats_sum || !buy || !sell || !bar_count || !max_trades ||
            !ats_mean_24h || !ats_std_24h || !nt_mean_24h || !nt_std_24h)
            return std::nullopt;

        double ats_std_safe = std::max(*ats_std_24h, 1e-12);
        double nt_std_safe = std::max(*nt_std_24h, 1e-12);
        double bar_count_safe = std::max(*bar_count, 1.0);
        double ats_mean_w = *ats_sum / bar_count_safe;

        Values v;
        v.avg_trade_size_z = (ats_mean_w - *ats_mean_24h) / ats_std_safe;
        v.max_trades_z = (*max_trades - *nt_mean_24h) / nt_std_safe;
        v.one_sided_buy_frac = *buy / bar_count_safe;
        v.one_sided_sell_frac = *sell / bar_count_safe;
        v.bar_density = bar_count_safe / static_cast<double>(window_sec_);
        return v;
    }

   private:
    int window_sec_;
    RollingSum<double> ats_window_sum_;
    RollingSum<double> buy_count_;
    RollingSum<double> sell_count_;
    RollingSum<double> bar_count_;
    RollingMonotonicMinMax max_trades_;
    RollingMeanStd ats_baseline_;
    RollingMeanStd nt_baseline_;
};

// Time-of-day / day-of-week sinusoidal features from epoch ms.
struct TimeFeatures {
    double tod_sin;
    double tod_cos;
    double dow_sin;
};
inline TimeFeatures time_features_at(long long ts_ms) {
    constexpr double TWO_PI = 6.283185307179586;
    long long sec_of_day = (ts_ms / 1000) % 86400;
    long long day_of_week = (ts_ms / 86400000) % 7;
    double tod = sec_of_day / 86400.0 * TWO_PI;
    double dow = day_of_week / 7.0 * TWO_PI;
    return {std::sin(tod), std::cos(tod), std::sin(dow)};
}

}  // namespace RollingFeatures
