#include "market_making/dtypes/market_data.h"

nlohmann::json Core::MarketData::to_json() const {
    nlohmann::json json;
    json["aggr_trades"] = this->aggr_trades;
    json["trades"] = this->trades;
    json["ohlcv_klines"] = this->ohlcv_klines;
    json["order_book_snapshots"] = this->order_book_snapshots;
    json["order_book_updates"] = this->order_book_updates;
    json["predictions"] = this->predictions;
    json["polymarket"] = this->polymarket;
    return json;
}