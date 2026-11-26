#include <catch2/catch_all.hpp>

#include <chrono>
#include <map>
#include <mutex>
#include <thread>

#include "datetime_utils.h"
#include "live_infra/paper_exchange.h"
#include "market_making/dtypes/constants.h"
#include "market_making/dtypes/def_only_structs.h"

using Live::PaperExchange;

namespace {

constexpr int kPaperStrategy = 7;
constexpr int kLiveStrategy = 8;
constexpr long long kLatencyMs = 450;

// Engine order book stand-in: applies reports the way the order-state path
// does (a higher fill books it; a closed status removes the order), unless
// `lag` keeps closed orders listed like an engine that has not caught up.
struct FakeVenue {
    std::mutex mutex;
    std::map<unsigned long long, Core::Order> orders;
    std::vector<Core::OrderResNormalized> reports;
    std::vector<std::string> errors;
    std::map<long long, MarketData::DepthSnapshot> books;  // received_ms -> book
    Live::PaperQuote quote{.best_bid = 99.0, .best_ask = 100.0, .last = 99.5};
    bool lag = false;

    Live::PaperVenue venue() {
        return {
            .has_paper_strategies = [] { return true; },
            .open_orders =
                [this] {
                    std::lock_guard lock(mutex);
                    std::vector<Core::Order> out;
                    for (const auto &[id, o] : orders) out.push_back(o);
                    return out;
                },
            .is_paper_strategy = [](int id) { return id == kPaperStrategy; },
            .depth_latency_ms = [](const Core::Order &o) -> std::optional<long long> {
                if (o.exchange_name != "lighter") return std::nullopt;
                return kLatencyMs;
            },
            .book_at =
                [this](const Core::Order &, long long ts)
                -> std::optional<MarketData::DepthSnapshot> {
                std::lock_guard lock(mutex);
                auto it = books.upper_bound(ts);
                if (it == books.begin()) return std::nullopt;
                return std::prev(it)->second;
            },
            .quote = [this](const Core::Order &, long long) {
                std::lock_guard lock(mutex);
                return quote;
            },
            .fee_rate = [](const Core::Order &, bool) { return 0.0; },
            .report =
                [this](const Core::OrderResNormalized &res) {
                    std::lock_guard lock(mutex);
                    reports.push_back(res);
                    for (auto it = orders.begin(); it != orders.end(); ++it) {
                        if (it->second.client_order_id != res.client_order_id) continue;
                        if (res.filled_quantity > it->second.filled_quantity) {
                            it->second.filled_quantity = res.filled_quantity;
                        }
                        if (res.status == Core::ORDER_STATUS_NOT_WORKING && !lag) {
                            orders.erase(it);
                        }
                        break;
                    }
                },
            .log =
                [this](const Core::Order *, const std::string &message,
                       const nlohmann::json &, bool is_error) {
                    std::lock_guard lock(mutex);
                    if (is_error) errors.push_back(message);
                },
        };
    }

    Core::Order &add(unsigned long long id, const std::string &exchange,
                     Core::OrderType type, Core::OrderSide side, double price,
                     double qty, long long ack_ms, int strategy = kPaperStrategy) {
        std::lock_guard lock(mutex);
        Core::Order o;
        o.id = id;
        o.client_order_id = "c" + std::to_string(id);
        o.strategy_id = strategy;
        o.symbol = "BTCUSDC";
        o.exchange_name = exchange;
        o.market_type = "swap";
        o.type = type;
        o.side = side;
        o.price = price;
        o.quantity = qty;
        o.filled_quantity = 0;
        o.create_timestamp = ack_ms - 20;
        o.ack_create_timestamp = ack_ms;
        o.is_order_creation_acknowledged = true;
        return orders[id] = o;
    }

    void book(long long received_ms, std::vector<MarketData::DepthLevel> asks) {
        std::lock_guard lock(mutex);
        MarketData::DepthSnapshot b;
        b.received_ms = received_ms;
        b.bids = {{99.0, 5.0}};
        b.asks = std::move(asks);
        books[received_ms] = b;
    }

    size_t report_count() {
        std::lock_guard lock(mutex);
        return reports.size();
    }
};

const auto IOC = Core::OrderType::IMMEDIATE_OR_CANCEL;
const auto GTC = Core::OrderType::LIMIT;
const auto BUY = Core::OrderSide::BUY;
const long long kDue = 10'000 + kLatencyMs + PaperExchange::kSettleMs;

}  // namespace

TEST_CASE("a depth-venue IOC fills on the book standing at ack plus latency",
          "[paper_exchange]") {
    FakeVenue fake;
    PaperExchange exchange(fake.venue());
    fake.book(9'990, {{100.0, 5.0}});
    fake.book(10'300, {{100.5, 0.5}, {101.5, 5.0}});
    const auto &order = fake.add(1, "lighter", IOC, BUY, 101.0, 1.0, 10'000);
    exchange.submit(order, 10'000);

    exchange.tick(kDue - 1);
    REQUIRE(fake.reports.empty());

    exchange.tick(kDue);
    REQUIRE(fake.reports.size() == 2);
    REQUIRE(fake.reports[0].price == 100.5);
    REQUIRE(fake.reports[0].filled_quantity == 0.5);
    REQUIRE(fake.reports[0].last_trade_timestamp == 10'000 + kLatencyMs);
    REQUIRE(fake.reports[1].status == Core::ORDER_STATUS_NOT_WORKING);
    REQUIRE(fake.orders.empty());
}

TEST_CASE("a cancel before the due time takes effect; after it the order executes first",
          "[paper_exchange]") {
    FakeVenue fake;
    PaperExchange exchange(fake.venue());
    fake.book(9'990, {{100.0, 5.0}});

    const auto &early = fake.add(1, "lighter", IOC, BUY, 101.0, 1.0, 10'000);
    exchange.submit(early, 10'000);
    REQUIRE(exchange.cancel(1, 10'000 + kLatencyMs - 1));
    fake.orders.erase(1);  // the ticker's cancel success
    exchange.tick(kDue);
    REQUIRE(fake.reports.empty());

    const auto &late = fake.add(2, "lighter", IOC, BUY, 101.0, 1.0, 10'000);
    exchange.submit(late, 10'000);
    REQUIRE_FALSE(exchange.cancel(2, 10'000 + kLatencyMs));
    exchange.tick(kDue);
    REQUIRE(fake.reports.size() == 1);
    REQUIRE(fake.reports[0].filled_quantity == 1.0);
    REQUIRE(fake.orders.empty());
}

TEST_CASE("a deferred cancel cancels what rests after the order executes",
          "[paper_exchange]") {
    FakeVenue fake;
    PaperExchange exchange(fake.venue());
    fake.book(9'990, {{100.0, 0.4}, {102.0, 5.0}});
    const auto &order = fake.add(1, "lighter", GTC, BUY, 101.0, 1.0, 10'000);
    exchange.submit(order, 10'000);
    REQUIRE_FALSE(exchange.cancel(1, 20'000));
    exchange.tick(20'000);
    REQUIRE(fake.reports.size() == 2);
    REQUIRE(fake.reports[0].filled_quantity.toDouble() == Catch::Approx(0.4));
    REQUIRE(fake.reports[0].status == Core::ORDER_STATUS_WORKING);
    REQUIRE(fake.reports[1].status == Core::ORDER_STATUS_NOT_WORKING);
    REQUIRE(fake.orders.empty());
}

TEST_CASE("an order the engine still lists after it closed is not judged again",
          "[paper_exchange]") {
    FakeVenue fake;
    fake.lag = true;
    PaperExchange exchange(fake.venue());
    fake.book(9'990, {{100.0, 5.0}});
    const auto &order = fake.add(1, "lighter", IOC, BUY, 101.0, 1.0, 10'000);
    exchange.submit(order, 10'000);
    exchange.tick(kDue);
    REQUIRE(fake.reports.size() == 1);
    exchange.tick(kDue + 5'000);
    exchange.tick(kDue + 10'000);
    REQUIRE(fake.reports.size() == 1);
}

TEST_CASE("open paper orders restored from disk are adopted from their ack",
          "[paper_exchange]") {
    FakeVenue fake;
    PaperExchange exchange(fake.venue());
    fake.book(9'990, {{100.0, 5.0}});
    fake.add(1, "lighter", IOC, BUY, 101.0, 1.0, 10'000);
    fake.add(2, "lighter", IOC, BUY, 101.0, 1.0, 10'000, kLiveStrategy);
    exchange.tick(kDue);
    REQUIRE(fake.reports.size() == 1);
    REQUIRE(fake.reports[0].client_order_id == "c1");
    REQUIRE(fake.orders.contains(2));
}

TEST_CASE("a quote-venue IOC waits for a price, then fills or cancels at once",
          "[paper_exchange]") {
    FakeVenue fake;
    fake.quote = {};
    PaperExchange exchange(fake.venue());
    const auto &order = fake.add(1, "binance", IOC, BUY, 101.0, 1.0, 10'000);
    exchange.submit(order, 10'000);
    exchange.tick(10'000 + PaperExchange::kSettleMs);
    REQUIRE(fake.reports.empty());

    fake.quote = {.best_bid = 99.0, .best_ask = 100.0, .last = 99.5};
    exchange.tick(11'100);
    REQUIRE(fake.reports.size() == 1);
    REQUIRE(fake.reports[0].price == 100.0);
    REQUIRE_FALSE(fake.reports[0].is_maker);

    const auto &outside = fake.add(2, "binance", IOC, BUY, 99.5, 1.0, 20'000);
    exchange.submit(outside, 20'000);
    exchange.tick(20'000 + PaperExchange::kSettleMs);
    REQUIRE(fake.reports.size() == 2);
    REQUIRE(fake.reports[1].status == Core::ORDER_STATUS_NOT_WORKING);
    REQUIRE(fake.reports[1].filled_quantity == 0);
}

TEST_CASE("a resting order fills as a maker on a trade through its limit",
          "[paper_exchange]") {
    FakeVenue fake;
    PaperExchange exchange(fake.venue());
    const auto &order = fake.add(1, "binance", GTC, BUY, 99.0, 1.0, 10'000);
    exchange.submit(order, 10'000);
    exchange.tick(10'030);
    REQUIRE(fake.reports.empty());

    exchange.on_trade("binance|swap|BTCUSDC", 99.0);
    exchange.tick(11'100);
    REQUIRE(fake.reports.empty());

    exchange.on_trade("binance|swap|BTCUSDC", 98.9);
    exchange.tick(12'200);
    REQUIRE(fake.reports.size() == 1);
    REQUIRE(fake.reports[0].price == 99.0);
    REQUIRE(fake.reports[0].is_maker);
}

TEST_CASE("the worker judges an order at its due time and stops cleanly",
          "[paper_exchange]") {
    FakeVenue fake;
    PaperExchange exchange(fake.venue());
    const long long ack = DatetimeUtils::get_current_time_ms();
    fake.book(ack - 10, {{100.0, 5.0}});
    exchange.start();
    const auto &order = fake.add(1, "lighter", IOC, BUY, 101.0, 1.0, ack);
    exchange.submit(order, ack);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (fake.report_count() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const long long seen = DatetimeUtils::get_current_time_ms();
    REQUIRE(fake.report_count() == 1);
    REQUIRE(seen >= ack + kLatencyMs);
    REQUIRE(seen <= ack + kLatencyMs + 300);
    REQUIRE(exchange.last_tick_ms() > 0);

    exchange.stop();
    fake.add(2, "lighter", IOC, BUY, 101.0, 1.0, DatetimeUtils::get_current_time_ms());
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    REQUIRE(fake.report_count() == 1);
    REQUIRE(fake.errors.empty());
}

TEST_CASE("a resting order canceled after a print through its limit fills instead",
          "[paper_exchange]") {
    FakeVenue fake;
    PaperExchange exchange(fake.venue());
    const auto &order = fake.add(1, "binance", GTC, BUY, 99.0, 1.0, 10'000);
    exchange.submit(order, 10'000);
    exchange.tick(10'030);
    REQUIRE(fake.reports.empty());

    exchange.on_trade("binance|swap|BTCUSDC", 98.9);
    REQUIRE_FALSE(exchange.cancel(1, 10'500));
    REQUIRE_FALSE(exchange.cancel(1, 10'505));
    exchange.tick(10'510);
    REQUIRE(fake.reports.size() == 1);
    REQUIRE(fake.reports[0].price == 99.0);
    REQUIRE(fake.reports[0].filled_quantity == 1.0);
    REQUIRE(fake.reports[0].is_maker);
    REQUIRE(fake.reports[0].status == Core::ORDER_STATUS_NOT_WORKING);
    REQUIRE(fake.orders.empty());
}

TEST_CASE("a canceled resting order ignores prints after the cancel",
          "[paper_exchange]") {
    FakeVenue fake;
    PaperExchange exchange(fake.venue());
    const auto &order = fake.add(1, "binance", GTC, BUY, 99.0, 1.0, 10'000);
    exchange.submit(order, 10'000);
    exchange.tick(10'030);

    exchange.on_trade("binance|swap|BTCUSDC", 99.0);
    REQUIRE_FALSE(exchange.cancel(1, 10'500));
    exchange.on_trade("binance|swap|BTCUSDC", 98.0);
    fake.quote = {.best_bid = 97.5, .best_ask = 98.0, .last = 97.8};
    exchange.tick(10'510);
    REQUIRE(fake.reports.size() == 1);
    REQUIRE(fake.reports[0].filled_quantity == 0);
    REQUIRE(fake.reports[0].status == Core::ORDER_STATUS_NOT_WORKING);
    REQUIRE(fake.orders.empty());
}

TEST_CASE("a resting order is judged only on prints since the exchange took it",
          "[paper_exchange]") {
    FakeVenue fake;
    PaperExchange exchange(fake.venue());
    exchange.on_trade("binance|swap|BTCUSDC", 98.0);
    const auto &first = fake.add(1, "binance", GTC, BUY, 99.0, 1.0, 10'000);
    exchange.submit(first, 10'000);
    exchange.tick(10'030);
    exchange.on_trade("binance|swap|BTCUSDC", 98.5);

    const auto &second = fake.add(2, "binance", GTC, BUY, 99.0, 1.0, 10'200);
    exchange.submit(second, 10'200);
    auto &other = fake.add(3, "binance", GTC, BUY, 99.0, 1.0, 10'200);
    other.symbol = "ETHUSDC";
    exchange.submit(other, 10'200);
    exchange.tick(10'230);
    exchange.on_trade("binance|swap|ETHUSDC", 98.5);

    exchange.tick(11'100);
    REQUIRE(fake.reports.size() == 2);
    REQUIRE(fake.reports[0].client_order_id != "c2");
    REQUIRE(fake.reports[1].client_order_id != "c2");
    REQUIRE(fake.orders.size() == 1);
    REQUIRE(fake.orders.contains(2));
}
