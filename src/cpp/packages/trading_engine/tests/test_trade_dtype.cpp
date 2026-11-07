#include <catch2/catch_test_macros.hpp>

#include "market_making/dtypes/enums.h"
#include "market_making/dtypes/order_fill_info.h"
#include "market_making/dtypes/trade.h"

namespace {

Core::OrderFillInfo entry_fill(unsigned long long order_id, Core::TradeCreationType type,
                               std::unordered_map<std::string, double> kv) {
    Core::OrderFillInfo fill;
    fill.symbol = "BTCUSDT";
    fill.exchange_name = "binance";
    fill.market_type = "swap";
    fill.price = 50000;
    fill.fill_amount = mpfr::mpreal("0.1");
    fill.order_id = order_id;
    fill.order_fill_id = order_id;
    fill.trade_creation_type = type;
    fill.kv_cache = std::move(kv);
    return fill;
}

}  // namespace

TEST_CASE("a discretionary entry fill keeps the trade's kv", "[trade][kv_cache]") {
    auto trade = Core::Trade::from_order_fill(
        entry_fill(1, Core::TradeCreationType::DISCRETIONARY, {{"disc_signal", 7}}),
        mpfr::mpreal("0.1"));
    auto kv = trade.kv_cache;
    kv["disc_stop"] = 48000;
    trade.set_kv_cache(kv);

    trade.on_enter_fill(
        entry_fill(2, Core::TradeCreationType::DISCRETIONARY, {{"disc_signal", 7}, {"disc_cmd", 9}}),
        mpfr::mpreal("0.1"));

    CHECK(trade.kv_cache.at("disc_stop") == 48000);
    CHECK_FALSE(trade.kv_cache.contains("disc_cmd"));
    CHECK(trade.open_fills.back().kv_cache.at("disc_cmd") == 9);
}

TEST_CASE("other entry fills replace the trade's kv", "[trade][kv_cache]") {
    auto trade = Core::Trade::from_order_fill(
        entry_fill(1, Core::TradeCreationType::ORGANIC, {{"VOL_PROXY", 1}}), mpfr::mpreal("0.1"));
    trade.on_enter_fill(entry_fill(2, Core::TradeCreationType::ORGANIC, {{"VOL_PROXY", 2}}),
                        mpfr::mpreal("0.1"));
    CHECK(trade.kv_cache.at("VOL_PROXY") == 2);
}
