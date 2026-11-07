#include "market_making/dtypes/trade.h"

#include <algorithm>

#include "datetime_utils.h"
#include "market_making/dtypes/order_fill_info.h"
#include "market_making/dtypes/symbol_precision.h"
#include "market_making/engine.h"
#include "str_math.h"
#include "uuid.h"

static std::string get_trade_type_internal_str(
    const Core::TradeTypeInternal &type_internal) {
    if (type_internal == Core::TradeTypeInternal::DIRECTIONAL_TRADE) {
        return "DIRECTIONAL_TRADE";
    } else if (type_internal == Core::TradeTypeInternal::HEDGE) {
        return "HEDGE";
    }
    return "DIRECTIONAL_TRADE";
}

static Core::TradeTypeInternal get_trade_type_internal_from_str(
    const std::string &type_internal_str) {
    if (type_internal_str == "DIRECTIONAL_TRADE") {
        return Core::TradeTypeInternal::DIRECTIONAL_TRADE;
    } else if (type_internal_str == "HEDGE") {
        return Core::TradeTypeInternal::HEDGE;
    }
    return Core::TradeTypeInternal::DIRECTIONAL_TRADE;
}

nlohmann::json Core::Trade::to_json() const {
    nlohmann::json json;
    json["id"] = this->id;
    json["id_str"] = this->id_str;
    json["symbol"] = this->symbol;
    json["exchange_name"] = this->exchange_name;
    json["market_type"] = this->market_type;
    json["open_price"] = StrMath::to_fixed_string(
        this->open_price,
        SymbolPrecisionAccess::getInstance().get_price_precision(
            this->symbol, this->exchange_name, this->market_type));
    json["close_price"] = StrMath::to_fixed_string(
        this->close_price,
        SymbolPrecisionAccess::getInstance().get_price_precision(
            this->symbol, this->exchange_name, this->market_type));
    json["open_quantity"] = StrMath::to_fixed_string(
        this->open_quantity,
        SymbolPrecisionAccess::getInstance().get_qty_precision(
            this->symbol, this->exchange_name, this->market_type));
    json["closed_quantity"] = StrMath::to_fixed_string(
        this->closed_quantity,
        SymbolPrecisionAccess::getInstance().get_qty_precision(
            this->symbol, this->exchange_name, this->market_type));
    json["num_expired_exit_orders"] = this->num_expired_exit_orders;
    json["pnl"] = this->pnl.toString();
    json["pnl_percentage"] = this->pnl_percentage.toString();
    json["account_id"] = this->account_id;
    json["fees"] = this->fees.toString();
    json["num_alfa_signal_override_cancel_orders"] =
        this->num_alfa_signal_override_cancel_orders;
    json["open_time_ms"] = this->open_time_ms;
    json["enter_signal_received_time_ms"] = this->enter_signal_received_time_ms;
    json["close_time_ms"] = this->close_time_ms;
    json["exit_signal_received_time_ms"] = this->exit_signal_received_time_ms;
    json["type"] = this->type == TradeType::LONG ? "LONG" : "SHORT";
    json["sizing_coeff_delta"] = this->sizing_coeff_delta.toString();
    json["strategy_id"] = this->strategy_id;
    json["exit_order_ids"] = this->exit_order_ids;
    json["enter_order_ids"] = this->enter_order_ids;
    json["signal_id"] = this->signal_id;
    json["close_fills"] = nlohmann::json::array();
    if (this->prediction.has_value()) {
        json["prediction"] = this->prediction.value().to_json();
    }
    for (const auto &fill : this->close_fills) {
        json["close_fills"].push_back(fill.to_json());
    }
    for (const auto &fill : this->open_fills) {
        json["open_fills"].push_back(fill.to_json());
    }
    json["kv_cache"] = this->kv_cache;
    json["type_internal"] = get_trade_type_internal_str(this->type_internal);

    if (this->hedge_for_trade_id.has_value()) {
        json["hedge_for_trade_id"] = this->hedge_for_trade_id.value();
    }

    if (this->hedge_for_symbol.has_value()) {
        json["hedge_for_symbol"] = this->hedge_for_symbol.value();
    }

    if (!this->clob_token_id.empty()) {
        json["clob_token_id"] = this->clob_token_id;
    }

    json["trade_creation_type"] =
        trade_creation_type_to_str(this->trade_creation_type);

    return json;
}

mpfr::mpreal Core::Trade::calculate_base_asset_fee_deduction(
    const Core::OrderFillInfo &fill, const mpfr::mpreal &fill_delta,
    const std::vector<Core::OrderFillInfo> &open_fills) {
    if (fill_delta <= 0 || !fill.is_fee_paid_in_base_asset ||
        !fill.fee.has_value()) {
        return 0;
    }
    mpfr::mpreal prev_fee = 0;
    mpfr::mpreal max_fill_amount = 0;
    for (const auto &f : open_fills) {
        if (f.order_id == fill.order_id && f.fill_amount > max_fill_amount) {
            max_fill_amount = f.fill_amount;
            if (f.fee.has_value()) {
                prev_fee = f.fee.value().amount;
            }
        }
    }
    return fill.fee.value().amount - prev_fee;
}

Core::Trade Core::Trade::from_order_fill(const Core::OrderFillInfo &fill,
                                         const mpfr::mpreal &fill_delta) {
    Core::Trade trade;
    trade.symbol =
        fill.hedge_symbol.has_value() ? fill.hedge_symbol.value() : fill.symbol;
    trade.exchange_name = fill.exchange_name;
    trade.market_type = fill.market_type;
    trade.open_price = fill.price;
    mpfr::mpreal delta_applied =
        fill_delta - calculate_base_asset_fee_deduction(fill, fill_delta);
    trade.open_quantity = delta_applied;
    trade.closed_quantity = 0;
    trade.account_id = fill.account_id;
    trade.open_time_ms = fill.order_fill_timestamp;
    trade.enter_signal_received_time_ms = fill.order_created_timestamp;
    trade.close_time_ms = 0;
    trade.id = Common::generate_timestamp_uuid();
    trade.type =
        fill.side == OrderSide::BUY ? TradeType::LONG : TradeType::SHORT;
    trade.sizing_coeff_delta = fill.sizing_coeff_delta;
    trade.strategy_id = fill.strategy_id;
    trade.num_expired_exit_orders = 0;
    trade.num_alfa_signal_override_cancel_orders = 0;
    trade.prediction = fill.prediction;
    if (fill.prediction.has_value()) {
        trade.signal_id = fill.prediction.value().signal_id;
    }
    Core::OrderFillInfo fill_with_delta = fill;
    fill_with_delta.fill_delta_applied = delta_applied;
    trade.open_fills = {fill_with_delta};
    trade.enter_order_ids = {fill.order_id};
    trade.close_fills = {};
    trade.exit_order_ids = {};
    trade.kv_cache = fill.kv_cache;
    trade.clob_token_id = fill.clob_token_id;
    trade.trade_creation_type = fill.trade_creation_type;
    trade.hedge_for_trade_id = fill.hedge_for_trade_id;
    if (fill.hedge_for_trade_id.has_value()) {
        trade.type_internal = Core::TradeTypeInternal::HEDGE;
        trade.hedge_for_symbol = fill.symbol;
    } else {
        trade.type_internal = Core::TradeTypeInternal::DIRECTIONAL_TRADE;
        trade.hedge_for_symbol = std::nullopt;
    }

    return trade;
}

Core::Trade Core::Trade::from_json(const nlohmann::json &json) {
    Core::Trade trade;
    trade.id = json["id"].get<unsigned long long>();
    trade.id_str = std::to_string(trade.id);
    trade.symbol = json["symbol"].get<std::string>();
    trade.exchange_name = json["exchange_name"].get<std::string>();
    trade.market_type = json["market_type"].get<std::string>();
    trade.account_id = json["account_id"].get<long long>();
    trade.open_price = mpfr::mpreal(json["open_price"].get<std::string>());
    trade.close_price = mpfr::mpreal(json["close_price"].get<std::string>());
    trade.open_quantity =
        mpfr::mpreal(json["open_quantity"].get<std::string>());
    trade.closed_quantity =
        mpfr::mpreal(json["closed_quantity"].get<std::string>());
    trade.num_expired_exit_orders = json["num_expired_exit_orders"].get<int>();
    trade.num_alfa_signal_override_cancel_orders =
        json["num_alfa_signal_override_cancel_orders"].get<int>();
    trade.open_time_ms = json["open_time_ms"].get<long long>();
    trade.close_time_ms = json["close_time_ms"].get<long long>();
    trade.enter_signal_received_time_ms =
        json["enter_signal_received_time_ms"].get<long long>();
    trade.exit_signal_received_time_ms =
        json["exit_signal_received_time_ms"].get<long long>();
    trade.type = json["type"] == "LONG" ? TradeType::LONG : TradeType::SHORT;
    trade.sizing_coeff_delta =
        mpfr::mpreal(json["sizing_coeff_delta"].get<std::string>());
    trade.strategy_id = json["strategy_id"].get<int>();
    trade.signal_id = json.contains("signal_id")
                          ? json["signal_id"].get<unsigned long long>()
                          : 0;
    if (json["exit_order_ids"].empty()) {
        trade.exit_order_ids = {};
    } else {
        trade.exit_order_ids =
            json["exit_order_ids"].get<std::vector<unsigned long long>>();
    }

    if (json["enter_order_ids"].empty()) {
        trade.enter_order_ids = {};
    } else {
        trade.enter_order_ids =
            json["enter_order_ids"].get<std::vector<unsigned long long>>();
    }

    // Handle close_fills array
    trade.close_fills.clear();
    for (const auto &fill_json : json["close_fills"]) {
        trade.close_fills.push_back(OrderFillInfo::from_json(fill_json));
    }

    trade.open_fills.clear();
    for (const auto &fill_json : json["open_fills"]) {
        trade.open_fills.push_back(OrderFillInfo::from_json(fill_json));
    }

    // Handle optional prediction
    if (json.contains("prediction") && !json["prediction"].is_null()) {
        trade.prediction = Signal::from_json(json["prediction"]);
    }

    // Initialize pnl fields that aren't in json
    trade.pnl = mpfr::mpreal(json["pnl"].get<std::string>());
    trade.pnl_percentage =
        mpfr::mpreal(json["pnl_percentage"].get<std::string>());
    trade.fees = mpfr::mpreal(json["fees"].get<std::string>());

    if (json.contains("type_internal")) {
        trade.type_internal = get_trade_type_internal_from_str(
            json["type_internal"].get<std::string>());
    } else {
        trade.type_internal = Core::TradeTypeInternal::DIRECTIONAL_TRADE;
    }

    if (json.contains("kv_cache") && !json["kv_cache"].is_null()) {
        trade.kv_cache =
            json["kv_cache"].get<std::unordered_map<std::string, double>>();
    } else {
        trade.kv_cache = {};
    }

    trade.hedge_for_trade_id =
        json.contains("hedge_for_trade_id")
            ? std::make_optional<unsigned long long>(
                  json["hedge_for_trade_id"].get<unsigned long long>())
            : std::nullopt;

    if (json.contains("hedge_for_symbol") &&
        !json["hedge_for_symbol"].is_null()) {
        trade.hedge_for_symbol = std::make_optional<std::string>(
            json["hedge_for_symbol"].get<std::string>());
    } else {
        trade.hedge_for_symbol = std::nullopt;
    }

    if (json.contains("clob_token_id") && !json["clob_token_id"].is_null()) {
        trade.clob_token_id = json["clob_token_id"].get<std::string>();
    }

    if (json.contains("trade_creation_type") &&
        !json["trade_creation_type"].is_null()) {
        trade.trade_creation_type = trade_creation_type_from_str(
            json["trade_creation_type"].get<std::string>());
    }

    return trade;
}

nlohmann::json Core::Trade::to_db_json(bool is_live_trade, bool is_closed_trade,
                                       std::optional<long long> db_id) const {
    nlohmann::json json;
    json["symbol"] = this->symbol;
    json["strategy_id"] = this->strategy_id;
    json["exchange_name"] = this->exchange_name;
    json["market_type"] = this->market_type;
    json["open_price"] = this->open_price.toDouble();
    json["close_price"] = this->close_price.toDouble();
    json["open_quantity"] = this->open_quantity.toDouble();
    json["unique_id"] = this->id;
    json["closed_quantity"] = this->closed_quantity.toDouble();
    json["base_size"] = this->open_quantity.toDouble();
    json["account_id"] = this->account_id;
    json["is_long_trade"] = this->type == TradeType::LONG;
    json["is_short_trade"] = this->type == TradeType::SHORT;
    json["is_live_trade"] = is_live_trade;
    json["is_closed_trade"] = is_closed_trade;
    json["open_time"] = TimeUtils::ms_to_timestamptz(this->open_time_ms);
    json["close_time"] = TimeUtils::ms_to_timestamptz(this->close_time_ms);
    json["pnl"] = this->pnl.toDouble();
    json["pnl_percentage"] = this->pnl_percentage.toDouble();
    json["fees"] = this->fees.toDouble();
    json["strategy_version"] = 2;
    if (db_id.has_value()) {
        json["id"] = db_id.value();
    }
    json["is_hedge"] = this->type_internal == Core::TradeTypeInternal::HEDGE;

    if (this->hedge_for_trade_id.has_value()) {
        json["hedge_for_trade_id"] = this->hedge_for_trade_id.value();
    }
    if (!this->clob_token_id.empty()) {
        json["clob_token_id"] = this->clob_token_id;
    }
    json["trade_creation_type"] =
        trade_creation_type_to_str(this->trade_creation_type);
    json["signal_id"] = this->signal_id;
    if (!this->kv_cache.empty()) {
        json["kv_cache"] = nlohmann::json(this->kv_cache).dump();
    }
    return json;
}

Core::Trade Core::Trade::from_db_json_full(const nlohmann::json &json) {
    Core::Trade trade;
    trade.is_live_trade = true;

    if (json.contains("unique_id") && !json["unique_id"].is_null()) {
        trade.id = std::stoull(json["unique_id"].get<std::string>());
    }
    trade.id_str = std::to_string(trade.id);

    if (json.contains("id") && !json["id"].is_null()) {
        trade.db_id = std::stoll(json["id"].get<std::string>());
    }

    trade.symbol = json.value("symbol", "");
    trade.exchange_name = json.value("exchange_name", "");
    trade.market_type = json.value("market_type", "");
    trade.strategy_id =
        json.contains("strategy_id") && !json["strategy_id"].is_null()
            ? std::stoi(json["strategy_id"].get<std::string>())
            : 0;
    trade.account_id =
        json.contains("account_id") && !json["account_id"].is_null()
            ? std::stoll(json["account_id"].get<std::string>())
            : 0;

    trade.open_price =
        json.contains("open_price") && !json["open_price"].is_null()
            ? mpfr::mpreal(json["open_price"].get<std::string>())
            : 0;
    trade.close_price =
        json.contains("close_price") && !json["close_price"].is_null()
            ? mpfr::mpreal(json["close_price"].get<std::string>())
            : 0;
    trade.open_quantity =
        json.contains("open_quantity") && !json["open_quantity"].is_null()
            ? mpfr::mpreal(json["open_quantity"].get<std::string>())
            : 0;
    trade.closed_quantity =
        json.contains("closed_quantity") && !json["closed_quantity"].is_null()
            ? mpfr::mpreal(json["closed_quantity"].get<std::string>())
            : 0;

    trade.pnl = json.contains("pnl") && !json["pnl"].is_null()
                    ? mpfr::mpreal(json["pnl"].get<std::string>())
                    : 0;
    trade.pnl_percentage =
        json.contains("pnl_percentage") && !json["pnl_percentage"].is_null()
            ? mpfr::mpreal(json["pnl_percentage"].get<std::string>())
            : 0;
    trade.fees = json.contains("fees") && !json["fees"].is_null()
                     ? mpfr::mpreal(json["fees"].get<std::string>())
                     : 0;

    if (json.contains("open_time") && !json["open_time"].is_null()) {
        trade.open_time_ms = DatetimeUtils::timestamptz_string_to_ms(
            json["open_time"].get<std::string>());
    }
    if (json.contains("close_time") && !json["close_time"].is_null()) {
        const auto &close_time_str = json["close_time"].get<std::string>();
        if (!close_time_str.empty()) {
            trade.close_time_ms =
                DatetimeUtils::timestamptz_string_to_ms(close_time_str);
        }
    }

    bool is_long =
        json.contains("is_long_trade") && !json["is_long_trade"].is_null() &&
        json["is_long_trade"].get<std::string>() == "t";
    trade.type = is_long ? TradeType::LONG : TradeType::SHORT;

    if (json.contains("clob_token_id") && !json["clob_token_id"].is_null()) {
        trade.clob_token_id = json["clob_token_id"].get<std::string>();
    }

    trade.sizing_coeff_delta = 0;
    trade.enter_signal_received_time_ms = trade.open_time_ms;

    return trade;
}

Core::Trade Core::Trade::minimal_from_db_json(const nlohmann::json &json) {
    Core::Trade trade;
    trade.id = json["unique_id"].get<unsigned long long>();
    trade.symbol = json["symbol"].get<std::string>();
    trade.strategy_id = json["strategy_id"].get<int>();
    trade.exchange_name = json["exchange_name"].get<std::string>();
    trade.market_type = json["market_type"].get<std::string>();
    trade.db_id = json["id"].get<long long>();
    if (json.contains("clob_token_id") && !json["clob_token_id"].is_null()) {
        trade.clob_token_id = json["clob_token_id"].get<std::string>();
    }
    return trade;
}

mpfr::mpreal Core::Trade::calculate_trade_fee(Engine &engine) const {
    mpfr::mpreal fees = 0;

    const auto unique_fills = this->get_unique_finished_fills();
    const auto &unique_open_fills = unique_fills.first;
    const auto &unique_close_fills = unique_fills.second;

    const auto compute_fees_from_fills = [&](const auto &fills) {
        for (const auto &fill : fills) {
            if (!fill.fee.has_value()) {
                const auto fee_rate = engine.get_fee_rate(
                    this->exchange_name, this->market_type, false);
                fees += (fill.fill_amount * fill.price) * fee_rate;
                continue;
            }

            if (fill.fee.value().currency == "USDT" ||
                fill.fee.value().currency == "USDC" ||
                fill.fee.value().currency == "BNFCR") {
                fees += fill.fee.value().amount;
            } else {
                const auto price =
                    engine.get_price(fill.fee.value().currency + "USDT",
                                     this->exchange_name, this->market_type);
                if (price.has_value()) {
                    fees += fill.fee.value().amount * price.value();
                }
            }
        }
    };

    compute_fees_from_fills(unique_open_fills);
    compute_fees_from_fills(unique_close_fills);
    return fees;
}

std::pair<mpfr::mpreal, mpfr::mpreal> Core::Trade::get_open_and_close_price()
    const {
    const auto &unique_fills = this->get_unique_finished_fills();
    const auto &unique_open_fills = unique_fills.first;
    const auto &unique_close_fills = unique_fills.second;

    mpfr::mpreal avg_open_price = 0;
    mpfr::mpreal total_fill_opens_amount = 0;

    for (const auto &fill : unique_open_fills) {
        avg_open_price += fill.price.toDouble() * fill.fill_amount.toDouble();
        total_fill_opens_amount += fill.fill_amount.toDouble();
    }

    mpfr::mpreal avg_close_price = 0;
    mpfr::mpreal total_fill_closes_amount = 0;

    for (const auto &fill : unique_close_fills) {
        avg_close_price += fill.price.toDouble() * fill.fill_amount.toDouble();
        total_fill_closes_amount += fill.fill_amount.toDouble();
    }

    if (total_fill_opens_amount > 0) {
        avg_open_price /= total_fill_opens_amount;
    }
    if (total_fill_closes_amount > 0) {
        avg_close_price /= total_fill_closes_amount;
    }

    return std::make_pair(avg_open_price, avg_close_price);
}

mpfr::mpreal Core::Trade::get_average_open_price() const {
    const auto &unique_fills = this->get_unique_finished_fills();
    const auto &unique_open_fills = unique_fills.first;

    mpfr::mpreal avg_open_price = 0;
    mpfr::mpreal total_fill_opens_amount = 0;

    for (const auto &fill : unique_open_fills) {
        avg_open_price += fill.price.toDouble() * fill.fill_amount.toDouble();
        total_fill_opens_amount += fill.fill_amount.toDouble();
    }

    if (total_fill_opens_amount > 0) {
        avg_open_price /= total_fill_opens_amount;
    }

    return avg_open_price;
}

mpfr::mpreal Core::Trade::calculate_pnl() const {
    const auto open_and_close_price = this->get_open_and_close_price();
    const auto avg_open_price = open_and_close_price.first;
    const auto avg_close_price = open_and_close_price.second;

    if (this->type == TradeType::LONG) {
        return (avg_close_price - avg_open_price) * this->open_quantity -
               this->fees;
    } else {
        return (avg_open_price - avg_close_price) * this->open_quantity -
               this->fees;
    }
}

void Core::Trade::calculate_pnl_and_fees(Core::Engine &engine,
                                         Core::Trade &trade) {
    trade.fees = trade.calculate_trade_fee(engine);

    const std::pair<mpfr::mpreal, mpfr::mpreal> open_and_close_price =
        trade.get_open_and_close_price();

    trade.open_price = open_and_close_price.first;
    trade.close_price = open_and_close_price.second;

    trade.pnl = trade.calculate_pnl();

    const auto open_notional = trade.open_quantity * trade.open_price;

    if (open_notional != 0) {
        trade.pnl_percentage = (trade.pnl / open_notional) * 100;
    } else {
        trade.pnl_percentage = 0.0;
    }
}

std::pair<std::vector<Core::OrderFillInfo>, std::vector<Core::OrderFillInfo>>
Core::Trade::get_unique_finished_fills() const {
    std::vector<Core::OrderFillInfo> unique_open_fills;
    std::vector<Core::OrderFillInfo> unique_close_fills;

    std::unordered_map<unsigned long long, OrderFillInfo> open_fills_map;
    std::unordered_map<unsigned long long, OrderFillInfo> close_fills_map;

    for (const auto &fill : open_fills) {
        auto it = open_fills_map.find(fill.order_id);
        if (it == open_fills_map.end() ||
            fill.fill_amount > it->second.fill_amount) {
            open_fills_map[fill.order_id] = fill;
        }
    }

    for (const auto &fill : close_fills) {
        auto it = close_fills_map.find(fill.order_id);
        if (it == close_fills_map.end() ||
            fill.fill_amount > it->second.fill_amount) {
            close_fills_map[fill.order_id] = fill;
        }
    }

    for (const auto &[_, fill] : open_fills_map) {
        unique_open_fills.push_back(fill);
    }

    for (const auto &[_, fill] : close_fills_map) {
        unique_close_fills.push_back(fill);
    }

    return std::make_pair(unique_open_fills, unique_close_fills);
}

std::optional<std::reference_wrapper<const Core::OrderFillInfo>>
Core::Trade::get_prev_fill_with_highest_fill_amount(
    unsigned long long order_id) const {
    std::optional<std::reference_wrapper<const OrderFillInfo>> result;
    mpfr::mpreal max_fill_amount = -1;
    for (const auto &f : this->open_fills) {
        if (f.order_id == order_id && f.fill_amount > max_fill_amount) {
            max_fill_amount = f.fill_amount;
            result = std::cref(f);
        }
    }
    return result;
}

void Core::Trade::on_enter_fill(const Core::OrderFillInfo &fill,
                                const mpfr::mpreal &fill_delta) {
    mpfr::mpreal fee_deduction =
        calculate_base_asset_fee_deduction(fill, fill_delta, this->open_fills);
    mpfr::mpreal delta_applied = fill_delta - fee_deduction;

    Core::OrderFillInfo fill_copy = fill;
    fill_copy.fill_delta_applied = delta_applied;
    this->open_fills.push_back(fill_copy);

    this->open_quantity += delta_applied;
    this->sizing_coeff_delta += fill.sizing_coeff_delta;
    // A discretionary trade's kv holds its brackets, set after the opening
    // fill; a later entry order carries only what was known when it was sent.
    if (fill.trade_creation_type != TradeCreationType::DISCRETIONARY) {
        this->kv_cache = fill.kv_cache;
    }
}

void Core::Trade::on_exit_fill(const Core::OrderFillInfo &fill) {
    if (this->exit_signal_received_time_ms == 0) {
        this->exit_signal_received_time_ms = fill.order_fill_timestamp;
    }

    this->close_fills.push_back(fill);
    this->closed_quantity = this->get_unique_order_fills_on_trade();

    mpfr::mpreal trade_remaining_qty =
        this->open_quantity - this->closed_quantity;

    if (this->clob_token_id.empty() && trade_remaining_qty * fill.price < 1) {
        this->closed_quantity = this->open_quantity;
    }
}

void Core::Trade::revert_enter_fill(const Core::OrderFillInfo &fill) {
    this->open_quantity -= fill.fill_delta_applied;
    this->sizing_coeff_delta -= fill.sizing_coeff_delta;
    this->open_fills.erase(
        std::remove(this->open_fills.begin(), this->open_fills.end(), fill),
        this->open_fills.end());
    this->enter_order_ids.erase(
        std::remove(this->enter_order_ids.begin(), this->enter_order_ids.end(),
                    fill.order_id),
        this->enter_order_ids.end());
}

void Core::Trade::revert_exit_fill(const Core::OrderFillInfo &fill) {
    this->close_fills.erase(
        std::remove(this->close_fills.begin(), this->close_fills.end(), fill),
        this->close_fills.end());
    if (this->close_fills.empty()) {
        this->closed_quantity = 0;
    } else {
        this->closed_quantity = this->get_unique_order_fills_on_trade();
    }
}

bool Core::Trade::has_unfinalized_fills() const {
    for (const auto &fill : this->open_fills) {
        if (!fill.is_polymarket_fill_confirmed) return true;
    }
    for (const auto &fill : this->close_fills) {
        if (!fill.is_polymarket_fill_confirmed) return true;
    }
    return false;
}

int Core::Trade::count_open_fills() const {
    return static_cast<int>(this->open_fills.size());
}

mpfr::mpreal Core::Trade::get_unique_order_fills_on_trade() {
    std::vector<OrderFillInfo> unique_fills{};

    for (const auto &close_fill : this->close_fills) {
        const auto order_id = close_fill.order_id;
        OrderFillInfo max_fill = close_fill;

        for (const auto &fill : this->close_fills) {
            if (fill.order_fill_id == close_fill.order_fill_id) {
                continue;
            }

            if (fill.order_id == order_id &&
                fill.fill_amount > max_fill.fill_amount) {
                max_fill = fill;
            }
        }

        bool already_added = false;
        for (const auto &fill : unique_fills) {
            if (fill.order_id == order_id) {
                already_added = true;
                break;
            }
        }

        if (!already_added) {
            unique_fills.push_back(max_fill);
        }
    }

    mpfr::mpreal ret = 0;
    for (const auto &fill : unique_fills) {
        ret += fill.fill_amount;
    }
    return ret;
}

long long Core::Trade::get_time_since_open(Engine &engine) const {
    return engine.get_time() - this->open_time_ms;
}

void Core::Trade::set_kv_cache(
    const std::unordered_map<std::string, double> &kv_cache) {
    this->kv_cache = kv_cache;
}

mpfr::mpreal Core::Trade::calculate_remaining_notional(double price) const {
    const auto remaining_quantity = this->calculate_remaining_quantity();
    return remaining_quantity * price;
}

mpfr::mpreal Core::Trade::calculate_remaining_quantity() const {
    return this->open_quantity - this->closed_quantity;
}

std::optional<long long> Core::Trade::get_time_since_last_exit_fill(
    Engine &engine) const {
    if (this->close_fills.empty()) {
        return std::nullopt;
    }
    long long last_order_fill_timestamp = 0;
    for (const auto &fill : this->close_fills) {
        if (fill.order_fill_timestamp > last_order_fill_timestamp) {
            last_order_fill_timestamp = fill.order_fill_timestamp;
        }
    }
    return engine.get_time() - last_order_fill_timestamp;
}