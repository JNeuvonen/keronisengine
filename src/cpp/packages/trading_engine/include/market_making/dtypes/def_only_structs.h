#pragma once

#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

#include "market_making/dtypes/enums.h"
#include "market_making/dtypes/prediction.h"
#include "mpreal.h"

namespace Core {

struct Order;
struct Engine;
struct Strategy;

struct SubscribeMarketDataStream {
    MarketDataSubscriptionType type;
    MarketDataStreamType stream_type;
    std::optional<std::string> symbol;
    std::optional<std::string> exchange_name;
    std::optional<std::string> market_type;
};

struct ExchangeAndMarket {
    std::string exchange_name;
    std::string market_type;
    std::optional<long long> account_id;
    // Paper fills on venues with a depth model are judged against the book
    // this long after the ack. Not part of operator==: changing it must not
    // read as a ticker-config change.
    std::optional<long long> paper_fill_latency_ms;

    bool operator==(const ExchangeAndMarket& other) const {
        return exchange_name == other.exchange_name &&
               market_type == other.market_type &&
               account_id == other.account_id;
    }

    [[nodiscard]] nlohmann::json to_json() const {
        nlohmann::json json;
        json["exchange_name"] = exchange_name;
        json["market_type"] = market_type;
        json["account_id"] = account_id.value_or(-1);
        if (paper_fill_latency_ms.has_value())
            json["paper_fill_latency_ms"] = *paper_fill_latency_ms;
        return json;
    }
};

struct ExchangeMarketSymbol {
    std::string exchange_name;
    std::string market_type;
    std::string symbol;
};

struct SymbolLimitsAmount {
    mpfr::mpreal min;
    mpfr::mpreal max;
};

struct SymbolLimitsPrice {
    mpfr::mpreal min;
    mpfr::mpreal max;
};

struct SymbolLimitsCost {
    mpfr::mpreal min;
    mpfr::mpreal max;
};

struct SymbolLimits {
    SymbolLimitsAmount amount;
    SymbolLimitsPrice price;
    SymbolLimitsCost cost;
};

struct SymbolPrecision {
    double price;
    double amount;
    double base;
    double quote;
};

struct SymbolPrecisionInt {
    int price;
    int amount;
};

struct SymbolPriceInfo {
    std::string exchange_name;
    std::string market_type;
    std::string symbol;
    double price;
};

struct AssetBorrowableInfo {
    std::string exchange_name;
    std::string market_type;
    std::string asset;
    double available_for_borrow;

    [[nodiscard]] nlohmann::json to_json() const {
        nlohmann::json json;
        json["exchange_name"] = exchange_name;
        json["market_type"] = market_type;
        json["asset"] = asset;
        json["available_for_borrow"] = available_for_borrow;
        return json;
    }
};

struct SymbolExchangeInfo {
    std::string symbol;
    std::string base;
    std::string quote;
    std::string type;
    bool is_active;
    bool allows_margin = false;
    SymbolLimits limits;
    SymbolPrecision precision;
    SymbolPrecisionInt precision_int;

    [[nodiscard]] nlohmann::json to_json() const {
        nlohmann::json json;
        json["symbol"] = symbol;
        json["base"] = base;
        json["quote"] = quote;
        json["type"] = type;
        json["is_active"] = is_active;

        nlohmann::json limits_json;
        limits_json["amount"]["min"] = limits.amount.min.toString();
        limits_json["amount"]["max"] = limits.amount.max.toString();
        limits_json["price"]["min"] = limits.price.min.toString();
        limits_json["price"]["max"] = limits.price.max.toString();
        limits_json["cost"]["min"] = limits.cost.min.toString();
        limits_json["cost"]["max"] = limits.cost.max.toString();
        json["limits"] = limits_json;

        nlohmann::json precision_json;
        precision_json["price"] = precision.price;
        precision_json["amount"] = precision.amount;
        precision_json["base"] = precision.base;
        precision_json["quote"] = precision.quote;
        json["precision"] = precision_json;

        nlohmann::json precision_int_json;
        precision_int_json["price"] = precision_int.price;
        precision_int_json["amount"] = precision_int.amount;
        json["precision_int"] = precision_int_json;

        json["allows_margin"] = allows_margin;

        return json;
    }

    static SymbolExchangeInfo from_json(const nlohmann::json& json) {
        SymbolExchangeInfo info;
        info.symbol = json.at("symbol").get<std::string>();
        info.base = json.at("base").get<std::string>();
        info.quote = json.at("quote").get<std::string>();
        info.is_active = json.at("is_active").get<bool>();
        info.type = json.at("type").get<std::string>();

        // to_json writes limits as strings.
        const auto limit = [&](const char *field, const char *bound) {
            const auto &v = json.at("limits").at(field).at(bound);
            if (v.is_null()) return mpfr::mpreal(0);
            return v.is_string() ? mpfr::mpreal(v.get<std::string>())
                                 : mpfr::mpreal(v.get<double>());
        };
        info.limits.amount.min = limit("amount", "min");
        info.limits.amount.max = limit("amount", "max");
        info.limits.price.min = limit("price", "min");
        info.limits.price.max = limit("price", "max");
        info.limits.cost.min = limit("cost", "min");
        info.limits.cost.max = limit("cost", "max");

        info.precision.price = json.at("precision").at("price").get<double>();
        info.precision.amount = json.at("precision").at("amount").get<double>();
        info.precision.base = json.at("precision").at("base").get<double>();
        info.precision.quote = json.at("precision").at("quote").get<double>();

        info.precision_int.price =
            json.at("precision_int").at("price").get<int>();
        info.precision_int.amount =
            json.at("precision_int").at("amount").get<int>();

        info.allows_margin = json.contains("allows_margin")
                                 ? json.at("allows_margin").get<bool>()
                                 : false;

        return info;
    }
};

struct MarkoutConfig {
    long long duration_ms = 900000;  // 15 min default
    long long interval_ms = 1000;    // 1s default
};

struct MarkoutSnapshot {
    long long timestamp_ms;
    long long elapsed_ms;
    double mid_price;
    double markout_bps;
};

struct CompletedMarkout {
    unsigned long long order_fill_id;
    unsigned long long order_id;
    int strategy_id;
    std::string symbol;
    std::string exchange_name;
    std::string market_type;
    double fill_price;
    OrderSide fill_side;
    long long fill_timestamp_ms;
    long long duration_ms;
    long long interval_ms;
    std::vector<MarkoutSnapshot> snapshots;
    std::unordered_map<std::string, double> fill_time_kv_cache;

    nlohmann::json to_db_json() const {
        nlohmann::json j;
        j["order_fill_id"] = this->order_fill_id;
        j["order_id"] = this->order_id;
        j["strategy_id"] = this->strategy_id;
        j["symbol"] = this->symbol;
        j["exchange_name"] = this->exchange_name;
        j["market_type"] = this->market_type;
        j["fill_price"] = this->fill_price;
        j["fill_side"] = this->fill_side == OrderSide::BUY ? "BUY" : "SELL";
        j["fill_timestamp_ms"] = this->fill_timestamp_ms;
        j["duration_ms"] = this->duration_ms;
        j["interval_ms"] = this->interval_ms;
        j["num_snapshots"] = static_cast<int>(this->snapshots.size());

        auto snapshots_json = nlohmann::json::array();
        for (const auto& s : this->snapshots) {
            snapshots_json.push_back({{"timestamp_ms", s.timestamp_ms},
                                      {"elapsed_ms", s.elapsed_ms},
                                      {"mid_price", s.mid_price},
                                      {"markout_bps", s.markout_bps}});
        }
        j["snapshots"] = snapshots_json.dump();

        if (!this->fill_time_kv_cache.empty()) {
            j["fill_time_kv_cache"] =
                nlohmann::json(this->fill_time_kv_cache).dump();
        }

        return j;
    }
};

struct CreateOrderParams {
    std::string symbol;
    Core::OrderType type;
    Core::OrderSide side;
    mpfr::mpreal price;
    std::optional<Signal> prediction;
    mpfr::mpreal quantity;
    std::string exchange_name;
    std::string market_type;
    int strategy_id;
    long long expires_on_ms;
    mpfr::mpreal sizing_coeff_delta;
    std::optional<unsigned long long> trade_id;
    bool reduce_only;
    long long timestamp;
    const std::string quote_asset;
    const std::string base_asset;
    bool allowed_to_bypass_risk_engine;
    bool must_pass_min_notional_filter;
    bool is_trade_close_order;
    bool is_alt_market_routed = false;
    std::optional<std::string> route_order_to_alt_symbol = std::nullopt;
    std::optional<std::string> routed_order_base_asset = std::nullopt;
    std::optional<std::string> routed_order_quote_asset = std::nullopt;
    bool margin_auto_borrow = false;
    bool margin_auto_repay = false;
    std::string derivatives_settle_currency = "USDT";
    std::optional<double> maximum_notional_quantity = std::nullopt;
    Core::OrderTypeInternal type_internal = Core::OrderTypeInternal::DEFAULT;
    std::unordered_map<std::string, double> kv_cache = {};
    std::optional<unsigned long long> hedge_for_trade_id = std::nullopt;
    std::optional<long long> account_id = std::nullopt;
    std::optional<PositionSide> position_side = std::nullopt;
    bool is_stop_loss_order = false;
    bool is_randomized_size = false;
    bool is_randomized_delay_between_orders = false;
    std::optional<double> randomized_size_lower_bound = std::nullopt;
    std::optional<double> randomized_size_upper_bound = std::nullopt;
    std::optional<long long> randomized_delay_between_orders_ms_lower_bound =
        std::nullopt;
    std::optional<long long> randomized_delay_between_orders_ms_upper_bound =
        std::nullopt;
    bool is_maximum_slippage_enabled = false;
    std::optional<double> signal_start_price = std::nullopt;
    std::optional<double> maximum_slippage_coeff = std::nullopt;
    bool prevent_overlapping_orders = false;
    bool limit_number_of_open_orders = false;
    std::optional<int> max_number_of_open_orders = std::nullopt;
    bool ensure_post_only = false;
    std::optional<double> best_bid_price = std::nullopt;
    std::optional<double> best_ask_price = std::nullopt;
    bool randomize_expiration_time = false;
    std::optional<long long> randomized_expiration_time_ms_lower_bound =
        std::nullopt;
    std::optional<long long> randomized_expiration_time_ms_upper_bound =
        std::nullopt;
    bool prevent_price_exceeding_threshold = false;
    std::optional<double> safe_price_above_current_price_threshold =
        std::nullopt;
    bool automatically_add_orderbook_prices_to_params = false;
    bool limit_strategy_level_enter_order_rate = false;
    bool limit_ticker_level_enter_order_rate = false;
    bool limit_strategy_level_exit_order_rate = false;
    bool limit_ticker_level_exit_order_rate = false;
    std::optional<int> max_strategy_level_enter_orders_per_10s = std::nullopt;
    std::optional<int> max_ticker_level_enter_orders_per_10s = std::nullopt;
    std::optional<int> max_strategy_level_exit_orders_per_10s = std::nullopt;
    std::optional<int> max_ticker_level_exit_orders_per_10s = std::nullopt;
    bool only_post_if_price_within_threshold = false;
    std::optional<double> price_within_threshold_percent = std::nullopt;
    bool ensure_inventory_exists_or_borrowable = false;
    bool apply_cooldown_after_failed_order = true;
    std::optional<long long> failed_order_cooldown_ms_enter = 30000;  // 30 sec
    std::optional<long long> failed_order_cooldown_ms_exit = 2500;    // 2.5 sec
    std::optional<double> max_notional_resting_on_book = std::nullopt;
    std::optional<int> max_unacked_enter_orders = std::nullopt;
    std::optional<int> max_unacked_exit_orders = std::nullopt;
    bool is_prediction_market_order = false;
    std::string clob_id = "";
    std::string outcome_name = "";
    std::optional<MarkoutConfig> markout_config = std::nullopt;

    nlohmann::json to_json() const {
        nlohmann::json json;
        json["symbol"] = symbol;
        json["type"] = type;
        json["side"] = side;
        json["price"] = price.toString();
        json["quantity"] = quantity.toString();
        json["exchange_name"] = exchange_name;
        json["market_type"] = market_type;
        json["strategy_id"] = strategy_id;
        json["is_maximum_slippage_enabled"] = is_maximum_slippage_enabled;
        json["maximum_slippage_coeff"] = maximum_slippage_coeff.has_value()
                                             ? maximum_slippage_coeff.value()
                                             : 0;
        json["expires_on_ms"] = expires_on_ms;
        json["sizing_coeff_delta"] = sizing_coeff_delta.toString();
        json["trade_id"] =
            trade_id.has_value() ? std::to_string(trade_id.value()) : "";
        json["reduce_only"] = reduce_only;
        json["timestamp"] = timestamp;
        json["quote_asset"] = quote_asset;
        json["base_asset"] = base_asset;
        json["allowed_to_bypass_risk_engine"] = allowed_to_bypass_risk_engine;
        json["must_pass_min_notional_filter"] = must_pass_min_notional_filter;
        json["is_trade_close_order"] = is_trade_close_order;
        json["account_id"] =
            account_id.has_value() ? std::to_string(account_id.value()) : "";
        json["is_alt_market_routed"] = is_alt_market_routed;
        json["route_order_to_alt_symbol"] =
            route_order_to_alt_symbol.has_value()
                ? route_order_to_alt_symbol.value()
                : "";
        json["routed_order_base_asset"] = routed_order_base_asset.has_value()
                                              ? routed_order_base_asset.value()
                                              : "";
        json["routed_order_quote_asset"] =
            routed_order_quote_asset.has_value()
                ? routed_order_quote_asset.value()
                : "";
        json["margin_auto_borrow"] = margin_auto_borrow;
        json["margin_auto_repay"] = margin_auto_repay;
        json["derivatives_settle_currency"] = derivatives_settle_currency;
        json["maximum_notional_quantity"] =
            maximum_notional_quantity.has_value()
                ? maximum_notional_quantity.value()
                : 0;
        json["type_internal"] = type_internal;
        json["kv_cache"] = kv_cache;
        json["hedge_for_trade_id"] =
            hedge_for_trade_id.has_value()
                ? std::to_string(hedge_for_trade_id.value())
                : "";
        json["position_side"] =
            position_side.has_value()
                ? position_side.value() == PositionSide::LONG_SIDE ? "LONG"
                                                                   : "SHORT"
                : "";
        json["limit_number_of_open_orders"] = limit_number_of_open_orders;
        json["max_number_of_open_orders"] =
            max_number_of_open_orders.has_value()
                ? max_number_of_open_orders.value()
                : 0;
        json["is_prediction_market_order"] = is_prediction_market_order;
        json["clob_id"] = clob_id;
        json["outcome_name"] = outcome_name;
        return json;
    }
};

struct OrderQuantityInfo {
    mpfr::mpreal quantity;
    mpfr::mpreal sizing_coeff_delta;
};

struct NonTrackedInventoryToBeClosed {
    std::string symbol;
    std::string exchange_name;
    std::string market_type;
    long long account_id;
    // Excess inventory magnitude on `position_side` that needs flattening.
    // Always >= 0; the side itself disambiguates long vs short excess.
    // For non-hedge venues, position_side is nullopt and `quantity` is the
    // signed legacy delta (positive = long excess, negative = short excess);
    // see close_nontracked_inventory for the per-side fan-out.
    mpfr::mpreal quantity;
    std::optional<Core::PositionSide> position_side;

    [[nodiscard]] nlohmann::json to_json() const {
        nlohmann::json json;
        json["symbol"] = symbol;
        json["exchange_name"] = exchange_name;
        json["market_type"] = market_type;
        json["account_id"] = account_id;
        json["quantity"] = quantity.toString();
        json["position_side"] =
            position_side.has_value()
                ? (position_side.value() == Core::PositionSide::LONG_SIDE
                       ? "LONG"
                       : "SHORT")
                : "BOTH";
        return json;
    }
};

// Normalized order-creation response. Used for the generic ccxt path
// (binance, bybit, gateio, etc.) where polymarket/aster-specific fields
// don't apply. Venue-specific result shapes (polymarket status, taking
// amount, etc.) stay in their wrapper layer.
struct OrderCreationResult {
    bool success = false;
    std::string exchange_order_id;
    std::string error_msg;
    // The venue's order (ccxt shape) when the response carried one.
    std::optional<nlohmann::json> order;
};

struct PolymarketBulkOrderResult {
    bool success = false;
    std::string order_id;
    std::string error_msg;
    std::string status;
    std::string taking_amount;
    std::string making_amount;
};

struct PolymarketBulkCancelResult {
    bool success = false;
    std::string order_id;
    std::string error_msg;
};

// One ensemble member: a specific ONNX file paired with its input column list.
// Self-contained — no parallel-array coupling with siblings.
struct ModelMemberSpec {
    std::string onnx_path;                    // absolute, ready to load
    std::vector<std::string> input_columns;
};

// Active model bundle for a strategy. Spec-only: paths + columns + opaque
// metadata. The strategy lazy-loads ONNX sessions keyed by path on first use;
// the engine never touches ONNX. `metadata` is opaque to the engine — the
// strategy reads its own keys (e.g. bear_thresholds, bull_thresholds).
struct ModelBundle {
    std::vector<ModelMemberSpec> members;
    nlohmann::json metadata;
};

}  // namespace Core
