#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "live_infra/paper_trading.h"
#include "market_data/depth_history.h"
#include "market_making/dtypes/order.h"
#include "market_making/dtypes/order_res.h"
#include "nlohmann/json.hpp"

namespace Live {

// What the paper exchange trades against and reports to. The live engine wires
// these to its order book, market data and order-state path; tests use fakes.
struct PaperVenue {
    // False while no paper strategy is loaded; the exchange then stays idle.
    std::function<bool()> has_paper_strategies;
    // Every open engine order; the exchange keeps the paper strategies' ones.
    std::function<std::vector<Core::Order>()> open_orders;
    std::function<bool(int strategy_id)> is_paper_strategy;
    // Ack-to-fill latency for venues whose fills walk recorded depth (Lighter);
    // nullopt for venues judged on the quote.
    std::function<std::optional<long long>(const Core::Order &)> depth_latency_ms;
    std::function<std::optional<MarketData::DepthSnapshot>(const Core::Order &,
                                                           long long ts_ms)>
        book_at;
    std::function<PaperQuote(const Core::Order &, long long now_ms)> quote;
    std::function<double(const Core::Order &, bool is_maker)> fee_rate;
    // The order-state entry point exchange reports go through.
    std::function<void(const Core::OrderResNormalized &)> report;
    // `order` is null for errors outside any one order.
    std::function<void(const Core::Order *order, const std::string &message,
                       const nlohmann::json &metadata, bool is_error)>
        log;
    double slippage_bps = 3.0;
};

// The exchange paper strategies trade on. An order is judged once as the
// aggressor at its due time (ack + latency on depth venues, the ack elsewhere);
// a remainder that rests is judged as a maker once a second against the quote
// and the trades it has seen since the exchange took it, and once more against
// those trades when it is canceled. Fills and cancels come back through
// `PaperVenue::report`, as a real venue's stream reports would.
class PaperExchange {
   public:
    static constexpr long long kRestingCheckIntervalMs = 1000;
    // Books are published a few ms after they are received; waiting this long
    // past the due time lets the book that stood at it arrive.
    static constexpr long long kSettleMs = 25;
    // A depth snapshot older than this at the fill time no longer describes
    // the book (Lighter publishes every ~50ms while the feed is healthy).
    static constexpr long long kDepthMaxAgeMs = 2000;

    explicit PaperExchange(PaperVenue venue);
    ~PaperExchange();
    PaperExchange(const PaperExchange &) = delete;
    PaperExchange &operator=(const PaperExchange &) = delete;

    // An acked paper order enters the exchange.
    void submit(const Core::Order &order, long long ack_ms);
    // True when the cancel takes effect now: the order has not reached its due
    // time. Otherwise false: the order executes first (a resting order fills if
    // a trade before the request went through its limit), and its remainder is
    // canceled by report.
    bool cancel(unsigned long long order_id, long long request_ms);
    void on_trade(const std::string &exchange_market_symbol, double price);

    // Judges everything due by `now_ms`. Open paper orders the exchange has
    // not seen (restored from disk) are adopted from their ack time.
    void tick(long long now_ms);

    void start();
    void stop();
    // Wall-clock ms of the worker's last pass; 0 before the first.
    long long last_tick_ms() const { return last_tick_ms_.load(); }

    static std::string range_key(const Core::Order &order);

   private:
    struct Pending {
        long long due_ms;
        bool depth;
    };
    // RETRY: no price to judge the order on yet.
    enum class Outcome { CLOSED, RESTING, RETRY };

    long long due_ms(const Core::Order &order, long long ack_ms) const;
    Outcome first_check(const Core::Order &order, const Pending &pending,
                        long long now_ms);
    Outcome depth_check(const Core::Order &order, long long fill_time_ms);
    Outcome quote_first_check(const Core::Order &order, long long now_ms);
    // True when the order filled.
    bool resting_check(const Core::Order &order, long long now_ms, bool canceled);
    void track(const Core::Order &order);
    void untrack_gone(const std::unordered_map<unsigned long long,
                                               const Core::Order *> &open);
    void report_fill(const Core::Order &order, double cumulative, double price,
                     const char *status, double fee_qty, bool is_maker,
                     long long fill_time_ms, const char *fee_currency);
    void report_canceled(const Core::Order &order, long long now_ms);
    long long next_wake_ms() const;
    void run(std::stop_token stop);

    PaperVenue venue_;

    mutable std::mutex mutex_;
    std::unordered_map<unsigned long long, Pending> pending_;
    std::unordered_set<unsigned long long> in_flight_;
    std::unordered_set<unsigned long long> resting_;
    std::unordered_set<unsigned long long> cancel_deferred_;
    // Orders this exchange closed or canceled that the engine still lists;
    // they must not be adopted again.
    std::unordered_set<unsigned long long> closed_;
    bool woken_ = false;
    long long next_resting_check_ms_ = 0;

    // Lowest and highest trade price each order has seen since the exchange
    // took it: a print through a resting limit fills it. A cancel freezes the
    // range, so later prints cannot fill a canceled order. Lock order: mutex_,
    // then ranges_mutex_.
    struct TradeRange {
        std::string key;
        double low = 0.0;
        double high = 0.0;
        bool seen = false;
        bool frozen = false;
    };
    std::mutex ranges_mutex_;
    std::unordered_map<unsigned long long, TradeRange> ranges_;
    std::unordered_map<std::string, std::unordered_set<unsigned long long>>
        orders_by_key_;

    std::condition_variable_any wake_;
    std::atomic<long long> last_tick_ms_{0};
    std::jthread worker_;
};

}  // namespace Live
