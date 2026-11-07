#pragma once

#include <optional>
#include <string>
#include <vector>

#include "market_making/dtypes/enums.h"
#include "market_making/dtypes/order_fill_info.h"
#include "market_making/dtypes/prediction.h"
#include "mpreal.h"

namespace Core {

class Engine;

enum TradeTypeInternal {
    DIRECTIONAL_TRADE = 0,
    HEDGE = 1,
};

struct Trade {
    int strategy_id;
    int num_expired_exit_orders = 0;
    int num_alfa_signal_override_cancel_orders = 0;
    std::string symbol;
    std::string exchange_name;
    std::string market_type;
    mpfr::mpreal open_price;
    mpfr::mpreal close_price;
    mpfr::mpreal open_quantity = 0;
    mpfr::mpreal closed_quantity = 0;
    long long account_id;
    mpfr::mpreal pnl = 0;
    mpfr::mpreal pnl_percentage = 0;
    mpfr::mpreal fees = 0;

    long long open_time_ms;
    long long enter_signal_received_time_ms;
    long long close_time_ms;
    long long exit_signal_received_time_ms = 0;

    unsigned long long id;
    std::string id_str;
    unsigned long long signal_id = 0;
    TradeType type;
    mpfr::mpreal sizing_coeff_delta;
    std::vector<unsigned long long> exit_order_ids;
    std::vector<unsigned long long> enter_order_ids;
    std::vector<OrderFillInfo> open_fills;
    std::vector<OrderFillInfo> close_fills;
    std::optional<Signal> prediction;
    std::unordered_map<std::string, double> kv_cache = {};
    TradeTypeInternal type_internal = TradeTypeInternal::DIRECTIONAL_TRADE;
    std::optional<unsigned long long> hedge_for_trade_id = std::nullopt;
    std::optional<std::string> hedge_for_symbol = std::nullopt;
    long long db_id = 0;
    std::string clob_token_id = "";
    TradeCreationType trade_creation_type = TradeCreationType::ORGANIC;
    bool is_live_trade = false;

    mpfr::mpreal get_unique_order_fills_on_trade();
    std::pair<mpfr::mpreal, mpfr::mpreal> get_open_and_close_price() const;
    mpfr::mpreal get_average_open_price() const;
    std::pair<std::vector<OrderFillInfo>, std::vector<OrderFillInfo>>
    get_unique_finished_fills() const;

    mpfr::mpreal calculate_trade_fee(Engine &engine) const;
    mpfr::mpreal calculate_pnl() const;
    mpfr::mpreal calculate_remaining_notional(double price) const;
    mpfr::mpreal calculate_remaining_quantity() const;
    long long get_time_since_open(Engine &engine) const;
    std::optional<long long> get_time_since_last_exit_fill(Engine &engine) const;

    void set_kv_cache(const std::unordered_map<std::string, double> &kv_cache);

    nlohmann::json to_json() const;

    void on_exit_fill(const OrderFillInfo &fill);
    void on_enter_fill(const OrderFillInfo &fill,
                       const mpfr::mpreal &fill_delta);
    void revert_enter_fill(const OrderFillInfo &fill);
    void revert_exit_fill(const OrderFillInfo &fill);
    bool has_unfinalized_fills() const;
    int count_open_fills() const;
    std::optional<std::reference_wrapper<const OrderFillInfo>>
    get_prev_fill_with_highest_fill_amount(unsigned long long order_id) const;

    static Trade from_order_fill(const OrderFillInfo &fill,
                                 const mpfr::mpreal &fill_delta);

    static mpfr::mpreal calculate_base_asset_fee_deduction(
        const OrderFillInfo &fill, const mpfr::mpreal &fill_delta,
        const std::vector<OrderFillInfo> &open_fills = {});

    static Trade from_json(const nlohmann::json &json);

    nlohmann::json to_db_json(bool is_live_trade, bool is_closed_trade,
                              std::optional<long long> db_id) const;

    static Trade minimal_from_db_json(const nlohmann::json &json);
    static Trade from_db_json_full(const nlohmann::json &json);

    static void calculate_pnl_and_fees(Engine &engine, Core::Trade &trade);

    bool operator==(const Trade &other) const {
        return this->id == other.id && this->symbol == other.symbol &&
               this->exchange_name == other.exchange_name &&
               this->market_type == other.market_type;
    }

    bool operator!=(const Trade &other) const { return !(*this == other); }
};

}  // namespace Core