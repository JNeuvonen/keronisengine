#include "market_making/dtypes/quote_asset.h"

nlohmann::json Core::QuoteAsset::to_json() const {
    nlohmann::json json;
    json["asset"] = this->asset;
    json["quantity"] = this->quantity.toString();
    json["price"] = this->price.toString();
    json["account_id"] = this->account_id;
    json["exchange_name"] = this->exchange_name;
    json["market_type"] = this->market_type;
    return json;
}

Core::QuoteAsset Core::QuoteAsset::from_json(const nlohmann::json &json) {
    Core::QuoteAsset quote_asset;
    quote_asset.asset = json["asset"].get<std::string>();
    quote_asset.quantity = mpfr::mpreal(json["quantity"].get<std::string>());
    quote_asset.price = mpfr::mpreal(json["price"].get<std::string>());

    if (json.contains("account_id")) {
        quote_asset.account_id = json["account_id"].get<long long>();
    }

    if (json.contains("exchange_name")) {
        quote_asset.exchange_name = json["exchange_name"].get<std::string>();
    }

    if (json.contains("market_type")) {
        quote_asset.market_type = json["market_type"].get<std::string>();
    }
    return quote_asset;
}