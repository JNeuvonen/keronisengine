#include "market_data/aggregate_trade.h"

#include "market_data/normalized_trade.h"

namespace MarketData {

AggregateTrade AggregateTrade::from_json(const nlohmann::json& json,
                                         const std::string& symbol,
                                         const std::string& exchange_name,
                                         const std::string& market_type) {
    AggregateTrade trade;
    trade.symbol = symbol;
    trade.exchange_name = exchange_name;
    trade.market_type = market_type;
    trade.price = std::stod(json["p"].get<std::string>());
    trade.quantity = std::stod(json["q"].get<std::string>());
    trade.first_id = json["f"].get<long long>();
    trade.last_id = json["l"].get<long long>();
    trade.timestamp = json["T"].get<long long>();
    trade.is_buyer_maker = json["m"].get<bool>();
    return trade;
}

std::vector<NormalizedParsedTrade> AggregateTrade::to_normalized_trades() {
    std::vector<NormalizedParsedTrade> ret;

    int num_trades = this->last_id - this->first_id + 1;

    if (num_trades == 1) {
        NormalizedParsedTrade trade;
        trade.symbol = this->symbol;
        trade.exchange_name = this->exchange_name;
        trade.market_type = this->market_type;
        trade.price = this->price;
        trade.quantity = this->quantity;
        trade.timestamp = this->timestamp;
        trade.parsed_timestamp = this->timestamp;
        trade.is_buyer_maker = this->is_buyer_maker;
        trade.trade_id = std::to_string(this->first_id);
        ret.push_back(trade);
        return ret;
    }

    const double quantity_splitted = this->quantity / num_trades;

    for (int i = 0; i < num_trades; i++) {
        NormalizedParsedTrade trade;
        trade.symbol = this->symbol;
        trade.exchange_name = this->exchange_name;
        trade.market_type = this->market_type;
        trade.price = this->price;
        trade.quantity = quantity_splitted;
        trade.timestamp = this->timestamp;
        trade.parsed_timestamp = this->timestamp;
        trade.is_buyer_maker = this->is_buyer_maker;
        trade.trade_id = std::to_string(this->first_id + i);
        ret.push_back(trade);
    }
    return ret;
}

}  // namespace MarketData