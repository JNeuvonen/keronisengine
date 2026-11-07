#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "historical_feature_gen/transformations/six_venue_order_sm.h"

class SixVenueBookFixture {
   public:
    static void bba(HistoricalFeatureGen::SixVenueOrderSM& sm, int venue,
                    long long timestamp, double mid) {
        MarketData::MarketEvent event;
        event.type = MarketData::MarketEvent::BBA;
        event.bba = MarketData::BBAEvent{
            symbols[venue], exchanges[venue], markets[venue],
            mid - 0.01, 100.0, mid + 0.01, 100.0, timestamp};
        sm.react(event, true);
    }

    static void l2(HistoricalFeatureGen::SixVenueOrderSM& sm, int venue,
                   long long timestamp,
                   std::vector<std::pair<double, double>> bids,
                   std::vector<std::pair<double, double>> asks,
                   bool snapshot = false) {
        MarketData::L2UpdateEvent update;
        update.symbol = symbols[venue];
        update.exchange_name = exchanges[venue];
        update.market_type = markets[venue];
        update.sub_type = snapshot ? MarketData::L2UpdateEvent::FULL_REFRESH
                                   : MarketData::L2UpdateEvent::INCREMENTAL;
        update.update.timestamp = timestamp;
        update.update.bids = std::move(bids);
        update.update.asks = std::move(asks);
        MarketData::MarketEvent event;
        event.type = MarketData::MarketEvent::L2_UPDATE;
        event.l2_update = std::move(update);
        sm.react(event, true);
    }

    static void seed(HistoricalFeatureGen::SixVenueOrderSM& sm,
                     double outer_bid = 0.0, double outer_ask = 0.0) {
        sm.initialize_symbol_states();
        for (int venue = 0; venue < 6; ++venue) {
            bba(sm, venue, 1000, 100.0);
            MarketData::MarketEvent trade;
            trade.type = MarketData::MarketEvent::TRADE;
            trade.trade = MarketData::TradeEvent{
                symbols[venue], exchanges[venue], markets[venue],
                100.0, 1.0, false, 1000};
            sm.react(trade, true);
            std::vector<std::pair<double, double>> bids{{99.99, 100.0}};
            std::vector<std::pair<double, double>> asks{{100.01, 100.0}};
            if (venue == 0 && outer_bid > 0.0) bids.emplace_back(outer_bid, 2.0);
            if (venue == 0 && outer_ask > 0.0) asks.emplace_back(outer_ask, 2.0);
            l2(sm, venue, 1000, std::move(bids), std::move(asks), true);
        }
    }

   private:
    static constexpr const char* symbols[6] = {
        "XRPUSDT", "XRPUSDT", "XRP-USD", "XRP_USDT", "XRP", "XRP-USDT-SWAP"};
    static constexpr const char* exchanges[6] = {
        "binance", "bybit", "coinbase", "gateio", "hyperliquid", "okx"};
    static constexpr const char* markets[6] = {
        "swap", "swap", "spot", "swap", "swap", "swap"};
};

TEST_CASE("Six-venue depth applies deletions outside the moving window",
          "[six_venue_depth]") {
    const int direction = GENERATE(1, -1);
    HistoricalFeatureGen::SixVenueOrderSM sm;
    SixVenueBookFixture::seed(sm);
    const double mid = direction == 1 ? 99.6 : 100.4;
    SixVenueBookFixture::bba(sm, 0, 2000, mid);
    SixVenueBookFixture::l2(sm, 0, 2000,
                           {{99.99, 0.0}, {mid - 0.01, 10.0}},
                           {{100.01, 0.0}, {mid + 0.01, 10.0}});
    const auto state = sm.emit_state_for_symbol(0);
    REQUIRE(state.has_value());
    const auto index = direction == 1 ? 4 : 5;
    const double price = direction == 1 ? mid - 0.01 : mid + 0.01;
    REQUIRE_THAT((*state)[index], Catch::Matchers::WithinRel(price * 10.0, 1e-5));
}

TEST_CASE("Six-venue depth retains unchanged levels entering the measured window",
          "[six_venue_depth]") {
    const int direction = GENERATE(1, -1);
    const double outer = direction == 1 ? 99.71 : 100.29;
    HistoricalFeatureGen::SixVenueOrderSM sm;
    SixVenueBookFixture::seed(sm, direction == 1 ? outer : 0.0,
                             direction == -1 ? outer : 0.0);
    const double mid = direction == 1 ? 99.8 : 100.2;
    SixVenueBookFixture::bba(sm, 0, 2000, mid);
    SixVenueBookFixture::l2(sm, 0, 2000,
                           {{99.99, 0.0}, {mid - 0.01, 10.0}},
                           {{100.01, 0.0}, {mid + 0.01, 10.0}});
    const auto state = sm.emit_state_for_symbol(0);
    REQUIRE(state.has_value());
    const auto index = direction == 1 ? 4 : 5;
    const double price = direction == 1 ? mid - 0.01 : mid + 0.01;
    REQUIRE_THAT((*state)[index],
                 Catch::Matchers::WithinRel(price * 10.0 + outer * 2.0, 1e-5));
}
