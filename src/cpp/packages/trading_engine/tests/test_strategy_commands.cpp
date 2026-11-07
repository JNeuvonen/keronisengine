#include <catch2/catch_all.hpp>
#include <filesystem>
#include <fstream>

#include "market_making/strategy_commands.h"

namespace {

Core::StrategyCommand command(long long id, int strategy_id) {
    Core::StrategyCommand c;
    c.id = id;
    c.strategy_id = strategy_id;
    c.op = "cancel_all_orders";
    return c;
}

std::filesystem::path write_file(const std::string& name,
                                 const std::string& body) {
    const auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream(path) << body;
    return path;
}

}  // namespace

TEST_CASE("StrategyCommandInbox drains each strategy's commands in order",
          "[strategy-commands]") {
    Core::StrategyCommandInbox inbox;
    inbox.push(command(1, 7));
    inbox.push(command(2, 8));
    inbox.push(command(3, 7));

    const auto seven = inbox.drain(7);
    REQUIRE(seven.size() == 2);
    CHECK(seven[0].id == 1);
    CHECK(seven[1].id == 3);
    CHECK(inbox.drain(7).empty());
    CHECK(inbox.drain(8).size() == 1);
    CHECK(inbox.drain(9).empty());
}

TEST_CASE("StrategyCommandInbox keeps reports only when enabled",
          "[strategy-commands]") {
    Core::StrategyCommandInbox inbox;
    inbox.report({1, 7, Core::STRATEGY_COMMAND_DONE, ""});
    CHECK(inbox.take_reports().empty());

    inbox.set_reporting_enabled(true);
    inbox.report({1, 7, Core::STRATEGY_COMMAND_WORKING, ""});
    auto reports = inbox.take_reports();
    REQUIRE(reports.size() == 1);
    CHECK(inbox.take_reports().empty());

    // A failed write puts its reports back ahead of newer ones.
    inbox.report({1, 7, Core::STRATEGY_COMMAND_DONE, ""});
    inbox.restore_reports(std::move(reports));
    const auto retried = inbox.take_reports();
    REQUIRE(retried.size() == 2);
    CHECK(retried[0].status == Core::STRATEGY_COMMAND_WORKING);
    CHECK(retried[1].status == Core::STRATEGY_COMMAND_DONE);
}

TEST_CASE("StrategyCommandInbox keeps order links for the persister",
          "[strategy-commands]") {
    Core::StrategyCommandInbox inbox;
    inbox.link_order({1, 10});
    CHECK(inbox.take_order_links().empty());

    inbox.set_reporting_enabled(true);
    inbox.link_order({1, 11});
    auto links = inbox.take_order_links();
    REQUIRE(links.size() == 1);
    inbox.link_order({2, 12});
    inbox.restore_order_links(std::move(links));
    const auto retried = inbox.take_order_links();
    REQUIRE(retried.size() == 2);
    CHECK(retried[0].order_id == 11);
    CHECK(retried[1].order_id == 12);
}

TEST_CASE("StrategyCommandInbox releases replay commands as time passes",
          "[strategy-commands]") {
    const auto path = write_file(
        "strategy_commands_replay.jsonl",
        "{\"ts_ms\": 2000, \"strategy_id\": 7, \"op\": \"close\", "
        "\"symbol\": \"BTCUSDT\"}\n"
        "\n"
        "{\"ts_ms\": 1000, \"strategy_id\": 7, \"op\": \"place_order\"}\n"
        "{\"ts_ms\": 1000, \"strategy_id\": 8, \"op\": \"pause_entries\"}\n");
    Core::StrategyCommandInbox inbox;
    inbox.load_replay_file(path.string());

    inbox.release_due(999);
    CHECK(inbox.drain(7).empty());

    inbox.release_due(1000);
    const auto first = inbox.drain(7);
    REQUIRE(first.size() == 1);
    CHECK(first[0].op == "place_order");
    CHECK(first[0].id == 3);
    CHECK(inbox.drain(8).size() == 1);

    inbox.release_due(5000);
    const auto second = inbox.drain(7);
    REQUIRE(second.size() == 1);
    CHECK(second[0].payload.at("symbol") == "BTCUSDT");
    CHECK_FALSE(second[0].payload.contains("ts_ms"));
    std::filesystem::remove(path);
}

TEST_CASE("StrategyCommandInbox rejects a malformed replay line",
          "[strategy-commands]") {
    const auto path = write_file("strategy_commands_bad.jsonl",
                                 "{\"ts_ms\": 1000, \"op\": \"close\"}\n");
    Core::StrategyCommandInbox inbox;
    CHECK_THROWS_WITH(inbox.load_replay_file(path.string()),
                      Catch::Matchers::ContainsSubstring(":1:"));
    std::filesystem::remove(path);
}

TEST_CASE("Only discretionary strategies take commands",
          "[strategy-commands]") {
    CHECK(Core::accepts_strategy_commands("crypto-discretionary"));
    CHECK_FALSE(Core::accepts_strategy_commands("example-strategy"));
    CHECK_FALSE(Core::accepts_strategy_commands("crypto-discretionary-2"));
    CHECK(Core::accepts_strategy_commands("crypto-discretionary:1000000"));
    CHECK_FALSE(Core::accepts_strategy_commands("crypto-discretionary:"));
    CHECK_FALSE(Core::accepts_strategy_commands("crypto-discretionary:0"));
    CHECK_FALSE(Core::accepts_strategy_commands("crypto-discretionary:01"));
    CHECK_FALSE(Core::accepts_strategy_commands("crypto-discretionary:alice"));
}

TEST_CASE("shared discretionary executor names use the command implementation", "[strategy-commands]") {
    CHECK(Core::accepts_strategy_commands("crypto-discretionary-paper"));
    CHECK(Core::accepts_strategy_commands("crypto-discretionary-realmoney"));
    CHECK_FALSE(Core::accepts_strategy_commands("crypto-discretionary-realmoney:1"));
}

#include <catch2/catch_all.hpp>
#include "test_helpers.h"
#include "market_making/discretionary_accounts.h"
#include "market_making/strategy_commands.h"
#include "strategies/discretionary/discretionary.h"

namespace {
struct LiveClock {
    Core::Engine& engine = Core::Engine::getInstance();
    bool previous = engine.is_backtest;
    LiveClock() { engine.is_backtest = false; }
    ~LiveClock() { engine.is_backtest = previous; }
};
}

TEST_CASE("account pauses and resumed pauses do not pause another account", "[discretionary][accounts]") {
    TestHelpers::ensure_minimal_engine();
    LiveClock clock;
    auto& inbox = Core::StrategyCommandInbox::instance();
    inbox.set_reporting_enabled(true);
    Core::DiscretionaryAccounts::instance().configure({
        {.id=101, .paper=true, .enabled=true, .capital=1000},
        {.id=102, .paper=true, .enabled=true, .capital=5000}});
    for (const bool resumed : {false,true}) {
        const int strategy_id = resumed ? 10102 : 10101;
        auto row = TestHelpers::make_disabled_strategy_config(strategy_id,"crypto-discretionary-paper");
        row.is_paper_trading = true;
        row.discretionary_multi_account = true;
        Core::Strategy strategy(row);
        inbox.push({.id=1001, .strategy_id=strategy_id, .op="pause_entries", .payload={{"account_id",101}}, .resumed=resumed});
        inbox.push({.id=1002, .strategy_id=strategy_id, .op="pause_entries", .payload={{"account_id",102}}, .resumed=resumed});
        inbox.push({.id=1003, .strategy_id=strategy_id, .op="resume_entries", .payload={{"account_id",101}}});
        AlgorithmDiscretionary::on_market_event_callback(clock.engine,strategy,std::nullopt);
        const auto books = inbox.take_changed_books();
        const auto found = std::find_if(books.begin(),books.end(),[&](const auto& book){return book.first==strategy_id;});
        REQUIRE(found!=books.end());
        CHECK(found->second.at("paused_accounts")==nlohmann::json::array({102}));
    }
}

TEST_CASE("executor refuses missing account and incompatible execution mode", "[discretionary][accounts]") {
    TestHelpers::ensure_minimal_engine();
    LiveClock clock;
    auto& inbox = Core::StrategyCommandInbox::instance();
    inbox.set_reporting_enabled(true);
    inbox.take_reports();
    Core::DiscretionaryAccounts::instance().configure({{.id=103, .paper=false, .enabled=true}});
    auto row = TestHelpers::make_disabled_strategy_config(10103,"crypto-discretionary-paper");
    row.is_paper_trading=true;
    row.discretionary_multi_account=true;
    Core::Strategy strategy(row);
    inbox.push({.id=2001,.strategy_id=row.id,.op="pause_entries",.payload=nlohmann::json::object()});
    inbox.push({.id=2002,.strategy_id=row.id,.op="pause_entries",.payload={{"account_id",103}}});
    AlgorithmDiscretionary::on_market_event_callback(clock.engine,strategy,std::nullopt);
    const auto reports=inbox.take_reports();
    CHECK(std::count_if(reports.begin(),reports.end(),[](const auto& report){return report.status==Core::STRATEGY_COMMAND_FAILED;})==2);
}

TEST_CASE("discretionary risk caps use selected account NAV and reservations", "[discretionary][accounts]") {
    TestHelpers::ensure_minimal_engine();
    auto& engine=Core::Engine::getInstance();
    auto row=TestHelpers::make_disabled_strategy_config(10104,"crypto-discretionary-paper");
    row.is_paper_trading=true;
    row.discretionary_multi_account=true;
    row.max_directional_exposure=6;
    Core::Strategy strategy(row);
    Core::DiscretionaryAccounts::instance().configure({
        {.id=104,.paper=true,.enabled=true,.exchange="binance",.market="swap",.capital=1000,.max_exposure=1},
        {.id=105,.paper=true,.enabled=true,.exchange="binance",.market="swap",.capital=5000,.max_exposure=1}});
    Core::RiskEngine risk;
    risk.update_navs_by_account_id({{104,1000},{105,5000}});
    Core::Order first;
    first.id=9001; first.account_id=104; first.symbol="BTCUSDT";
    first.exchange_name="binance"; first.market_type="swap"; first.side=Core::OrderSide::BUY;
    first.quantity=20; first.price=100; first.sizing_coeff_delta=2;
    risk.apply_risk_bounds_resizing(engine,strategy,first);
    REQUIRE(first.quantity==10);
    risk.register_order(first);
    auto another=first; another.id=9002; another.quantity=1;
    risk.apply_risk_bounds_resizing(engine,strategy,another);
    CHECK(another.quantity==0);
    auto other=first; other.id=9003; other.account_id=105; other.quantity=20;
    risk.apply_risk_bounds_resizing(engine,strategy,other);
    CHECK(other.quantity==20);
    Core::DiscretionaryAccounts::instance().configure({{.id=105,.paper=false,.enabled=true,.exchange="binance",.market="swap"}});
    risk.apply_risk_bounds_resizing(engine,strategy,other);
    CHECK(other.quantity==0);
}

TEST_CASE("account cancellation leaves other accounts orders intact", "[discretionary][accounts]") {
    TestHelpers::ensure_minimal_engine();
    LiveClock clock;
    auto row=TestHelpers::make_disabled_strategy_config(10105,"crypto-discretionary-paper");
    row.is_paper_trading=true;
    row.discretionary_multi_account=true;
    Core::Strategy strategy(row);
    auto ticker=std::make_shared<TestHelpers::MockTradeTicker>(
        std::vector<Core::ExchangeAndMarket>{{"binance","swap",201},{"binance","swap",202}},
        "BTCUSDT","USDT","BTC",row.id);
    strategy.add_trade_ticker("BTCUSDT",ticker);
    Core::DiscretionaryAccounts::instance().configure({
        {.id=201,.paper=true,.enabled=true,.exchange="binance",.market="swap"},
        {.id=202,.paper=true,.enabled=true,.exchange="binance",.market="swap"}});
    for (long long account : {201,202}) {
        Core::Order order{};
        order.id=account; order.id_str=std::to_string(account); order.account_id=account;
        order.strategy_id=row.id; order.symbol="BTCUSDT"; order.exchange_name="binance"; order.market_type="swap";
        order.side=Core::OrderSide::BUY; order.quantity=1; order.price=100;
        order.expires_on_ms=clock.engine.get_time()+60000;
        order.kv_cache={{"disc_signal",static_cast<double>(account)},{"disc_cmd",static_cast<double>(account)},{"disc_role",4}};
        ticker->get_active_orders_mut_ref().push_back(order);
    }
    auto& inbox=Core::StrategyCommandInbox::instance();
    inbox.set_reporting_enabled(true);
    inbox.push({.id=3001,.strategy_id=row.id,.op="cancel_all_orders",.payload={{"account_id",201}}});
    const auto decisions=AlgorithmDiscretionary::on_market_event_callback(clock.engine,strategy,std::nullopt);
    REQUIRE(decisions.size()==1);
    REQUIRE(decisions[0].cancelled_orders.size()==1);
    CHECK(decisions[0].cancelled_orders[0].account_id==201);
    CHECK(ticker->get_active_orders_ref().size()==2);
}
