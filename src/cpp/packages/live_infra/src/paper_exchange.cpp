#include "live_infra/paper_exchange.h"

#include <algorithm>
#include <chrono>
#include <exception>

#include "datetime_utils.h"
#include "market_making/dtypes/constants.h"

Live::PaperExchange::PaperExchange(PaperVenue venue)
    : venue_(std::move(venue)) {}

Live::PaperExchange::~PaperExchange() { stop(); }

std::string Live::PaperExchange::range_key(const Core::Order &order) {
    return order.exchange_name + "|" + order.market_type + "|" + order.symbol;
}

long long Live::PaperExchange::due_ms(const Core::Order &order,
                                      long long ack_ms) const {
    const auto latency = venue_.depth_latency_ms(order);
    return ack_ms + latency.value_or(0);
}

void Live::PaperExchange::submit(const Core::Order &order, long long ack_ms) {
    {
        std::lock_guard lock(mutex_);
        if (pending_.contains(order.id) || in_flight_.contains(order.id) ||
            resting_.contains(order.id) || closed_.contains(order.id)) {
            return;
        }
        pending_[order.id] = {due_ms(order, ack_ms),
                              venue_.depth_latency_ms(order).has_value()};
        track(order);
        woken_ = true;
    }
    wake_.notify_one();
}

bool Live::PaperExchange::cancel(unsigned long long order_id,
                                 long long request_ms) {
    {
        std::lock_guard lock(mutex_);
        // Closed orders already have their final report on the way.
        if (closed_.contains(order_id)) return false;
        const auto it = pending_.find(order_id);
        const bool executing =
            in_flight_.contains(order_id) || resting_.contains(order_id) ||
            (it != pending_.end() && it->second.due_ms <= request_ms);
        if (!executing) {
            if (it != pending_.end()) pending_.erase(it);
            closed_.insert(order_id);
            return true;
        }
        {
            std::lock_guard ranges_lock(ranges_mutex_);
            const auto range = ranges_.find(order_id);
            if (range != ranges_.end()) range->second.frozen = true;
        }
        cancel_deferred_.insert(order_id);
        woken_ = true;
    }
    wake_.notify_one();
    return false;
}

void Live::PaperExchange::on_trade(const std::string &exchange_market_symbol,
                                   double price) {
    if (!(price > 0)) return;
    std::lock_guard lock(ranges_mutex_);
    const auto ids = orders_by_key_.find(exchange_market_symbol);
    if (ids == orders_by_key_.end()) return;
    for (const auto id : ids->second) {
        auto &range = ranges_.at(id);
        if (range.frozen) continue;
        range.low = range.seen ? std::min(range.low, price) : price;
        range.high = range.seen ? std::max(range.high, price) : price;
        range.seen = true;
    }
}

void Live::PaperExchange::track(const Core::Order &order) {
    std::lock_guard lock(ranges_mutex_);
    const auto [it, inserted] = ranges_.try_emplace(order.id);
    if (!inserted) return;
    it->second.key = range_key(order);
    orders_by_key_[it->second.key].insert(order.id);
}

void Live::PaperExchange::untrack_gone(
    const std::unordered_map<unsigned long long, const Core::Order *> &open) {
    std::lock_guard lock(ranges_mutex_);
    for (auto it = ranges_.begin(); it != ranges_.end();) {
        if (open.contains(it->first)) {
            ++it;
            continue;
        }
        const auto ids = orders_by_key_.find(it->second.key);
        if (ids != orders_by_key_.end()) {
            ids->second.erase(it->first);
            if (ids->second.empty()) orders_by_key_.erase(ids);
        }
        it = ranges_.erase(it);
    }
}

void Live::PaperExchange::tick(long long now_ms) {
    last_tick_ms_.store(now_ms);
    if (!venue_.has_paper_strategies()) return;
    const auto orders = venue_.open_orders();
    std::unordered_map<unsigned long long, const Core::Order *> paper;
    for (const auto &order : orders) {
        if (order.is_order_creation_acknowledged &&
            venue_.is_paper_strategy(order.strategy_id)) {
            paper[order.id] = &order;
        }
    }

    std::vector<std::pair<const Core::Order *, Pending>> due;
    std::vector<const Core::Order *> resting;
    std::vector<const Core::Order *> canceled_resting;
    bool resting_due = false;
    {
        std::lock_guard lock(mutex_);
        const auto gone = [&](unsigned long long id) {
            return !paper.contains(id);
        };
        std::erase_if(pending_, [&](const auto &e) { return gone(e.first); });
        std::erase_if(resting_, gone);
        std::erase_if(closed_, gone);
        std::erase_if(cancel_deferred_, gone);

        for (const auto &[id, order] : paper) {
            if (pending_.contains(id) || in_flight_.contains(id) ||
                resting_.contains(id) || closed_.contains(id)) {
                continue;
            }
            const long long ack_ms = order->ack_create_timestamp > 0
                                         ? order->ack_create_timestamp
                                         : order->create_timestamp;
            pending_[id] = {due_ms(*order, ack_ms),
                            venue_.depth_latency_ms(*order).has_value()};
            track(*order);
        }
        for (auto it = cancel_deferred_.begin(); it != cancel_deferred_.end();) {
            if (!resting_.contains(*it)) {
                ++it;
                continue;
            }
            resting_.erase(*it);
            closed_.insert(*it);
            canceled_resting.push_back(paper.at(*it));
            it = cancel_deferred_.erase(it);
        }
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (it->second.due_ms + kSettleMs <= now_ms) {
                due.emplace_back(paper.at(it->first), it->second);
                in_flight_.insert(it->first);
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
        if (now_ms >= next_resting_check_ms_) {
            next_resting_check_ms_ = now_ms + kRestingCheckIntervalMs;
            resting_due = true;
            for (const auto id : resting_) resting.push_back(paper.at(id));
        }
    }
    untrack_gone(paper);

    for (const auto *order : canceled_resting) {
        try {
            if (!resting_check(*order, now_ms, true)) report_canceled(*order, now_ms);
        } catch (const std::exception &e) {
            venue_.log(order, std::string("Paper cancel check failed: ") + e.what(),
                       nlohmann::json::object(), true);
            report_canceled(*order, now_ms);
        }
    }

    std::vector<std::pair<const Core::Order *, Outcome>> outcomes;
    for (const auto &[order, pending] : due) {
        Outcome outcome = Outcome::CLOSED;
        try {
            outcome = first_check(*order, pending, now_ms);
        } catch (const std::exception &e) {
            venue_.log(order, std::string("Paper first check failed: ") + e.what(),
                       nlohmann::json::object(), true);
        }
        outcomes.emplace_back(order, outcome);
    }

    std::vector<const Core::Order *> cancels;
    {
        std::lock_guard lock(mutex_);
        for (const auto &[order, outcome] : outcomes) {
            in_flight_.erase(order->id);
            const bool deferred = cancel_deferred_.erase(order->id) > 0;
            if (outcome == Outcome::CLOSED) {
                closed_.insert(order->id);
            } else if (deferred) {
                closed_.insert(order->id);
                cancels.push_back(order);
            } else if (outcome == Outcome::RESTING) {
                resting_.insert(order->id);
            } else {
                pending_[order->id] = {now_ms + kRestingCheckIntervalMs, false};
            }
        }
    }
    for (const auto *order : cancels) report_canceled(*order, now_ms);

    if (resting_due) {
        for (const auto *order : resting) {
            try {
                resting_check(*order, now_ms, false);
            } catch (const std::exception &e) {
                venue_.log(order,
                           std::string("Paper resting check failed: ") + e.what(),
                           nlohmann::json::object(), true);
            }
        }
    }
}

Live::PaperExchange::Outcome Live::PaperExchange::first_check(
    const Core::Order &order, const Pending &pending, long long now_ms) {
    if (order.quantity - order.filled_quantity <= 0) return Outcome::CLOSED;
    return pending.depth ? depth_check(order, pending.due_ms)
                         : quote_first_check(order, now_ms);
}

void Live::PaperExchange::report_fill(const Core::Order &order,
                                      double cumulative, double price,
                                      const char *status, double fee_qty,
                                      bool is_maker, long long fill_time_ms,
                                      const char *fee_currency) {
    Core::OrderResNormalized res;
    res.symbol = order.get_order_api_symbol();
    res.client_order_id = order.client_order_id;
    res.status = status;
    res.price = price;
    res.orig_quantity = order.quantity;
    res.filled_quantity = cumulative;
    res.timestamp = order.create_timestamp;
    res.last_updated_timestamp = fill_time_ms;
    res.last_trade_timestamp = fill_time_ms;
    res.fee = Core::OrderResFees{
        .currency = fee_currency,
        .amount = mpfr::mpreal(fee_qty) * price * venue_.fee_rate(order, is_maker)};
    res.is_maker = is_maker;
    venue_.report(res);
}

void Live::PaperExchange::report_canceled(const Core::Order &order,
                                          long long now_ms) {
    Core::OrderResNormalized res;
    res.symbol = order.get_order_api_symbol();
    res.client_order_id = order.client_order_id;
    res.status = Core::ORDER_STATUS_NOT_WORKING;
    res.price = order.price;
    res.orig_quantity = order.quantity;
    res.filled_quantity = order.filled_quantity;
    res.timestamp = order.create_timestamp;
    res.last_updated_timestamp = now_ms;
    venue_.report(res);
}

Live::PaperExchange::Outcome Live::PaperExchange::depth_check(
    const Core::Order &order, long long fill_time_ms) {
    const bool is_buy = order.side == Core::OrderSide::BUY;
    const double limit = order.price.toDouble();
    const double remaining = (order.quantity - order.filled_quantity).toDouble();

    const auto book = venue_.book_at(order, fill_time_ms);
    const bool book_usable = book.has_value() && book->is_valid &&
                             fill_time_ms - book->received_ms <= kDepthMaxAgeMs;
    const auto fill = book_usable
                          ? PaperTrading::evaluate_against_depth(
                                order.side, order.type, order.post_only, limit,
                                remaining, *book)
                          : PaperTrading::stale_book(order.type, order.post_only);

    double cumulative = order.filled_quantity.toDouble();
    for (size_t i = 0; i < fill.slices.size(); ++i) {
        cumulative += fill.slices[i].quantity;
        const bool order_done = i + 1 == fill.slices.size() &&
                                fill.outcome == PaperDepthOutcome::FILLED;
        report_fill(order, order_done ? order.quantity.toDouble() : cumulative,
                    fill.slices[i].price,
                    order_done ? Core::ORDER_STATUS_NOT_WORKING
                               : Core::ORDER_STATUS_WORKING,
                    fill.slices[i].quantity, false, fill_time_ms, "USDC");
    }
    if (fill.cancels_remainder) {
        report_fill(order, cumulative, limit, Core::ORDER_STATUS_NOT_WORKING, 0,
                    false, fill_time_ms, "USDC");
    }

    const auto touch = [](const std::optional<MarketData::DepthSnapshot> &snap,
                          bool bids) -> nlohmann::json {
        if (!snap.has_value() || !snap->is_valid) return nullptr;
        const auto &side = bids ? snap->bids : snap->asks;
        if (side.empty()) return nullptr;
        return side.front().price;
    };
    const long long ack_ms = fill_time_ms - venue_.depth_latency_ms(order).value_or(0);
    const auto decision_touch =
        touch(venue_.book_at(order, order.create_timestamp), !is_buy);
    nlohmann::json md = {
        {"order_id", std::to_string(order.id)},
        {"client_order_id", order.client_order_id},
        {"side", is_buy ? "BUY" : "SELL"},
        {"order_type", static_cast<int>(order.type)},
        {"post_only", order.post_only},
        {"quantity", remaining},
        {"limit_price", limit},
        {"decision_ms", order.create_timestamp},
        {"ack_ms", ack_ms},
        {"latency_ms", fill_time_ms - ack_ms},
        {"fill_time_ms", fill_time_ms},
        {"touch_at_decision", decision_touch},
        {"bid_at_fill", touch(book, true)},
        {"ask_at_fill", touch(book, false)},
        {"book_sequence", book.has_value() ? nlohmann::json(book->sequence)
                                           : nlohmann::json(nullptr)},
        {"book_age_ms", book.has_value()
                            ? nlohmann::json(fill_time_ms - book->received_ms)
                            : nlohmann::json(nullptr)},
        {"filled_quantity", fill.filled_quantity},
        {"vwap", fill.slices.empty() ? nlohmann::json(nullptr)
                                     : nlohmann::json(fill.vwap)},
        {"levels_consumed", fill.levels_consumed},
        {"depth_exhausted", fill.depth_exhausted},
        {"outcome", PaperTrading::outcome_name(fill.outcome)},
    };
    if (!fill.slices.empty() && decision_touch.is_number()) {
        const double ref = decision_touch.get<double>();
        md["slippage_bps_vs_decision_touch"] =
            (is_buy ? fill.vwap - ref : ref - fill.vwap) / ref * 10000.0;
    }
    venue_.log(&order,
               std::string("Paper fill evaluated against depth: ") +
                   PaperTrading::outcome_name(fill.outcome),
               md, false);

    const bool closed =
        fill.outcome == PaperDepthOutcome::FILLED || fill.cancels_remainder;
    return closed ? Outcome::CLOSED : Outcome::RESTING;
}

Live::PaperExchange::Outcome Live::PaperExchange::quote_first_check(
    const Core::Order &order, long long now_ms) {
    const auto quote = venue_.quote(order, now_ms);
    if (!quote.best_ask.has_value() && !quote.last.has_value()) {
        return Outcome::RETRY;
    }
    if (order.post_only) {
        if (!PaperTrading::rejects_post_only(order.side, order.price.toDouble(),
                                             quote)) {
            return Outcome::RESTING;
        }
        report_canceled(order, now_ms);
        return Outcome::CLOSED;
    }
    const auto fill = PaperTrading::evaluate(order.side, order.price.toDouble(),
                                             true, quote, venue_.slippage_bps);
    if (!fill.has_value()) {
        if (order.type == Core::OrderType::IMMEDIATE_OR_CANCEL &&
            !order.is_prediction_market_order) {
            report_canceled(order, now_ms);
            return Outcome::CLOSED;
        }
        return Outcome::RESTING;
    }
    report_fill(order, order.quantity.toDouble(), fill->price,
                Core::ORDER_STATUS_NOT_WORKING, order.quantity.toDouble(),
                fill->is_maker, now_ms, "USDT");
    return Outcome::CLOSED;
}

bool Live::PaperExchange::resting_check(const Core::Order &order,
                                        long long now_ms, bool canceled) {
    // A canceled order is judged on the prints before its cancel only.
    auto quote = canceled ? PaperQuote{} : venue_.quote(order, now_ms);
    {
        std::lock_guard lock(ranges_mutex_);
        const auto range = ranges_.find(order.id);
        if (range != ranges_.end() && range->second.seen) {
            quote.trade_low = range->second.low;
            quote.trade_high = range->second.high;
        }
    }
    const auto fill = PaperTrading::evaluate(order.side, order.price.toDouble(),
                                             false, quote, venue_.slippage_bps);
    if (!fill.has_value()) return false;
    const bool depth_venue = venue_.depth_latency_ms(order).has_value();
    const double fee_qty = depth_venue
                               ? (order.quantity - order.filled_quantity).toDouble()
                               : order.quantity.toDouble();
    {
        std::lock_guard lock(mutex_);
        resting_.erase(order.id);
        closed_.insert(order.id);
    }
    report_fill(order, order.quantity.toDouble(), fill->price,
                Core::ORDER_STATUS_NOT_WORKING, fee_qty, fill->is_maker, now_ms,
                "USDT");
    return true;
}

long long Live::PaperExchange::next_wake_ms() const {
    long long wake = next_resting_check_ms_;
    for (const auto &[id, pending] : pending_) {
        wake = std::min(wake, pending.due_ms + kSettleMs);
    }
    return wake;
}

void Live::PaperExchange::run(std::stop_token stop) {
    while (!stop.stop_requested()) {
        try {
            tick(DatetimeUtils::get_current_time_ms());
        } catch (const std::exception &e) {
            venue_.log(nullptr, std::string("Paper exchange pass failed: ") + e.what(),
                       nlohmann::json::object(), true);
        }
        std::unique_lock lock(mutex_);
        if (woken_) {
            woken_ = false;
            continue;
        }
        const long long wait_ms = std::clamp(
            next_wake_ms() - DatetimeUtils::get_current_time_ms(), 1LL,
            kRestingCheckIntervalMs);
        wake_.wait_for(lock, stop, std::chrono::milliseconds(wait_ms),
                       [this] { return woken_; });
        woken_ = false;
    }
}

void Live::PaperExchange::start() {
    if (worker_.joinable()) return;
    worker_ = std::jthread([this](std::stop_token stop) { run(stop); });
}

void Live::PaperExchange::stop() {
    if (!worker_.joinable()) return;
    worker_.request_stop();
    wake_.notify_all();
    worker_.join();
}
