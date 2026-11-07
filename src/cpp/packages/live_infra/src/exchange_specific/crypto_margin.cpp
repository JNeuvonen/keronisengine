#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_set>

#include "cache_path_utils.h"
#include "ccxt_proxy.h"
#include "ccxt_utils.h"
#include "common_types/log_event_types.h"
#include "datetime_utils.h"
#include "disk_logging/log.h"
#include "generic_utils.h"
#include "live_infra/state_updater.h"
#include "market_making/dtypes/def_only_structs.h"
#include "market_making/dtypes/enums.h"
#include "market_making/dtypes/exchange_info.h"
#include "market_making/dtypes/order.h"
#include "market_making/dtypes/order_res.h"
#include "market_making/engine.h"
#include "market_making/utils.h"
#include "sqlite_cache.h"

namespace {

// Cadence gate for orphan-order reconciliation + per-account running latch.
// The latch prevents a slow HTTP fetch on one account from being stacked by
// the next poll tick (which would hammer the exchange and duplicate work).
std::chrono::steady_clock::time_point s_last_orphan_cancel_poll{};
std::mutex s_orphan_in_flight_mutex;
std::unordered_set<std::string> s_orphan_in_flight_keys;

bool try_claim_orphan_run(const std::string &key) {
    std::lock_guard<std::mutex> lock(s_orphan_in_flight_mutex);
    return s_orphan_in_flight_keys.insert(key).second;
}

void release_orphan_run(const std::string &key) {
    std::lock_guard<std::mutex> lock(s_orphan_in_flight_mutex);
    s_orphan_in_flight_keys.erase(key);
}

bool order_writes_disabled() {
    static const bool v = []() {
        const char *e = std::getenv("IS_ORDER_CREATION_DISABLED");
        return e != nullptr && std::string(e) == "1";
    }();
    return v;
}

void cancel_orphaned_orders_for_account(Live::StateUpdater &updater,
                                        const std::string &exchange_name,
                                        const std::string &market_type,
                                        long long account_id) {
    const std::string key =
        exchange_name + ":" + market_type + ":" + std::to_string(account_id);
    if (!try_claim_orphan_run(key)) {
        // Previous run for this account still in flight; skip this tick.
        return;
    }
    std::thread([&updater, exchange_name, market_type, account_id, key]() {
        try {
            auto &proxy = CcxtProxy::instance();
            auto &engine = Core::Engine::getInstance();

            auto exchange_orders = proxy.get_all_open_orders(
                exchange_name, market_type, account_id);

            // Engine snapshot: regular tracked orders + the misc map (where
            // NONTRACKED_INVENTORY_CLOSE orders live). Exchange orders are
            // matched against these via Core::Utils::is_same_order_adaptive —
            // the single canonical order-identity matcher used across the
            // engine (binance/swap matches on client_order_id; venues with
            // uses_exchange_order_id_for_lookup match on exchange_order_id).
            // The engine does not populate exchange_order_id for ccxt venues,
            // so an exchange_order_id-keyed diff here flagged the whole live
            // book as orphans. We DO NOT touch engine state based on the
            // exchange view — pruning engine orders from absence at the
            // exchange races with the fill / cancel / watchdog lifecycle paths
            // that already manage local state authoritatively. Read-only diff,
            // then cancel exchange-side orphans only.
            std::vector<Core::Order> engine_orders;
            size_t engine_order_count = 0;
            for (const auto &order : engine.get_orders()) {
                if (order.exchange_name != exchange_name) continue;
                if (order.market_type != market_type) continue;
                if (order.account_id != account_id) continue;
                ++engine_order_count;
                engine_orders.push_back(order);
            }
            for (const auto &[_, misc_orders] :
                 engine.get_tracked_misc_orders()) {
                for (const auto &order : misc_orders) {
                    if (order.exchange_name != exchange_name) continue;
                    if (order.market_type != market_type) continue;
                    if (order.account_id != account_id) continue;
                    ++engine_order_count;
                    engine_orders.push_back(order);
                }
            }

            // Direction 1: exchange has, engine doesn't → cancel at exchange.
            int cancelled_orphans = 0;
            for (const auto &eo : exchange_orders) {
                bool tracked_by_engine = false;
                for (const auto &order : engine_orders) {
                    if (Core::Utils::is_same_order_adaptive(
                            order, eo.client_order_id, eo.exchange_order_id,
                            account_id)) {
                        tracked_by_engine = true;
                        break;
                    }
                }
                if (tracked_by_engine) continue;
                if (eo.client_order_id.empty()) {
                    // Without a client_order_id we can't drive the cancel
                    // via the cancel_order_v2 path; log and skip.
                    LOG_WARNING(
                        Core::TRADING_LOG_EVENT_TYPE_MISC_ORDER_CLEANUP,
                        std::string("Orphan exchange order has no "
                                    "client_order_id, skipping cancel: ") +
                            eo.exchange_order_id,
                        (nlohmann::json{
                            {"exchange_name", exchange_name},
                            {"market_type", market_type},
                            {"account_id", account_id},
                            {"exchange_order_id", eo.exchange_order_id},
                            {"symbol", eo.symbol}}));
                    continue;
                }
                LOG_WARNING(
                    Core::TRADING_LOG_EVENT_TYPE_MISC_ORDER_CLEANUP,
                    std::string("Cancelling orphan exchange order ") +
                        eo.exchange_order_id + " (engine has no record)",
                    (nlohmann::json{
                        {"direction", "exchange_orphan"},
                        {"exchange_name", exchange_name},
                        {"market_type", market_type},
                        {"account_id", account_id},
                        {"exchange_order_id", eo.exchange_order_id},
                        {"client_order_id", eo.client_order_id},
                        {"symbol", eo.symbol},
                        {"price", eo.price.toString()},
                        {"orig_quantity", eo.orig_quantity.toString()},
                        {"filled_quantity", eo.filled_quantity.toString()}}));
                proxy.cancel_order_v2(
                    eo.client_order_id, exchange_name, market_type,
                    CCXTUtils::symbol_to_ccxt_format(eo.symbol, market_type),
                    account_id);
                ++cancelled_orphans;
            }

            LOG_INFO(
                Core::TRADING_LOG_EVENT_TYPE_MISC_ORDER_CLEANUP,
                "cancel_orphaned_orders: exchange=" + exchange_name +
                    " market=" + market_type +
                    " account=" + std::to_string(account_id) +
                    " engine=" + std::to_string(engine_order_count) +
                    " exchange=" + std::to_string(exchange_orders.size()) +
                    " cancelled_at_exchange=" +
                    std::to_string(cancelled_orphans),
                (nlohmann::json{{"exchange_name", exchange_name},
                                {"market_type", market_type},
                                {"account_id", account_id},
                                {"engine_orders", engine_order_count},
                                {"exchange_orders", exchange_orders.size()},
                                {"cancelled_at_exchange",
                                 cancelled_orphans}}));
        } catch (const std::exception &e) {
            LOG_ERROR(
                Core::TRADING_LOG_EVENT_TYPE_MISC_ORDER_CLEANUP,
                std::string("cancel_orphaned_orders exception: ") + e.what(),
                (nlohmann::json{{"exchange_name", exchange_name},
                                {"market_type", market_type},
                                {"account_id", account_id},
                                {"what", e.what()}}));
        } catch (...) {
            LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_MISC_ORDER_CLEANUP,
                      "cancel_orphaned_orders unknown exception",
                      (nlohmann::json{{"exchange_name", exchange_name},
                                      {"market_type", market_type},
                                      {"account_id", account_id}}));
        }
        release_orphan_run(key);
    }).detach();
}

}  // namespace

void Live::StateUpdater::poll_cancel_orphaned_orders() {
    auto now = std::chrono::steady_clock::now();
    if (now - s_last_orphan_cancel_poll < std::chrono::minutes(5)) return;
    s_last_orphan_cancel_poll = now;

    for (auto &[account_id, account] : this->accounts) {
        if (account.exchange_name == "binance" &&
            account.market_type == "swap") {
            cancel_orphaned_orders_for_account(*this, account.exchange_name,
                                               account.market_type, account_id);
        }
    }
}

void Live::StateUpdater::close_nontracked_inventory(
    const std::vector<Core::NonTrackedInventoryToBeClosed>
        nontracked_inventory_to_be_closed) {
    const auto &nontracked_inventory_close_orders =
        Core::Engine::getInstance().get_tracked_misc_orders();

    for (const auto &inventory : nontracked_inventory_to_be_closed) {
        const auto &key = Core::Utils::get_symbol_exchange_market_account_key(
            inventory.symbol, inventory.exchange_name, inventory.market_type,
            inventory.account_id);

        Core::Engine::getInstance().create_system_log(
            inventory.symbol, Core::LOG_LEVEL_INFO,
            "Closing nontracked inventory for: " + key,
            Core::TRADING_LOG_EVENT_TYPE_INVENTORY_CLOSE_ORDER_CREATED,
            inventory.to_json(), -1);

        const auto &stall_order_time =
            this->nontracked_positions_close_times_cache->get(key);
        const auto time_now_ms = DatetimeUtils::get_current_time_ms();

        if (stall_order_time.has_value()) {
            if (time_now_ms - stall_order_time.value() < 28800000) {  // 8h
                continue;
            }
        }
        this->nontracked_positions_close_times_cache->insert(
            key, DatetimeUtils::get_current_time_ms());

        // Per-side excess: when the validator runs on a hedge-mode venue
        // (binance USDS-M futures) it tags each NonTrackedInventoryToBeClosed
        // with the side (LONG / SHORT) and `quantity` is the gross unsigned
        // magnitude. When run on a non-hedge venue, position_side is nullopt
        // and `quantity` carries the legacy signed delta (positive = long
        // excess, negative = short excess) so we keep the old sign-infer
        // behavior for those paths.
        bool is_long_side_excess_position;
        double remaining_quantity;
        if (inventory.position_side.has_value()) {
            is_long_side_excess_position = inventory.position_side.value() ==
                                           Core::PositionSide::LONG_SIDE;
            remaining_quantity = std::abs(inventory.quantity.toDouble());
        } else {
            remaining_quantity = inventory.quantity.toDouble();
            is_long_side_excess_position = remaining_quantity > 0;
            remaining_quantity = std::abs(remaining_quantity);
        }

        std::vector<Core::Order> already_sent_orders;
        double quantity_in_book = 0;

        for (const auto &[key, orders] : nontracked_inventory_close_orders) {
            for (const auto &order : orders) {
                if (order.symbol == inventory.symbol &&
                    order.exchange_name == inventory.exchange_name &&
                    order.market_type == inventory.market_type &&
                    order.account_id == inventory.account_id &&
                    order.type_internal ==
                        Core::OrderTypeInternal::NONTRACKED_INVENTORY_CLOSE) {
                    already_sent_orders.push_back(order);
                    quantity_in_book += order.quantity.toDouble();
                }
            }
        }

        remaining_quantity -= quantity_in_book;

        while (remaining_quantity > 0) {
            const auto &price = Core::Engine::getInstance().get_price(
                inventory.symbol, inventory.exchange_name,
                inventory.market_type);

            if (!price.has_value()) {
                break;
            }

            auto order_close_qty = 200 / price.value();
            order_close_qty = std::min(order_close_qty, remaining_quantity);
            double order_price = is_long_side_excess_position
                                     ? price.value() * 0.9875
                                     : price.value() * 1.0125;
            Core::OrderSide side = is_long_side_excess_position
                                       ? Core::OrderSide::SELL
                                       : Core::OrderSide::BUY;

            const auto &[base_asset, quote_asset] =
                CCXTUtils::infer_base_quote_assets(inventory.symbol);

            bool route_to_usdc_pair = inventory.exchange_name == "binance" &&
                                      inventory.market_type == "spot";

            Core::CreateOrderParams params{
                .symbol =
                    route_to_usdc_pair ? base_asset + "USDC" : inventory.symbol,
                .type = Core::OrderType::LIMIT,
                .side = side,
                .price = order_price,
                .prediction = std::nullopt,
                .quantity = side == Core::OrderSide::SELL
                                ? order_close_qty - (1 / price.value())
                                : order_close_qty,
                .exchange_name = inventory.exchange_name,
                .market_type = inventory.market_type,
                .strategy_id = -1,
                .expires_on_ms =
                    DatetimeUtils::get_current_time_ms() + 60000 * 60,
                .sizing_coeff_delta = 0,
                .trade_id = std::nullopt,
                .reduce_only = false,
                .timestamp = DatetimeUtils::get_current_time_ms(),
                .quote_asset = route_to_usdc_pair ? "USDC" : quote_asset,
                .base_asset = base_asset,
                .allowed_to_bypass_risk_engine = true,
                .must_pass_min_notional_filter = true,
                .is_trade_close_order = false,
                .kv_cache = {},
                .hedge_for_trade_id = std::nullopt,
                // Tag the order with the hedge leg we're closing against.
                // Long excess → close LONG leg via a SELL; short excess →
                // close SHORT leg via a BUY. Previously hard-coded to
                // LONG_SIDE, which silently mis-routed every short-leg
                // close on hedge-mode accounts (Binance applied the BUY
                // against whichever leg satisfied the order, often opening
                // a fresh long instead of reducing the short).
                .position_side = is_long_side_excess_position
                                     ? Core::PositionSide::LONG_SIDE
                                     : Core::PositionSide::SHORT_SIDE};

            params.account_id = inventory.account_id;
            params.type_internal =
                Core::OrderTypeInternal::NONTRACKED_INVENTORY_CLOSE;

            auto order = Core::Order::create(
                params.symbol, params.type, params.side, params.price,
                params.prediction, params.quantity, params.exchange_name,
                params.market_type, params.strategy_id, params.expires_on_ms,
                params.sizing_coeff_delta, params.trade_id, params.reduce_only,
                params.timestamp, params.quote_asset, params.base_asset,
                params.allowed_to_bypass_risk_engine,
                params.is_trade_close_order, params.type_internal,
                params.is_alt_market_routed, params.route_order_to_alt_symbol,
                params.routed_order_base_asset, params.routed_order_quote_asset,
                params.margin_auto_borrow, params.margin_auto_repay,
                params.derivatives_settle_currency, params.kv_cache,
                params.hedge_for_trade_id, params.account_id.value(),
                params.position_side);

            Core::Engine::getInstance().round_quantity_and_price(order);

            if (order.quantity.toDouble() * order_price < 5) {
                break;
            }

            CcxtProxy::instance().create_order(order);
            Core::Engine::getInstance().add_tracked_misc_order(order);

            Core::Engine::getInstance().create_system_log(
                order.symbol, Core::LOG_LEVEL_INFO,
                "Creating nontracked inventory close order for: " +
                    order.symbol +
                    " on account: " + std::to_string(order.account_id),
                Core::TRADING_LOG_EVENT_TYPE_INVENTORY_CLOSE_ORDER_CREATED,
                order.to_json(), -1, inventory.exchange_name,
                inventory.market_type);

            std::this_thread::sleep_for(std::chrono::seconds(5));
            remaining_quantity -= order_close_qty;
        }
    }
}

void Live::StateUpdater::repay_loans_with_available_funds() {
    std::unordered_set<std::string> processed_keys;

    for (auto &[account_id, account] : this->accounts) {
        if (account.market_type != "spot") {
            continue;
        }
        if (account.exchange_name == "polymarket") {
            continue;
        }

        const auto &full_account_state =
            Core::Engine::getInstance().get_full_account_state(account_id);

        std::vector<std::string> active_symbols =
            Core::ExchangeInfo::getInstance().get_active_symbols(
                account.exchange_name, account.market_type);

        for (const auto &symbol : active_symbols) {
            for (const auto &inventory : full_account_state.inventories) {
                if (symbol != inventory.second.quote_conversion_symbol) {
                    continue;
                }

                const auto &price = Core::Engine::getInstance().get_price(
                    inventory.second.quote_conversion_symbol,
                    account.exchange_name, account.market_type);

                if (!price.has_value()) {
                    continue;
                }
                if (inventory.second.debt > 0) {
                    // const auto num_open_trades =
                    //     Core::Utils::get_acc_number_of_active_trades(
                    //         full_account_state.trades, symbol,
                    //         account.exchange_name, account.market_type,
                    //         account_id);

                    // if (num_open_trades > 0) {
                    //     continue;
                    // }

                    // const auto orders =
                    // Core::Engine::getInstance().get_orders(
                    //     symbol, account.exchange_name, account.market_type);

                    // if (!orders.empty()) {
                    //     continue;
                    // }

                    // const auto &last_closed_trade_time =
                    //     this->get_symbol_last_closed_trade_time(
                    //         symbol, account.exchange_name,
                    //         account.market_type);

                    // if (last_closed_trade_time.has_value()) {
                    //     if (DatetimeUtils::get_current_time_ms() -
                    //             last_closed_trade_time.value() <
                    //         43200000) {
                    //         continue;
                    //     }
                    // }

                    if (inventory.second.free > 0) {
                        const double step_size =
                            Core::Engine::getInstance().get_step_size(
                                symbol, account.exchange_name,
                                account.market_type);
                        const double free_asset =
                            inventory.second.free.toDouble() - step_size;
                        const double debt = inventory.second.debt.toDouble();

                        const double amount_to_repay =
                            std::min(free_asset, debt);

                        if (amount_to_repay < 0) {
                            continue;
                        }

                        if (amount_to_repay < step_size) {
                            continue;
                        }

                        const auto &[base_asset, quote_asset] =
                            CCXTUtils::infer_base_quote_assets(
                                inventory.second.quote_conversion_symbol);

                        const std::string &key =
                            Core::Utils::get_symbol_exchange_market_account_key(
                                base_asset, account.exchange_name,
                                account.market_type, account_id);

                        const std::string &amount =
                            Core::Engine::getInstance().round_quantity_str(
                                amount_to_repay, symbol, account.exchange_name,
                                account.market_type);

                        if (CommonUtils::contains_only_zeros_and_dots(amount)) {
                            continue;
                        }

                        if (processed_keys.find(key) != processed_keys.end()) {
                            continue;
                        }

                        Core::Engine::getInstance().create_system_log(
                            base_asset, Core::LOG_LEVEL_INFO,
                            "Repaying loan for: " + key,
                            Core::TRADING_LOG_EVENT_TYPE_LOAN_REPAYED,
                            nlohmann::json::object(), -1, account.exchange_name,
                            account.market_type);

                        const bool res = CcxtProxy::instance().repay_loan(
                            account.exchange_name, account.market_type,
                            account_id, base_asset, amount);

                        nlohmann::json metadata = nlohmann::json::object();
                        metadata["account_id"] = account_id;
                        metadata["symbol"] = symbol;
                        metadata["exchange_name"] = account.exchange_name;
                        metadata["market_type"] = account.market_type;
                        metadata["amount"] = amount;

                        if (res) {
                            Core::Engine::getInstance().create_system_log(
                                base_asset, Core::LOG_LEVEL_INFO,
                                "Repaid loan for: " + key,
                                Core::TRADING_LOG_EVENT_TYPE_LOAN_REPAY_SUCCESS,
                                metadata, -1, account.exchange_name,
                                account.market_type);
                        } else {
                            Core::Engine::getInstance().create_system_log(
                                base_asset, Core::LOG_LEVEL_ERROR,
                                "Failed to repay loan for: " + key,
                                Core::
                                    TRADING_LOG_EVENT_TYPE_LOAN_REPAYED_FAILED,
                                metadata, -1, account.exchange_name,
                                account.market_type);
                        }

                        processed_keys.insert(key);
                        std::this_thread::sleep_for(std::chrono::seconds(1));
                    }
                }
            }
        }
    }
}

void Live::StateUpdater::init_borrowable_inventories_sqlite_cache() {
    this->borrowable_inventories_sqlite_cache =
        std::make_unique<SqliteCache<long long>>(
            CommonUtils::CachePathUtils::get_sqlite_cache_path(
                "borrowable_inventories.db"));

    for (auto &[account_id, account] : accounts) {
        if (account.exchange_name != "gateio" ||
            account.market_type != "spot") {
            continue;
        }

        const auto &active_symbols = account.get_active_symbols();
        std::unordered_set<std::string> currencies;

        for (const auto &symbol : active_symbols) {
            const auto &[base_asset, quote_asset] =
                CCXTUtils::infer_base_quote_assets(symbol);
            currencies.insert(base_asset);
        }

        for (const auto &currency : currencies) {
            const auto &key =
                Core::Utils::get_symbol_exchange_market_account_key(
                    currency, account.exchange_name, account.market_type,
                    account_id);

            if (!this->borrowable_inventories_sqlite_cache->get(key)
                     .has_value()) {
                this->borrowable_inventories_sqlite_cache->insert(key, 0);
            }
        }
    }
}

void Live::StateUpdater::poll_borrowable_inventories() {
    if (!this->borrowable_inventories_sqlite_cache) {
        return;
    }

    const auto borrowable_inventories =
        this->borrowable_inventories_sqlite_cache->query(
            "SELECT key, value FROM cache ORDER BY value ASC");

    std::unordered_map<std::string,
                       std::vector<std::pair<std::string, long long>>>
        currencies_by_account;
    const int max_items_per_account = 10;

    for (const auto &[key, value] : borrowable_inventories) {
        const auto &full_key_parts = Core::Utils::get_full_key_parts(key);
        if (!full_key_parts.exchange_name.has_value() ||
            !full_key_parts.market_type.has_value() ||
            !full_key_parts.account_id.has_value() ||
            !full_key_parts.symbol.has_value()) {
            continue;
        }

        const auto &exchange_name = full_key_parts.exchange_name.value();
        const auto &market_type = full_key_parts.market_type.value();
        const auto &account_id = full_key_parts.account_id.value();

        if (exchange_name != "gateio" || market_type != "spot") {
            continue;
        }

        const auto &account_key = Core::Utils::get_exchange_market_account_key(
            exchange_name, market_type, account_id);

        if (currencies_by_account[account_key].size() >=
            max_items_per_account) {
            continue;
        }
        currencies_by_account[account_key].push_back({key, value});
    }

    auto now = std::chrono::steady_clock::now();

    for (const auto &[account_key, currencies] : currencies_by_account) {
        auto it = last_borrowable_inventories_poll_time_by_account_id.find(
            account_key);
        if (it != last_borrowable_inventories_poll_time_by_account_id.end()) {
            if (now - it->second < std::chrono::seconds(5)) {
                continue;
            }
        }
        last_borrowable_inventories_poll_time_by_account_id[account_key] = now;

        const auto &parts =
            Core::Utils::get_full_key_parts(currencies[0].first);
        const auto &exchange_name = parts.exchange_name.value();
        const auto &market_type = parts.market_type.value();
        const auto &account_id = parts.account_id.value();

        std::vector<std::string> batch_currencies;
        std::vector<std::string> batch_keys;
        for (const auto &[currency_key, timestamp] : currencies) {
            const auto &currency_parts =
                Core::Utils::get_full_key_parts(currency_key);
            batch_currencies.push_back(currency_parts.symbol.value());
            batch_keys.push_back(currency_key);
        }

        const auto &borrowable_infos =
            CcxtProxy::instance().fetch_borrowable_inventories(
                exchange_name, market_type, account_id, batch_currencies);

        if (!borrowable_infos.empty()) {
            std::unordered_map<std::string, double> updates;
            for (const auto &info : borrowable_infos) {
                const auto &key =
                    Core::Utils::get_symbol_exchange_market_account_key(
                        info.asset, info.exchange_name, info.market_type,
                        account_id);
                updates[key] = info.available_for_borrow;
            }
            Core::Engine::getInstance().update_borrowable_inventories(updates);

            for (const auto &key : batch_keys) {
                this->borrowable_inventories_sqlite_cache->insert(
                    key, DatetimeUtils::get_current_time_ms());
            }
        }
    }
}

void Live::StateUpdater::poll_ensure_minimum_gt_balance() {
    static std::chrono::steady_clock::time_point last_check_time =
        std::chrono::steady_clock::now() - std::chrono::hours(1);

    if (std::chrono::steady_clock::now() - last_check_time <
        std::chrono::minutes(2)) {
        return;
    }
    last_check_time = std::chrono::steady_clock::now();

    const double MIN_GT_BALANCE_USD = 20.0;
    const std::string GT_SYMBOL = "GTUSDT";

    for (auto &[account_id, account] : this->accounts) {
        if (account.exchange_name != "gateio" ||
            account.market_type != "spot") {
            continue;
        }

        const auto &gt_price =
            Core::Engine::getInstance().get_price(GT_SYMBOL, "gateio", "spot");

        if (!gt_price.has_value() || gt_price.value() <= 0) {
            continue;
        }

        const auto &full_account_state =
            Core::Engine::getInstance().get_full_account_state(account_id);

        double gt_balance = 0.0;
        for (const auto &[key, inventory] : full_account_state.inventories) {
            if (inventory.asset == "GT") {
                gt_balance = inventory.total.toDouble();
                break;
            }
        }

        double gt_balance_usd = gt_balance * gt_price.value();

        if (gt_balance_usd >= MIN_GT_BALANCE_USD) {
            continue;
        }

        double amount_to_buy_usd = MIN_GT_BALANCE_USD - gt_balance_usd + 5.0;
        double amount_to_buy_gt = amount_to_buy_usd / gt_price.value();

        Core::Engine::getInstance().create_system_log(
            GT_SYMBOL, Core::LOG_LEVEL_INFO,
            "GT balance below minimum, buying GT. Current: " +
                std::to_string(gt_balance_usd) + " USD",
            Core::TRADING_LOG_EVENT_TYPE_CRYPTO_MARGIN_FUNDING_TOPUP_TRIGGERED,
            nlohmann::json::object(), -1, "gateio", "spot");

        Core::CreateOrderParams params{
            .symbol = GT_SYMBOL,
            .type = Core::OrderType::LIMIT,
            .side = Core::OrderSide::BUY,
            .price = gt_price.value() * 1.01,
            .prediction = std::nullopt,
            .quantity = amount_to_buy_gt,
            .exchange_name = "gateio",
            .market_type = "spot",
            .strategy_id = -1,
            .expires_on_ms = DatetimeUtils::get_current_time_ms() + 60000 * 60,
            .sizing_coeff_delta = 0,
            .trade_id = std::nullopt,
            .reduce_only = false,
            .timestamp = DatetimeUtils::get_current_time_ms(),
            .quote_asset = "USDT",
            .base_asset = "GT",
            .allowed_to_bypass_risk_engine = true,
            .must_pass_min_notional_filter = true,
            .is_trade_close_order = false,
            .kv_cache = {},
            .hedge_for_trade_id = std::nullopt,
            .position_side = Core::PositionSide::LONG_SIDE};

        params.account_id = account_id;
        params.type_internal =
            Core::OrderTypeInternal::NONTRACKED_INVENTORY_CLOSE;

        auto order = Core::Order::create(
            params.symbol, params.type, params.side, params.price,
            params.prediction, params.quantity, params.exchange_name,
            params.market_type, params.strategy_id, params.expires_on_ms,
            params.sizing_coeff_delta, params.trade_id, params.reduce_only,
            params.timestamp, params.quote_asset, params.base_asset,
            params.allowed_to_bypass_risk_engine, params.is_trade_close_order,
            params.type_internal, params.is_alt_market_routed,
            params.route_order_to_alt_symbol, params.routed_order_base_asset,
            params.routed_order_quote_asset, params.margin_auto_borrow,
            params.margin_auto_repay, params.derivatives_settle_currency,
            params.kv_cache, params.hedge_for_trade_id,
            params.account_id.value(), params.position_side);

        Core::Engine::getInstance().round_quantity_and_price(order);

        if (order.quantity.toDouble() * order.price.toDouble() < 2) {
            continue;
        }

        CcxtProxy::instance().create_order(order);
        Core::Engine::getInstance().add_tracked_misc_order(order);

        Core::Engine::getInstance().create_system_log(
            GT_SYMBOL, Core::LOG_LEVEL_INFO,
            "Created GT buy order for " +
                std::to_string(order.quantity.toDouble()) + " GT",
            Core::TRADING_LOG_EVENT_TYPE_ORDER_CREATED, order.to_json(), -1,
            "gateio", "spot");
    }
}
