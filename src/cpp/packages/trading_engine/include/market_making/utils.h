#pragma once

#include "market_making/dtypes/def_only_structs.h"
#include "market_making/dtypes/order.h"
#include "market_making/dtypes/order_fill_info.h"
#include "market_making/dtypes/trade.h"
#include "mpreal.h"

namespace MarketData {
class OhlcvDataset;
}

namespace Core {

class StrategyDecision;
class Engine;
class Strategy;
class TradeTicker;
class OrderLadderParams;

struct Inventory;

struct MarketExchangeMarketAccountKey {
    std::optional<std::string> symbol;
    std::optional<std::string> exchange_name;
    std::optional<std::string> market_type;
    std::optional<long long> account_id;
};

namespace Utils {

mpfr::mpreal get_order_fill_size_delta(const Core::Order& order,
                                       const Core::OrderFillInfo& fill_info);

std::string get_exchange_market_key(const std::string& exchange_name,
                                    const std::string& market_type);

std::string get_exchange_market_account_key(const std::string& exchange_name,
                                            const std::string& market_type,
                                            long long account_id);

std::string symbol_with_usdc_conversion(const std::string& symbol,
                                        const std::string& exchange_name,
                                        const std::string& market_type);

std::pair<std::string, std::string> infer_base_quote_assets(
    const std::string& symbol);

std::string get_order_key(const Core::Order& order);

bool is_same_order(const Core::Order& a, const Core::Order& b);

bool is_same_order(const Core::Order& order, const std::string& client_order_id,
                   long long account_id, const std::string& symbol,
                   const std::string& exchange_name,
                   const std::string& market_type);

bool is_same_order_by_client_and_account(const Core::Order& a,
                                         const Core::Order& b);

bool is_same_order_by_client_and_account(const Core::Order& order,
                                         const std::string& client_order_id,
                                         long long account_id);

bool is_same_order_by_client_id(const Core::Order& a, const Core::Order& b);

bool is_same_order_by_client_id(const Core::Order& order,
                                const std::string& client_order_id);

bool is_same_order_adaptive(const Core::Order& order,
                            const std::string& client_order_id,
                            const std::string& exchange_order_id);

bool is_same_order_adaptive(const Core::Order& order,
                            const std::string& client_order_id,
                            const std::string& exchange_order_id,
                            long long account_id);

std::string get_symbol_exchange_market_key(const std::string& symbol,
                                           const std::string& exchange_name,
                                           const std::string& market_type);

std::string get_symbol_exchange_market_account_key(
    const std::string& symbol, const std::string& exchange_name,
    const std::string& market_type, long long account_id);

std::string get_strategy_symbol_exchange_market_key(
    const std::string& symbol, const std::string& exchange_name,
    const std::string& market_type, const int strategy_id);

std::string get_exchange_dataset_key(const std::string& symbol,
                                     const std::string& exchange_name,
                                     const std::string& market_type,
                                     long long interval_ms);

double calc_price_coeff_from_vol_and_trade_time_remaining(
    double normalized_vol, double trade_time_elapsed_factor, bool is_bid);

double calc_price_coeff_from_vol_and_trade_time_remaining_v2(
    double normalized_vol, long long trade_open_duration_ms,
    long long trade_max_time_ms, bool is_bid);

double calc_price_coeff_from_simple_time_decay(double trade_time_elapsed_factor,
                                               double slope_coeff, bool is_bid);

std::string get_interval_str_from_ohlcv_tick(long long interval_ms);

// Net signed open-quantity sum across all trades for the symbol. LONG
// trades count positive, SHORT trades count negative — back-compat.
mpfr::mpreal get_acc_symbol_trade_open_quantity(
    const std::vector<Core::Trade>& trades, const std::string& symbol,
    const std::string& exchange_name, const std::string& market_type,
    long long account_id);

// Side-filtered open-quantity sum (gross, unsigned). Pass LONG_SIDE to get
// total open size across LONG trades only, SHORT_SIDE for SHORT trades.
// Use this when the inventory validator needs to compare per-side against
// hedge-mode exchange positions.
mpfr::mpreal get_acc_symbol_trade_open_quantity_by_side(
    const std::vector<Core::Trade>& trades, const std::string& symbol,
    const std::string& exchange_name, const std::string& market_type,
    long long account_id, Core::PositionSide side);

int get_acc_number_of_active_trades(const std::vector<Core::Trade>& trades,
                                    const std::string& symbol,
                                    const std::string& exchange_name,
                                    const std::string& market_type,
                                    long long account_id);

// Net inventory total for the (symbol, exchange, market, account) key.
mpfr::mpreal get_account_symbol_inventory(
    const std::string& symbol, const std::string& exchange_name,
    const std::string& market_type, long long account_id,
    const std::unordered_map<std::string, Core::Inventory>& inventory);

// Side-filtered inventory total (gross, unsigned). Pass LONG_SIDE for
// long_total, SHORT_SIDE for short_total. Returns 0 if the key is absent.
mpfr::mpreal get_account_symbol_inventory_by_side(
    const std::string& symbol, const std::string& exchange_name,
    const std::string& market_type, long long account_id,
    const std::unordered_map<std::string, Core::Inventory>& inventory,
    Core::PositionSide side);

std::vector<std::pair<mpfr::mpreal, std::vector<Trade>>>
get_symbol_all_trade_open_quantity_combinations(
    const std::vector<Core::Trade>& trades, const std::string& symbol,
    const std::string& exchange_name, const std::string& market_type,
    long long account_id);

bool is_exchange_and_market_match(const std::string& exchange_name,
                                  const std::string& market_type,
                                  const std::string& key);
std::string get_model_key(int strategy_id, const std::string& model_path);
MarketExchangeMarketAccountKey get_full_key_parts(const std::string& key);

bool is_all_time_based_datasets_synced(
    const std::vector<std::shared_ptr<::MarketData::OhlcvDataset>>& datasets,
    long long last_kline_close_time_ms);

mpfr::mpreal calculate_exchange_distributed_quantity(
    const mpfr::mpreal& max_quote_sizing_notional,
    const mpfr::mpreal& sizing_coeff, const mpfr::mpreal& num_exchanges,
    double price);

double compute_mm_size_from_daily_turnover(double daily_turnover,
                                           double min_size, double max_size);
void create_order_ladder(Core::StrategyDecision& strategy_decision,
                         long long current_time_ms, Core::Engine& engine,
                         Core::Strategy& strategy,
                         Core::TradeTicker& trade_ticker,
                         const std::optional<Core::Trade>& trade,
                         const std::optional<Core::Signal>& prediction,
                         double remaining_quantity,
                         const Core::OrderLadderParams& lp);

void cancel_passive_take_profit_orders_for_trade(
    Core::Engine& engine, Core::StrategyDecision& strategy_decision,
    Core::TradeTicker& trade_ticker, const Core::Trade& trade);

void cancel_enter_trade_orders(Core::Engine& engine,
                               Core::StrategyDecision& strategy_decision,
                               Core::TradeTicker& trade_ticker);

void cancel_stale_orders_by_price_threshold(
    Core::Engine& engine, Core::StrategyDecision& strategy_decision,
    Core::TradeTicker& trade_ticker, double threshold_pct = 0.03);

OrderQuantityInfo calculate_exchange_distributed_quantity_info(
    const mpfr::mpreal& max_quote_sizing_notional,
    const mpfr::mpreal& sizing_coeff,
    const mpfr::mpreal& remaining_sizing_coeff,
    const mpfr::mpreal& num_exchanges, double price);

}  // namespace Utils
};  // namespace Core
