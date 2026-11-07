#include "market_making/dtypes/prediction.h"

nlohmann::json Core::Signal::to_json() const {
    nlohmann::json json;
    json["prediction"] = this->prediction;
    json["price"] = this->price;
    json["event_time_ms"] = this->event_time_ms;
    json["bar_close_time_ms"] = this->bar_close_time_ms;
    json["kv_cache"] = this->kv_cache;
    json["signal_id"] = this->signal_id;
    json["signal_strength"] = this->signal_strength.toDouble();
    json["strategy_id"] = this->strategy_id;
    json["symbol"] = this->symbol;
    return json;
}

Core::Signal Core::Signal::from_json(const nlohmann::json &json) {
    Core::Signal prediction;
    prediction.prediction = json["prediction"].get<double>();
    prediction.price = json["price"].get<double>();
    prediction.event_time_ms = json["event_time_ms"].get<long long>();
    prediction.bar_close_time_ms = json["bar_close_time_ms"].get<long long>();
    prediction.signal_id = json.contains("signal_id")
                               ? json["signal_id"].get<unsigned long long>()
                               : 0;

    if (json.contains("kv_cache")) {
        prediction.kv_cache =
            json["kv_cache"].get<std::unordered_map<std::string, double>>();
    }

    if (json.contains("signal_strength")) {
        prediction.signal_strength =
            mpfr::mpreal(json["signal_strength"].get<double>());
    } else {
        prediction.signal_strength = mpfr::mpreal(0.0);
    }

    if (json.contains("strategy_id")) {
        prediction.strategy_id = json["strategy_id"].get<long long>();
    }

    if (json.contains("symbol")) {
        prediction.symbol = json["symbol"].get<std::string>();
    }

    return prediction;
}
