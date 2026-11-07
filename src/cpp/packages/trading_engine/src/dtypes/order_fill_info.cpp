#include "market_making/dtypes/order_fill_info.h"

#include "market_making/dtypes/symbol_precision.h"
#include "market_making/engine.h"
#include "str_math.h"

nlohmann::json Core::OrderFillInfo::to_json() const {
    nlohmann::json json;
    json["symbol"] = this->symbol;
    json["exchange_name"] = this->exchange_name;
    json["market_type"] = this->market_type;
    json["price"] = StrMath::to_fixed_string(
        this->price, SymbolPrecisionAccess::getInstance().get_price_precision(
                         this->get_rounding_symbol(), this->exchange_name,
                         this->market_type));
    json["fill_amount"] = StrMath::to_fixed_string(
        this->fill_amount,
        SymbolPrecisionAccess::getInstance().get_qty_precision(
            this->get_rounding_symbol(), this->exchange_name,
            this->market_type));
    json["side"] = this->side == OrderSide::BUY ? "BUY" : "SELL";
    json["account_id"] = this->account_id;
    json["strategy_id"] = this->strategy_id;
    json["order_id"] = this->order_id;
    json["order_created_timestamp"] = this->order_created_timestamp;
    json["order_fill_timestamp"] = this->order_fill_timestamp;
    json["sizing_coeff_delta"] = this->sizing_coeff_delta.toString();
    json["trade_id"] = this->trade_id.has_value()
                           ? std::to_string(this->trade_id.value())
                           : "";
    if (this->prediction.has_value()) {
        json["prediction"] = this->prediction.value().to_json();
    }
    json["orig_order_full_size"] = StrMath::to_fixed_string(
        this->orig_order_full_size,
        SymbolPrecisionAccess::getInstance().get_qty_precision(
            this->get_rounding_symbol(), this->exchange_name,
            this->market_type));
    json["orig_order_sizing_coeff_delta"] =
        this->orig_order_sizing_coeff_delta.toString();
    json["order_fill_id"] = this->order_fill_id;

    if (this->fee.has_value()) {
        json["fee"] = this->fee.value().to_json();
    }

    json["kv_cache"] = this->kv_cache;

    if (this->hedge_for_trade_id.has_value()) {
        json["hedge_for_trade_id"] = this->hedge_for_trade_id.value();
    }

    if (this->hedge_symbol.has_value()) {
        json["hedge_symbol"] = this->hedge_symbol.value();
    }

    if (!this->clob_token_id.empty()) {
        json["clob_token_id"] = this->clob_token_id;
    }

    json["trade_creation_type"] =
        trade_creation_type_to_str(this->trade_creation_type);

    json["is_finalized"] = this->is_polymarket_fill_confirmed;
    json["fill_delta_applied"] = this->fill_delta_applied.toString();
    if (!this->taker_order_id.empty()) {
        json["taker_order_id"] = this->taker_order_id;
    }

    json["is_maker"] = this->is_maker;
    json["is_order_cancelled_before_fill"] =
        this->is_order_cancelled_before_fill;
    json["order_cancel_timestamp"] = this->order_cancel_timestamp;

    if (this->markout_config.has_value()) {
        nlohmann::json mc;
        mc["duration_ms"] = this->markout_config->duration_ms;
        mc["interval_ms"] = this->markout_config->interval_ms;
        json["markout_config"] = mc;
    }

    return json;
}

Core::OrderFillInfo Core::OrderFillInfo::from_json(const nlohmann::json &json) {
    Core::OrderFillInfo order_fill_info;
    order_fill_info.symbol = json["symbol"];
    order_fill_info.exchange_name = json["exchange_name"];
    order_fill_info.market_type = json["market_type"];
    order_fill_info.price = mpfr::mpreal(json["price"].get<std::string>());
    order_fill_info.account_id = json["account_id"];
    order_fill_info.fill_amount =
        mpfr::mpreal(json["fill_amount"].get<std::string>());
    order_fill_info.side =
        json["side"] == "BUY" ? OrderSide::BUY : OrderSide::SELL;
    order_fill_info.strategy_id = json["strategy_id"];
    order_fill_info.order_id = json["order_id"];
    order_fill_info.order_fill_timestamp =
        json["order_fill_timestamp"].get<long long>();
    order_fill_info.order_created_timestamp =
        json["order_created_timestamp"].get<long long>();
    order_fill_info.sizing_coeff_delta =
        mpfr::mpreal(json["sizing_coeff_delta"].get<std::string>());

    // Handle optional trade_id
    if (!json["trade_id"].is_null()) {
        const std::string trade_id = json["trade_id"].get<std::string>();
        if (trade_id != "") {
            order_fill_info.trade_id = std::stoull(trade_id);
        }
    }

    // Handle optional fee
    if (json.contains("fee") && !json["fee"].is_null()) {
        order_fill_info.fee = OrderResFees::from_json(json["fee"]);
    }

    // Handle optional prediction
    if (json.contains("prediction") && !json["prediction"].is_null()) {
        order_fill_info.prediction = Signal::from_json(json["prediction"]);
    }
    order_fill_info.orig_order_full_size =
        mpfr::mpreal(json["orig_order_full_size"].get<std::string>());
    order_fill_info.orig_order_sizing_coeff_delta =
        mpfr::mpreal(json["orig_order_sizing_coeff_delta"].get<std::string>());
    order_fill_info.order_fill_id =
        json["order_fill_id"].get<unsigned long long>();
    order_fill_info.trade_id =
        json["trade_id"].is_null() ||
                json["trade_id"].get<std::string>().empty()
            ? std::nullopt
            : std::make_optional(
                  std::stoull(json["trade_id"].get<std::string>()));

    if (json.contains("kv_cache") && !json["kv_cache"].is_null()) {
        order_fill_info.kv_cache =
            json["kv_cache"].get<std::unordered_map<std::string, double>>();
    } else {
        order_fill_info.kv_cache = {};
    }
    if (json.contains("hedge_for_trade_id") &&
        !json["hedge_for_trade_id"].is_null()) {
        order_fill_info.hedge_for_trade_id =
            std::make_optional<unsigned long long>(
                json["hedge_for_trade_id"].get<unsigned long long>());
    } else {
        order_fill_info.hedge_for_trade_id = std::nullopt;
    }

    if (json.contains("hedge_symbol") && !json["hedge_symbol"].is_null()) {
        order_fill_info.hedge_symbol = std::make_optional<std::string>(
            json["hedge_symbol"].get<std::string>());
    } else {
        order_fill_info.hedge_symbol = std::nullopt;
    }

    if (json.contains("clob_token_id") && !json["clob_token_id"].is_null()) {
        order_fill_info.clob_token_id =
            json["clob_token_id"].get<std::string>();
    }

    if (json.contains("trade_creation_type") &&
        !json["trade_creation_type"].is_null()) {
        order_fill_info.trade_creation_type = trade_creation_type_from_str(
            json["trade_creation_type"].get<std::string>());
    }

    if (json.contains("is_finalized") && !json["is_finalized"].is_null()) {
        order_fill_info.is_polymarket_fill_confirmed =
            json["is_finalized"].get<bool>();
    }
    if (json.contains("fill_delta_applied") &&
        !json["fill_delta_applied"].is_null()) {
        order_fill_info.fill_delta_applied =
            mpfr::mpreal(json["fill_delta_applied"].get<std::string>());
    }
    if (json.contains("taker_order_id") && !json["taker_order_id"].is_null()) {
        order_fill_info.taker_order_id =
            json["taker_order_id"].get<std::string>();
    }

    order_fill_info.is_maker = json.value("is_maker", true);

    if (json.contains("is_order_cancelled_before_fill") &&
        !json["is_order_cancelled_before_fill"].is_null()) {
        order_fill_info.is_order_cancelled_before_fill =
            json["is_order_cancelled_before_fill"].get<bool>();
    }

    if (json.contains("order_cancel_timestamp") &&
        !json["order_cancel_timestamp"].is_null()) {
        order_fill_info.order_cancel_timestamp =
            json["order_cancel_timestamp"].get<long long>();
    }

    if (json.contains("markout_config") && !json["markout_config"].is_null()) {
        Core::MarkoutConfig mc;
        mc.duration_ms = json["markout_config"]["duration_ms"].get<long long>();
        mc.interval_ms = json["markout_config"]["interval_ms"].get<long long>();
        order_fill_info.markout_config = mc;
    }

    return order_fill_info;
}

nlohmann::json Core::OrderFillInfo::to_db_json() const {
    nlohmann::json j;
    j["symbol"] = this->symbol;
    j["exchange_name"] = this->exchange_name;
    j["market_type"] = this->market_type;
    j["price"] = this->price.toDouble();
    j["fill_amount"] = this->fill_amount.toDouble();
    j["side"] = this->side == OrderSide::BUY ? "BUY" : "SELL";
    j["order_id"] = this->order_id;
    j["order_fill_id"] = this->order_fill_id;
    j["account_id"] = this->account_id;
    j["strategy_id"] = this->strategy_id;
    j["order_created_timestamp"] = this->order_created_timestamp;
    j["order_fill_timestamp"] = this->order_fill_timestamp;
    j["sizing_coeff_delta"] = this->sizing_coeff_delta.toDouble();
    j["fill_delta_applied"] = this->fill_delta_applied.toDouble();
    j["is_fee_paid_in_base_asset"] = this->is_fee_paid_in_base_asset;
    j["is_polymarket_fill_confirmed"] = this->is_polymarket_fill_confirmed;
    j["trade_creation_type"] =
        trade_creation_type_to_str(this->trade_creation_type);
    j["clob_token_id"] = this->clob_token_id;
    j["taker_order_id"] = this->taker_order_id;
    j["is_maker"] = this->is_maker;
    j["is_order_cancelled_before_fill"] = this->is_order_cancelled_before_fill;
    j["order_cancel_timestamp"] = this->order_cancel_timestamp;

    if (this->trade_id.has_value()) {
        j["trade_id"] = this->trade_id.value();
    }
    if (this->fee.has_value()) {
        j["fee_amount"] = this->fee.value().amount.toDouble();
        j["fee_currency"] = this->fee.value().currency;
    }
    if (this->hedge_for_trade_id.has_value()) {
        j["hedge_for_trade_id"] = this->hedge_for_trade_id.value();
    }
    if (this->hedge_symbol.has_value()) {
        j["hedge_symbol"] = this->hedge_symbol.value();
    }
    if (!this->kv_cache.empty()) {
        j["kv_cache"] = nlohmann::json(this->kv_cache).dump();
    }
    if (this->markout_config.has_value()) {
        j["markout_duration_ms"] = this->markout_config->duration_ms;
        j["markout_interval_ms"] = this->markout_config->interval_ms;
    }
    return j;
}

std::string Core::OrderFillInfo::get_rounding_symbol() const {
    if (this->hedge_symbol.has_value()) {
        return this->hedge_symbol.value();
    }
    return this->symbol;
}