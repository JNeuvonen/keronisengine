#include "market_making/dtypes/engine_state_snapshot.h"

nlohmann::json Core::EngineStateSnapshot::to_json() const {
    nlohmann::json json;
    json["timestamp_ms"] = this->timestamp_ms;
    json["nav"] = this->nav.toString();
    json["margin_notional"] = this->margin_notional.toString();
    json["long_exposure"] = this->long_exposure.toString();
    json["short_exposure"] = this->short_exposure.toString();
    json["funding_collected"] = this->funding_collected.toString();
    json["num_active_trades"] = this->active_trades.size();
    json["num_active_orders"] = this->active_orders.size();

    json["active_trades"] = nlohmann::json::array();
    for (const auto &trade : this->active_trades) {
        json["active_trades"].push_back(trade.to_json());
    }

    json["active_orders"] = nlohmann::json::array();
    for (const auto &order : this->active_orders) {
        json["active_orders"].push_back(order.to_json());
    }

    json["quote_assets"] = nlohmann::json::object();
    for (const auto &[symbol, quote_asset] : this->quote_assets) {
        json["quote_assets"][symbol] = quote_asset.to_json();
    }

    json["inventories"] = nlohmann::json::object();
    for (const auto &[symbol, inventory] : this->inventories) {
        json["inventories"][symbol] = inventory.to_json();
    }

    json["order_books"] = nlohmann::json::object();
    for (const auto &[symbol, order_book] : this->order_books) {
        json["order_books"][symbol] = order_book.to_json();
    }

    json["prices"] = nlohmann::json::object();
    for (const auto &[symbol, price] : this->prices) {
        json["prices"][symbol] = price;
    }

    json["top_of_book"] = nlohmann::json::object();
    for (const auto &[symbol, tob] : this->top_of_book) {
        json["top_of_book"][symbol] = tob.to_json();
    }

    json["new_trades"] = nlohmann::json::object();
    for (const auto &[symbol, trades] : this->new_trades) {
        json["new_trades"][symbol] = nlohmann::json::array();
        for (const auto &trade : trades) {
            json["new_trades"][symbol].push_back(trade.to_json());
        }
    }

    json["new_orders"] = nlohmann::json::object();
    for (const auto &[symbol, orders] : this->new_orders) {
        json["new_orders"][symbol] = nlohmann::json::array();
        for (const auto &order : orders) {
            json["new_orders"][symbol].push_back(order.to_json());
        }
    }

    json["new_orders_cancels"] = nlohmann::json::object();
    for (const auto &[symbol, orders] : this->new_orders_cancels) {
        json["new_orders_cancels"][symbol] = nlohmann::json::array();
        for (const auto &order : orders) {
            json["new_orders_cancels"][symbol].push_back(order.to_json());
        }
    }

    json["new_order_fills"] = nlohmann::json::object();
    for (const auto &[symbol, fills] : this->new_order_fills) {
        json["new_order_fills"][symbol] = nlohmann::json::array();
        for (const auto &fill : fills) {
            json["new_order_fills"][symbol].push_back(fill.to_json());
        }
    }

    json["actionable_signals"] = nlohmann::json::array();
    for (const auto &signal : this->actionable_signals) {
        json["actionable_signals"].push_back(signal.to_json());
    }

    return json;
}

Core::EngineStateSnapshot Core::EngineStateSnapshot::from_json(
    const nlohmann::json &json) {
    Core::EngineStateSnapshot snapshot;
    snapshot.timestamp_ms = json["timestamp_ms"].get<long long>();
    snapshot.nav = mpfr::mpreal(json["nav"].get<std::string>());
    snapshot.margin_notional =
        mpfr::mpreal(json["margin_notional"].get<std::string>());
    snapshot.long_exposure =
        mpfr::mpreal(json["long_exposure"].get<std::string>());
    snapshot.short_exposure =
        mpfr::mpreal(json["short_exposure"].get<std::string>());
    // funding_collected added later — tolerate snapshots written
    // before the field existed.
    if (json.contains("funding_collected")) {
        snapshot.funding_collected =
            mpfr::mpreal(json["funding_collected"].get<std::string>());
    }

    for (const auto &trade : json["active_trades"]) {
        snapshot.active_trades.push_back(Core::Trade::from_json(trade));
    }

    for (const auto &order : json["active_orders"]) {
        snapshot.active_orders.push_back(Core::Order::from_json(order));
    }

    snapshot.num_active_trades = json["num_active_trades"].get<long long>();
    snapshot.num_active_orders = json["num_active_orders"].get<long long>();

    for (const auto &[symbol, quote_asset] : json["quote_assets"].items()) {
        snapshot.quote_assets[symbol] =
            Core::QuoteAsset::from_json(quote_asset);
    }

    for (const auto &[symbol, inventory] : json["inventories"].items()) {
        snapshot.inventories[symbol] = Core::Inventory::from_json(inventory);
    }

    for (const auto &[symbol, order_book] : json["order_books"].items()) {
        auto restored = Core::OrderBook::from_json(order_book);
        // Construct the destination slot with a matching ring window_bits
        // so copy-assign (which requires same-sized rings) succeeds for
        // polymarket books restored at 10 bits and exchange books at 19.
        auto [it, _] = snapshot.order_books.try_emplace(
            symbol, restored->ring.window_bits);
        it->second.copy_from(*restored);
    }

    for (const auto &[symbol, price] : json["prices"].items()) {
        snapshot.prices[symbol] = price.get<double>();
    }

    if (json.contains("top_of_book")) {
        for (const auto &[symbol, tob] : json["top_of_book"].items()) {
            snapshot.top_of_book[symbol] = Core::TopOfBook::from_json(tob);
        }
    }

    for (const auto &[symbol, trades] : json["new_trades"].items()) {
        snapshot.new_trades[symbol] = std::vector<Core::Trade>();
        for (const auto &trade : trades) {
            snapshot.new_trades[symbol].push_back(
                Core::Trade::from_json(trade));
        }
    }

    for (const auto &[symbol, orders] : json["new_orders"].items()) {
        snapshot.new_orders[symbol] = std::vector<Core::Order>();
        for (const auto &order : orders) {
            snapshot.new_orders[symbol].push_back(
                Core::Order::from_json(order));
        }
    }

    for (const auto &[symbol, orders] : json["new_orders_cancels"].items()) {
        snapshot.new_orders_cancels[symbol] = std::vector<Core::Order>();
        for (const auto &order : orders) {
            snapshot.new_orders_cancels[symbol].push_back(
                Core::Order::from_json(order));
        }

        for (const auto &[symbol, fills] : json["new_order_fills"].items()) {
            snapshot.new_order_fills[symbol] =
                std::vector<Core::OrderFillInfo>();
            for (const auto &fill : fills) {
                snapshot.new_order_fills[symbol].push_back(
                    Core::OrderFillInfo::from_json(fill));
            }
        }
    }

    if (json.contains("actionable_signals")) {
        for (const auto &signal : json["actionable_signals"]) {
            snapshot.actionable_signals.push_back(
                Core::Signal::from_json(signal));
        }
    }

    return snapshot;
}

nlohmann::json Core::EngineStateSnapshotLight::to_json() const {
    nlohmann::json json;
    json["timestamp_ms"] = this->timestamp_ms;
    json["trades"] = nlohmann::json::array();
    for (const auto &trade : this->trades) {
        json["trades"].push_back(trade.to_json());
    }
    json["orders"] = nlohmann::json::array();
    for (const auto &order : this->orders) {
        json["orders"].push_back(order.to_json());
    }
    return json;
}
