#include "market_making/utils.h"

#include <optional>

#include "common_types/constants.h"
#include "market_data/ohlcv_dataset.h"
#include "market_making/dtypes/def_only_structs.h"
#include "market_making/dtypes/enums.h"
#include "market_making/dtypes/inventory.h"
#include "market_making/dtypes/order_ladder_params.h"
#include "market_making/dtypes/strategy_decision.h"
#include "market_making/engine.h"
#include "market_making/ticker.h"

mpfr::mpreal Core::Utils::get_order_fill_size_delta(
    const Core::Order& order, const Core::OrderFillInfo& fill_info) {
    mpfr::mpreal delta = fill_info.fill_amount - order.filled_quantity;
    return delta;
}

std::string Core::Utils::get_exchange_market_key(
    const std::string& exchange_name, const std::string& market_type) {
    return exchange_name + ":" + market_type;
}

std::string Core::Utils::get_exchange_market_account_key(
    const std::string& exchange_name, const std::string& market_type,
    long long account_id) {
    return exchange_name + ":" + market_type + ":" + std::to_string(account_id);
}

std::string Core::Utils::get_order_key(const Core::Order& order) {
    return get_exchange_market_account_key(
               order.exchange_name, order.market_type, order.account_id) +
           ":" + order.symbol + ":" + order.client_order_id;
}

bool Core::Utils::is_same_order(const Core::Order& a, const Core::Order& b) {
    return a.client_order_id == b.client_order_id &&
           a.account_id == b.account_id && a.symbol == b.symbol &&
           a.exchange_name == b.exchange_name && a.market_type == b.market_type;
}

bool Core::Utils::is_same_order(const Core::Order& order,
                                const std::string& client_order_id,
                                long long account_id, const std::string& symbol,
                                const std::string& exchange_name,
                                const std::string& market_type) {
    return order.client_order_id == client_order_id &&
           order.account_id == account_id && order.symbol == symbol &&
           order.exchange_name == exchange_name &&
           order.market_type == market_type;
}

bool Core::Utils::is_same_order_by_client_and_account(const Core::Order& a,
                                                      const Core::Order& b) {
    return a.client_order_id == b.client_order_id &&
           a.account_id == b.account_id;
}

bool Core::Utils::is_same_order_by_client_and_account(
    const Core::Order& order, const std::string& client_order_id,
    long long account_id) {
    return order.client_order_id == client_order_id &&
           order.account_id == account_id;
}

bool Core::Utils::is_same_order_by_client_id(const Core::Order& a,
                                             const Core::Order& b) {
    return a.client_order_id == b.client_order_id;
}

bool Core::Utils::is_same_order_by_client_id(
    const Core::Order& order, const std::string& client_order_id) {
    return order.client_order_id == client_order_id;
}

bool Core::Utils::is_same_order_adaptive(const Core::Order& order,
                                         const std::string& client_order_id,
                                         const std::string& exchange_order_id) {
    if (order.uses_exchange_order_id_for_lookup()) {
        return !exchange_order_id.empty() &&
               order.exchange_order_id == exchange_order_id;
    }
    return order.client_order_id == client_order_id;
}

bool Core::Utils::is_same_order_adaptive(const Core::Order& order,
                                         const std::string& client_order_id,
                                         const std::string& exchange_order_id,
                                         long long account_id) {
    if (order.account_id != account_id) return false;
    return is_same_order_adaptive(order, client_order_id, exchange_order_id);
}

std::string Core::Utils::get_symbol_exchange_market_key(
    const std::string& symbol, const std::string& exchange_name,
    const std::string& market_type) {
    return symbol + ":" + exchange_name + ":" + market_type;
}

std::string Core::Utils::get_exchange_dataset_key(
    const std::string& symbol, const std::string& exchange_name,
    const std::string& market_type, long long interval_ms) {
    return symbol + ":" + exchange_name + ":" + market_type + ":" +
           Core::Utils::get_interval_str_from_ohlcv_tick(interval_ms);
}

std::string Core::Utils::get_strategy_symbol_exchange_market_key(
    const std::string& symbol, const std::string& exchange_name,
    const std::string& market_type, const int strategy_id) {
    return symbol + ":" + exchange_name + ":" + market_type + ":" +
           std::to_string(strategy_id);
}

std::string Core::Utils::get_symbol_exchange_market_account_key(
    const std::string& symbol, const std::string& exchange_name,
    const std::string& market_type, long long account_id) {
    return Utils::get_symbol_exchange_market_key(symbol, exchange_name,
                                                 market_type) +
           ":" + std::to_string(account_id);
}

std::string Core::Utils::get_interval_str_from_ohlcv_tick(
    long long interval_ms) {
    switch (interval_ms) {
        case 60 * 1000:  // 1 minute
            return "1m";
        case 3 * 60 * 1000:  // 3 minutes
            return "3m";
        case 5 * 60 * 1000:  // 5 minutes
            return "5m";
        case 15 * 60 * 1000:  // 15 minutes
            return "15m";
        case 30 * 60 * 1000:  // 30 minutes
            return "30m";
        case 60 * 60 * 1000:  // 1 hour
            return "1h";
        case 2 * 60 * 60 * 1000:  // 2 hours
            return "2h";
        case 4 * 60 * 60 * 1000:  // 4 hours
            return "4h";
        case 6 * 60 * 60 * 1000:  // 6 hours
            return "6h";
        case 8 * 60 * 60 * 1000:  // 8 hours
            return "8h";
        case 12 * 60 * 60 * 1000:  // 12 hours
            return "12h";
        case 24 * 60 * 60 * 1000:  // 1 day
            return "1d";
        default:
            throw std::invalid_argument("Invalid interval_ms provided");
    }
}

mpfr::mpreal Core::Utils::get_acc_symbol_trade_open_quantity(
    const std::vector<Core::Trade>& trades, const std::string& symbol,
    const std::string& exchange_name, const std::string& market_type,
    long long account_id) {
    mpfr::mpreal open_quantity = 0;
    for (const auto& trade : trades) {
        if (trade.symbol == symbol && trade.exchange_name == exchange_name &&
            trade.market_type == market_type &&
            trade.account_id == account_id) {
            open_quantity += (trade.open_quantity - trade.closed_quantity) *
                             (trade.type == Core::TradeType::LONG ? 1 : -1);
        }
    }
    return open_quantity;
}

mpfr::mpreal Core::Utils::get_acc_symbol_trade_open_quantity_by_side(
    const std::vector<Core::Trade>& trades, const std::string& symbol,
    const std::string& exchange_name, const std::string& market_type,
    long long account_id, Core::PositionSide side) {
    const auto target_type = side == Core::PositionSide::LONG_SIDE
                                 ? Core::TradeType::LONG
                                 : Core::TradeType::SHORT;
    mpfr::mpreal open_quantity = 0;
    for (const auto& trade : trades) {
        if (trade.symbol == symbol && trade.exchange_name == exchange_name &&
            trade.market_type == market_type &&
            trade.account_id == account_id && trade.type == target_type) {
            open_quantity += trade.open_quantity - trade.closed_quantity;
        }
    }
    return open_quantity;
}

int Core::Utils::get_acc_number_of_active_trades(
    const std::vector<Core::Trade>& trades, const std::string& symbol,
    const std::string& exchange_name, const std::string& market_type,
    long long account_id) {
    int count = 0;
    for (const auto& trade : trades) {
        if (trade.symbol == symbol && trade.exchange_name == exchange_name &&
            trade.market_type == market_type &&
            trade.account_id == account_id) {
            count++;
        }
    }
    return count;
}

std::vector<std::pair<mpfr::mpreal, std::vector<Core::Trade>>>
Core::Utils::get_symbol_all_trade_open_quantity_combinations(
    const std::vector<Core::Trade>& trades, const std::string& symbol,
    const std::string& exchange_name, const std::string& market_type,
    long long account_id) {
    std::vector<std::pair<mpfr::mpreal, std::vector<Core::Trade>>>
        all_trade_open_quantity_combinations;
    for (const auto& trade_outer : trades) {
        if (trade_outer.symbol == symbol &&
            trade_outer.exchange_name == exchange_name &&
            trade_outer.market_type == market_type &&
            trade_outer.account_id == account_id) {
            mpfr::mpreal open_quantity =
                (trade_outer.open_quantity - trade_outer.closed_quantity) *
                (trade_outer.type == Core::TradeType::LONG ? 1 : -1);
            std::vector<Core::Trade> trade_combination{trade_outer};

            all_trade_open_quantity_combinations.push_back(
                {open_quantity, trade_combination});

            for (const auto& trade_inner : trades) {
                if (trade_inner.id == trade_outer.id) {
                    continue;
                }
                mpfr::mpreal trade_open_quantity =
                    (trade_inner.open_quantity - trade_inner.closed_quantity) *
                    (trade_outer.type == Core::TradeType::LONG ? 1 : -1);

                if (trade_open_quantity > MPREAL_EPSILON_E_10) {
                    trade_combination.push_back(trade_inner);
                    open_quantity += trade_open_quantity;
                    all_trade_open_quantity_combinations.push_back(
                        {open_quantity, trade_combination});
                }
            }
        }
    }

    // Sort each trade combination by ID
    for (auto& combination : all_trade_open_quantity_combinations) {
        std::sort(combination.second.begin(), combination.second.end(),
                  [](const Core::Trade& a, const Core::Trade& b) {
                      return a.id < b.id;
                  });
    }

    // Sort the combinations vector by size and then by trade IDs
    std::sort(all_trade_open_quantity_combinations.begin(),
              all_trade_open_quantity_combinations.end(),
              [](const auto& a, const auto& b) {
                  if (a.second.size() != b.second.size()) {
                      return a.second.size() < b.second.size();
                  }
                  for (size_t i = 0; i < a.second.size(); ++i) {
                      if (a.second[i].id != b.second[i].id) {
                          return a.second[i].id < b.second[i].id;
                      }
                  }
                  return false;
              });

    // Remove duplicates
    auto it = std::unique(all_trade_open_quantity_combinations.begin(),
                          all_trade_open_quantity_combinations.end(),
                          [](const auto& a, const auto& b) {
                              if (a.second.size() != b.second.size()) {
                                  return false;
                              }
                              for (size_t i = 0; i < a.second.size(); ++i) {
                                  if (a.second[i].id != b.second[i].id) {
                                      return false;
                                  }
                              }
                              return true;
                          });
    all_trade_open_quantity_combinations.erase(
        it, all_trade_open_quantity_combinations.end());

    return all_trade_open_quantity_combinations;
}

mpfr::mpreal Core::Utils::get_account_symbol_inventory(
    const std::string& symbol, const std::string& exchange_name,
    const std::string& market_type, long long account_id,
    const std::unordered_map<std::string, Core::Inventory>& inventory) {
    const auto inventory_it =
        inventory.find(get_symbol_exchange_market_account_key(
            symbol, exchange_name, market_type, account_id));

    if (inventory_it == inventory.end()) {
        return 0;
    }

    return inventory_it->second.total;
}

mpfr::mpreal Core::Utils::get_account_symbol_inventory_by_side(
    const std::string& symbol, const std::string& exchange_name,
    const std::string& market_type, long long account_id,
    const std::unordered_map<std::string, Core::Inventory>& inventory,
    Core::PositionSide side) {
    const auto key = get_symbol_exchange_market_account_key(
        symbol, exchange_name, market_type, account_id);
    const auto it = inventory.find(key);
    if (it == inventory.end()) {
        return mpfr::mpreal(0);
    }
    return side == Core::PositionSide::LONG_SIDE ? it->second.long_total
                                                 : it->second.short_total;
}

bool Core::Utils::is_exchange_and_market_match(const std::string& exchange_name,
                                               const std::string& market_type,
                                               const std::string& key) {
    std::vector<std::string> parts;
    std::stringstream ss(key);
    std::string part;

    while (std::getline(ss, part, ':')) {
        parts.push_back(part);
    }

    if (parts.size() != 3) {
        return false;
    }

    return parts[1] == exchange_name && parts[2] == market_type;
}

std::string Core::Utils::get_model_key(int strategy_id,
                                       const std::string& model_path) {
    return std::to_string(strategy_id) + ":" + model_path;
}

Core::MarketExchangeMarketAccountKey Core::Utils::get_full_key_parts(
    const std::string& key) {
    std::vector<std::string> parts;
    std::stringstream ss(key);
    std::string part;

    while (std::getline(ss, part, ':')) {
        parts.push_back(part);
    }

    if (parts.size() == 3) {
        return {parts[0], parts[1], parts[2], std::nullopt};
    } else if (parts.size() == 4) {
        return {parts[0], parts[1], parts[2], std::stoll(parts[3])};
    }
    return {};
}

bool Core::Utils::is_all_time_based_datasets_synced(
    const std::vector<std::shared_ptr<MarketData::OhlcvDataset>>& datasets,
    long long last_kline_close_time_ms) {
    for (const auto& dataset : datasets) {
        auto last_kline_close_time_ms = dataset->get_latest_bar_close_time_ms();

        if (!last_kline_close_time_ms.has_value()) {
            return false;
        }
        if (last_kline_close_time_ms.value() != last_kline_close_time_ms) {
            return false;
        }
    }
    return true;
}

mpfr::mpreal Core::Utils::calculate_exchange_distributed_quantity(
    const mpfr::mpreal& max_quote_sizing_notional,
    const mpfr::mpreal& sizing_coeff, const mpfr::mpreal& num_exchanges,
    double price) {
    if (sizing_coeff > 1 / num_exchanges) {
        return (max_quote_sizing_notional / num_exchanges) / price;
    }
    return (max_quote_sizing_notional) / price;
}

Core::OrderQuantityInfo
Core::Utils::calculate_exchange_distributed_quantity_info(
    const mpfr::mpreal& max_quote_sizing_notional,
    const mpfr::mpreal& sizing_coeff,
    const mpfr::mpreal& remaining_sizing_coeff,
    const mpfr::mpreal& num_exchanges, double price) {
    const auto orig_qty = max_quote_sizing_notional / price;
    const auto exchange_distributed_qty =
        calculate_exchange_distributed_quantity(
            max_quote_sizing_notional, sizing_coeff, num_exchanges, price);
    if (exchange_distributed_qty <= 0 || orig_qty <= 0) {
        return {0, 0};
    }

    const auto sizing_coeff_delta_scaling_factor =
        exchange_distributed_qty / orig_qty;

    auto actual_sizing_coeff_delta =
        sizing_coeff_delta_scaling_factor * sizing_coeff;

    if (actual_sizing_coeff_delta > remaining_sizing_coeff) {
        actual_sizing_coeff_delta = remaining_sizing_coeff;
    }

    auto adjusted_quantity = exchange_distributed_qty;
    if (actual_sizing_coeff_delta <
        sizing_coeff_delta_scaling_factor * sizing_coeff) {
        const auto adjustment_factor =
            actual_sizing_coeff_delta /
            (sizing_coeff_delta_scaling_factor * sizing_coeff);
        adjusted_quantity = exchange_distributed_qty * adjustment_factor;
    }

    return {adjusted_quantity, actual_sizing_coeff_delta};
}

std::pair<std::string, std::string> Core::Utils::infer_base_quote_assets(
    const std::string& symbol) {
    // USD listed last so longer USDT/USDC suffixes win (BTCUSDT -> USDT).
    // USD covers IBKR-shaped symbols (AAPLUSD) that don't match any
    // other quote.
    static const std::vector<std::string> quote_assets = {
        "USDT", "USDC", "BTC", "ETH", "BNB", "SOL", "USD"};

    for (const auto& quote : quote_assets) {
        if (symbol.size() > quote.size() &&
            symbol.compare(symbol.size() - quote.size(), quote.size(), quote) ==
                0) {
            std::string base = symbol.substr(0, symbol.size() - quote.size());
            return {base, quote};
        }
    }

    return {symbol, ""};
}

std::string Core::Utils::symbol_with_usdc_conversion(
    const std::string& symbol, const std::string& exchange_name,
    const std::string& market_type) {
    const auto& [base_asset, quote_asset] = infer_base_quote_assets(symbol);
    if (exchange_name == "binance" && market_type == "spot" &&
        quote_asset == "USDC") {
        return base_asset + "USDC";
    }
    return symbol;
}

double Core::Utils::calc_price_coeff_from_vol_and_trade_time_remaining(
    double normalized_vol, double trade_time_elapsed_factor, bool is_bid) {
    if (is_bid) {
        return 1.0 - normalized_vol * trade_time_elapsed_factor;
    } else {
        return 1.0 + normalized_vol * trade_time_elapsed_factor;
    }
}

double Core::Utils::calc_price_coeff_from_vol_and_trade_time_remaining_v2(
    double normalized_vol, long long trade_open_duration_ms,
    long long trade_max_time_ms, bool is_bid) {
    double effective_duration = 0;
    long long remaining = trade_open_duration_ms;
    int tier = 1;

    while (remaining > 0) {
        long long slice = std::min(remaining, trade_max_time_ms);
        effective_duration += static_cast<double>(slice) * tier;
        remaining -= slice;
        tier++;
    }

    double time_factor =
        1.0 - (effective_duration / static_cast<double>(trade_max_time_ms));

    if (is_bid) {
        return 1.0 - normalized_vol * time_factor;
    } else {
        return 1.0 + normalized_vol * time_factor;
    }
}

double Core::Utils::calc_price_coeff_from_simple_time_decay(
    double trade_time_elapsed_factor, double slope_coeff, bool is_bid) {
    if (is_bid) {
        return 1.0 + trade_time_elapsed_factor * slope_coeff;
    } else {
        return 1.0 - trade_time_elapsed_factor * slope_coeff;
    }
}

double Core::Utils::compute_mm_size_from_daily_turnover(double daily_turnover,
                                                        double min_size,
                                                        double max_size) {
    // first iter
    double size = daily_turnover * 0.0001;
    if (size < min_size) {
        return min_size;
    }
    if (size > max_size) {
        return max_size;
    }
    return size;
}

static int compute_ladder_tick_size_increment(double tick_size,
                                              double enter_price) {
    if (enter_price == 0 ||
        enter_price == std::numeric_limits<double>::infinity()) {
        return 1;
    }

    double tick_size_relative_to_price_coeff = tick_size / enter_price;
    if (tick_size_relative_to_price_coeff > 0.0008) {
        return 1;
    }

    if (tick_size_relative_to_price_coeff > 0.0008 / 2) {
        return 2;
    }

    if (tick_size_relative_to_price_coeff > 0.0008 / 3) {
        return 3;
    }

    return 4;
}

void Core::Utils::create_order_ladder(
    Core::StrategyDecision& strategy_decision, long long current_time_ms,
    Core::Engine& engine, Core::Strategy& strategy,
    Core::TradeTicker& trade_ticker, const std::optional<Core::Trade>& trade,
    const std::optional<Core::Signal>& prediction, double remaining_quantity,
    const Core::OrderLadderParams& lp) {
    bool is_alt_routed_binance_trade =
        lp.exchange_name == "binance" && lp.market_type == "spot";
    const std::string& symbol = !is_alt_routed_binance_trade
                                    ? trade_ticker.symbol
                                    : trade_ticker.base_asset + "USDC";

    double tick_size =
        engine.get_tick_size(symbol, lp.exchange_name, lp.market_type);
    double min_notional =
        engine.get_min_notional(symbol, lp.exchange_name, lp.market_type);
    double min_notional_in_base = min_notional / lp.reference_price;

    double curr_remaining = remaining_quantity;
    double max_quantity = remaining_quantity;

    int iter = 0;

    double price_direction = (lp.side == Core::OrderSide::BUY) ? -1.0 : 1.0;

    int tick_increment = 0;
    double order_size = 0;
    double price_multiplier_to_meet_min_notional = 1;

    if (lp.size_strategy == Core::OrderSizeStrategy::FROM_DAILY_TURNOVER) {
        if (!lp.daily_turnover.has_value()) {
            return;
        }

        double price_change_coeff = lp.start_price / lp.reference_price;
        price_multiplier_to_meet_min_notional = 1 / price_change_coeff;

        order_size = Core::Utils::compute_mm_size_from_daily_turnover(
            lp.daily_turnover.value(),
            std::max(min_notional * price_multiplier_to_meet_min_notional * 1.5,
                     lp.ensure_minimum_order_size_notional),
            lp.order_max_notional_quantity);
    }

    if (lp.price_change_strategy ==
        Core::TickSizeIncrementStrategy::DEFAULT_STRATEGY) {
        tick_increment =
            compute_ladder_tick_size_increment(tick_size, lp.reference_price);
    }

    int max_orders_in_ladder = lp.max_iterations;
    if (lp.limit_max_number_of_orders_in_ladder &&
        lp.max_number_of_orders_in_ladder.has_value()) {
        max_orders_in_ladder = lp.max_number_of_orders_in_ladder.value();
        order_size = std::max(
            (remaining_quantity / max_orders_in_ladder) * lp.reference_price,
            lp.ensure_minimum_order_size_notional);
    }

    if (order_size <= 0) {
        return;
    }

    int orders_created = 0;
    while (curr_remaining > 0 && iter < lp.max_iterations &&
           orders_created < max_orders_in_ladder) {
        double price =
            engine
                .round_price(symbol, lp.exchange_name, lp.market_type,
                             lp.start_price + price_direction *
                                                  (tick_size * tick_increment) *
                                                  iter)
                .toDouble();

        double order_quantity = std::min(order_size / price, curr_remaining);

        double remaining_after = curr_remaining - order_quantity;
        if (remaining_after > 0 &&
            remaining_after <
                min_notional_in_base *
                    (price_multiplier_to_meet_min_notional * 1.5)) {
            order_quantity = curr_remaining;
        }

        auto kv_cache = lp.kv_cache;
        kv_cache["order_ladder_iteration"] = static_cast<double>(iter);

        Core::CreateOrderParams params{
            .symbol = trade_ticker.symbol,
            .type = Core::OrderType::LIMIT,
            .side = lp.side,
            .price = price,
            .prediction = prediction,
            .quantity = order_quantity,
            .exchange_name = lp.exchange_name,
            .market_type = lp.market_type,
            .strategy_id = strategy.strategy_config.id,
            .expires_on_ms =
                current_time_ms + lp.expires_on_delta_to_present_time_ms,
            .sizing_coeff_delta = order_quantity / max_quantity,
            .trade_id = trade.has_value() ? std::make_optional(trade.value().id)
                                          : std::nullopt,
            .reduce_only = lp.reduce_only,
            .timestamp = current_time_ms,
            .quote_asset = trade_ticker.quote_asset,
            .base_asset = trade_ticker.base_asset,
            .allowed_to_bypass_risk_engine = false,
            .must_pass_min_notional_filter = true,
            .is_trade_close_order = lp.is_trade_close_order,
            .is_alt_market_routed = is_alt_routed_binance_trade,
            .route_order_to_alt_symbol = symbol,
            .routed_order_base_asset = trade_ticker.base_asset,
            .routed_order_quote_asset =
                is_alt_routed_binance_trade ? "USDC" : "USDT",
            .margin_auto_borrow = lp.margin_auto_borrow,
            .margin_auto_repay = lp.margin_auto_repay,
            .maximum_notional_quantity = lp.order_max_notional_quantity,
            .type_internal = lp.type_internal,
            .kv_cache = kv_cache,
            .is_randomized_size = lp.is_randomized_size,
            .is_randomized_delay_between_orders =
                lp.is_randomized_delay_between_orders,
            .randomized_size_lower_bound = lp.randomized_size_lower_bound,
            .randomized_size_upper_bound = lp.randomized_size_upper_bound,
            .randomized_delay_between_orders_ms_lower_bound =
                lp.randomized_delay_between_orders_ms_lower_bound,
            .randomized_delay_between_orders_ms_upper_bound =
                lp.randomized_delay_between_orders_ms_upper_bound,
            .is_maximum_slippage_enabled = lp.is_maximum_slippage_enabled,
            .signal_start_price = lp.signal_start_price,
            .maximum_slippage_coeff = lp.maximum_slippage_coeff,
            .prevent_overlapping_orders = lp.prevent_overlapping_orders,
            .limit_number_of_open_orders = lp.limit_number_of_open_orders,
            .max_number_of_open_orders = lp.max_number_of_open_orders,
            .ensure_post_only = lp.ensure_post_only,
            .best_bid_price = lp.best_bid_price,
            .best_ask_price = lp.best_ask_price,
            .randomize_expiration_time = lp.randomize_expiration_time,
            .randomized_expiration_time_ms_lower_bound =
                lp.randomized_expiration_time_ms_lower_bound,
            .randomized_expiration_time_ms_upper_bound =
                lp.randomized_expiration_time_ms_upper_bound,
            .prevent_price_exceeding_threshold =
                lp.prevent_price_exceeding_threshold,
            .safe_price_above_current_price_threshold =
                lp.safe_price_above_current_price_threshold,
            .automatically_add_orderbook_prices_to_params =
                lp.automatically_add_orderbook_prices_to_params,
            .limit_strategy_level_enter_order_rate =
                lp.limit_strategy_level_enter_order_rate,
            .limit_ticker_level_enter_order_rate =
                lp.limit_ticker_level_enter_order_rate,
            .limit_strategy_level_exit_order_rate =
                lp.limit_strategy_level_exit_order_rate,
            .limit_ticker_level_exit_order_rate =
                lp.limit_ticker_level_exit_order_rate,
            .max_strategy_level_enter_orders_per_10s =
                lp.max_strategy_level_enter_orders_per_10s,
            .max_ticker_level_enter_orders_per_10s =
                lp.max_ticker_level_enter_orders_per_10s,
            .max_strategy_level_exit_orders_per_10s =
                lp.max_strategy_level_exit_orders_per_10s,
            .max_ticker_level_exit_orders_per_10s =
                lp.max_ticker_level_exit_orders_per_10s,
            .only_post_if_price_within_threshold =
                lp.only_post_if_price_within_threshold,
            .price_within_threshold_percent = lp.price_within_threshold_percent,
            .max_notional_resting_on_book = lp.max_notional_resting_on_book,
            .max_unacked_enter_orders = lp.max_unacked_enter_orders,
            .max_unacked_exit_orders = lp.max_unacked_exit_orders,
        };

        auto [result, order] =
            trade_ticker.construct_order(engine, strategy, params);

        if (result == Core::OrderConstructionResult::VALID &&
            order.has_value()) {
            trade_ticker.mark_order_to_be_created(engine, order.value(),
                                                  strategy_decision, strategy);
            curr_remaining -= order_quantity;
            orders_created++;
        }
        if (result ==
                Core::OrderConstructionResult::OVERLAPPING_BID_ORDER_EXISTS ||
            result ==
                Core::OrderConstructionResult::OVERLAPPING_ASK_ORDER_EXISTS) {
            iter++;
            continue;
        }

        if (result != Core::OrderConstructionResult::VALID) {
            return;
        }

        iter++;
    }
}

void Core::Utils::cancel_passive_take_profit_orders_for_trade(
    Core::Engine& engine, Core::StrategyDecision& strategy_decision,
    Core::TradeTicker& trade_ticker, const Core::Trade& trade) {
    auto orders = trade_ticker.get_orders(false);
    for (auto& order : orders) {
        if (order.trade_id != trade.id) {
            continue;
        }
        if (order.type_internal ==
            Core::OrderTypeInternal::PASSIVE_TAKE_PROFIT) {
            if (trade_ticker.is_cancel_already_sent(order)) {
                continue;
            }
            trade_ticker.mark_order_to_be_cancelled(engine, order,
                                                    strategy_decision);
        }
    }
}

void Core::Utils::cancel_enter_trade_orders(
    Core::Engine& engine, Core::StrategyDecision& strategy_decision,
    Core::TradeTicker& trade_ticker) {
    auto orders = trade_ticker.get_orders(false);
    for (auto& order : orders) {
        if (!order.is_trade_close_order) {
            trade_ticker.mark_order_to_be_cancelled(engine, order,
                                                    strategy_decision);
        }
    }
}

void Core::Utils::cancel_stale_orders_by_price_threshold(
    Core::Engine& engine, Core::StrategyDecision& strategy_decision,
    Core::TradeTicker& trade_ticker, double threshold_pct) {
    auto orders = trade_ticker.get_orders(false);

    std::unordered_map<std::string, std::pair<double, double>> bid_ask_cache;

    for (auto& order : orders) {
        if (trade_ticker.is_cancel_already_sent(order)) {
            continue;
        }

        std::string cache_key = get_symbol_exchange_market_key(
            order.symbol, order.exchange_name, order.market_type);

        double best_bid, best_ask;
        auto cache_it = bid_ask_cache.find(cache_key);
        if (cache_it != bid_ask_cache.end()) {
            best_bid = cache_it->second.first;
            best_ask = cache_it->second.second;
        } else {
            auto best_bid_opt = engine.get_best_bid(
                order.symbol, order.exchange_name, order.market_type);
            auto best_ask_opt = engine.get_best_ask(
                order.symbol, order.exchange_name, order.market_type);

            if (!best_bid_opt.has_value() || !best_ask_opt.has_value()) {
                continue;
            }

            best_bid = best_bid_opt.value();
            best_ask = best_ask_opt.value();
            bid_ask_cache[cache_key] = {best_bid, best_ask};
        }

        double order_price = order.price.toDouble();

        if (order.side == Core::OrderSide::BUY) {
            double diff_pct = (best_bid - order_price) / best_bid;
            if (diff_pct > threshold_pct) {
                trade_ticker.mark_order_to_be_cancelled(engine, order,
                                                        strategy_decision);
            }
        } else {
            double diff_pct = (order_price - best_ask) / order_price;
            if (diff_pct > threshold_pct) {
                trade_ticker.mark_order_to_be_cancelled(engine, order,
                                                        strategy_decision);
            }
        }
    }
}