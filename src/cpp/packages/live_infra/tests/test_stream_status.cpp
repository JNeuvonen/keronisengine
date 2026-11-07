#include <catch2/catch_test_macros.hpp>

#include "market_data/stream_status.h"

using MarketData::StreamStatusUtils;

TEST_CASE("url_host drops path, query and credentials", "[stream_status]") {
    CHECK(StreamStatusUtils::url_host(
              "wss://fstream.binance.com/ws/LISTENKEY123") ==
          "wss://fstream.binance.com");
    CHECK(StreamStatusUtils::url_host(
              "wss://socket.polygon.io:443/stocks?apiKey=SECRET") ==
          "wss://socket.polygon.io:443");
    CHECK(StreamStatusUtils::url_host("ws://user:pass@localhost:8040/api") ==
          "ws://localhost:8040");
    CHECK(StreamStatusUtils::url_host("wss://api.hyperliquid.xyz#frag") ==
          "wss://api.hyperliquid.xyz");
    CHECK(StreamStatusUtils::url_host("wss://host.only") ==
          "wss://host.only");
    CHECK(StreamStatusUtils::url_host("") == "");
}
