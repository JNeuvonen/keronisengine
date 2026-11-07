#include "market_making/discretionary_accounts.h"
#include "market_making/dtypes/risk_engine.h"

#include "datetime_utils.h"
#include "market_making/dtypes/order.h"
#include "market_making/dtypes/trade.h"
#include "market_making/engine.h"

void Core::RiskEngine::add_trade(const Core::Trade& trade) {
    if (trade.hedge_for_trade_id.has_value()) {
        this->remove_trade(trade.hedge_for_trade_id.value(), true);
        return;
    }
    std::lock_guard<std::mutex> lock(this->mutex);

    this->trades.push_back(trade);
    this->trade_states[trade.id] = {
        trade.open_price, trade.open_quantity - trade.closed_quantity};

    this->trades_by_account_id[trade.account_id].push_back(trade);
    this->trade_states_by_account_id[trade.account_id][trade.id] = {
        trade.open_price, trade.open_quantity - trade.closed_quantity};

    if (trade.type == Core::TradeType::LONG) {
        this->trade_long_exposure +=
            (trade.open_quantity - trade.closed_quantity) * trade.open_price;
        this->trade_long_exposure_per_symbol[trade.symbol] +=
            (trade.open_quantity - trade.closed_quantity) * trade.open_price;

        this->trade_long_exposure_by_account_id[trade.account_id] +=
            (trade.open_quantity - trade.closed_quantity) * trade.open_price;
        this->trade_long_exposure_per_symbol_by_account_id[trade.account_id]
                                                          [trade.symbol] +=
            (trade.open_quantity - trade.closed_quantity) * trade.open_price;
    } else {
        this->trade_short_exposure +=
            (trade.open_quantity - trade.closed_quantity) * trade.open_price;
        this->trade_short_exposure_per_symbol[trade.symbol] +=
            (trade.open_quantity - trade.closed_quantity) * trade.open_price;

        this->trade_short_exposure_by_account_id[trade.account_id] +=
            (trade.open_quantity - trade.closed_quantity) * trade.open_price;
        this->trade_short_exposure_per_symbol_by_account_id[trade.account_id]
                                                           [trade.symbol] +=
            (trade.open_quantity - trade.closed_quantity) * trade.open_price;
    }
}

void Core::RiskEngine::update_exposures_on_open_trades() {
    {
        std::lock_guard<std::mutex> lock(this->mutex);
        if (this->last_updated_open_trades_tp + std::chrono::seconds(30) >
            std::chrono::system_clock::now()) {
            return;
        }
    }

    const auto current_time_ms = DatetimeUtils::get_current_time_ms();
    const auto seconds_since_minute = (current_time_ms / 1000) % 60;
    if (seconds_since_minute < 5) {
        return;
    }

    // First, get all the symbols we need prices for
    std::set<std::string> symbols_to_fetch;
    {
        std::lock_guard<std::mutex> lock(this->mutex);
        for (const auto& trade : this->trades) {
            symbols_to_fetch.insert(trade.symbol);
        }
    }

    // Fetch all prices outside of the mutex to avoid nested locks
    std::unordered_map<std::string, double> symbol_prices;
    for (const auto& symbol : symbols_to_fetch) {
        const auto price_opt = Core::Engine::getInstance().get_price(
            symbol, "binance", "swap");  // Assuming default exchange/market
        if (price_opt.has_value()) {
            symbol_prices[symbol] = price_opt.value();
        }
    }

    // Now acquire the mutex and update exposures using the pre-fetched prices
    std::lock_guard<std::mutex> lock(this->mutex);
    this->last_updated_open_trades_tp = std::chrono::system_clock::now();

    for (const auto& trade : this->trades) {
        const auto price_it = symbol_prices.find(trade.symbol);
        double price = trade.open_price.toDouble();  // fallback to open price
        if (price_it != symbol_prices.end()) {
            price = price_it->second;
        }
        this->update_exposure_on_trade(trade, price);
    }
}

void Core::RiskEngine::add_trades(const std::vector<Core::Trade>& trades) {
    std::lock_guard<std::mutex> lock(this->mutex);
    this->trades.insert(this->trades.end(), trades.begin(), trades.end());

    for (const auto& trade : trades) {
        this->trade_states[trade.id] = {
            trade.open_price, trade.open_quantity - trade.closed_quantity};

        this->trades_by_account_id[trade.account_id].push_back(trade);
        this->trade_states_by_account_id[trade.account_id][trade.id] = {
            trade.open_price, trade.open_quantity - trade.closed_quantity};

        if (trade.type == Core::TradeType::LONG) {
            this->trade_long_exposure +=
                (trade.open_quantity - trade.closed_quantity) *
                trade.open_price;
            this->trade_long_exposure_per_symbol[trade.symbol] +=
                (trade.open_quantity - trade.closed_quantity) *
                trade.open_price;

            this->trade_long_exposure_by_account_id[trade.account_id] +=
                (trade.open_quantity - trade.closed_quantity) *
                trade.open_price;
            this->trade_long_exposure_per_symbol_by_account_id[trade.account_id]
                                                              [trade.symbol] +=
                (trade.open_quantity - trade.closed_quantity) *
                trade.open_price;
        } else {
            this->trade_short_exposure +=
                (trade.open_quantity - trade.closed_quantity) *
                trade.open_price;
            this->trade_short_exposure_per_symbol[trade.symbol] +=
                (trade.open_quantity - trade.closed_quantity) *
                trade.open_price;

            this->trade_short_exposure_by_account_id[trade.account_id] +=
                (trade.open_quantity - trade.closed_quantity) *
                trade.open_price;
            this->trade_short_exposure_per_symbol_by_account_id
                [trade.account_id][trade.symbol] +=
                (trade.open_quantity - trade.closed_quantity) *
                trade.open_price;
        }
    }

    for (const auto& trade : trades) {
        if (trade.hedge_for_trade_id.has_value()) {
            this->remove_trade(trade.hedge_for_trade_id.value(), false);
        }
        for (const auto& trade_2 : this->trades) {
            if (trade_2.hedge_for_trade_id.has_value() &&
                trade_2.hedge_for_trade_id.value() == trade.id) {
                this->remove_trade(trade_2.id, false);
            }
        }
    }
}

void Core::RiskEngine::add_or_rm_exposures_from_order(const Core::Order& order,
                                                      bool is_reduce) {
    const auto& order_state = this->order_states.find(order.id);
    if (order_state == this->order_states.end()) {
        return;
    }

    if (order.side == Core::OrderSide::BUY) {
        if (is_reduce) {
            this->book_buy_exposure -=
                order_state->second.quantity * order_state->second.price;
            this->book_buy_exposure_per_symbol[order.symbol] -=
                order_state->second.quantity * order_state->second.price;

            this->book_buy_exposure_by_account_id[order.account_id] -=
                order_state->second.quantity * order_state->second.price;
            this->book_buy_exposure_per_symbol_by_account_id[order.account_id]
                                                            [order.symbol] -=
                order_state->second.quantity * order_state->second.price;
        } else {
            this->book_buy_exposure +=
                order_state->second.quantity * order_state->second.price;
            this->book_buy_exposure_per_symbol[order.symbol] +=
                order_state->second.quantity * order_state->second.price;

            this->book_buy_exposure_by_account_id[order.account_id] +=
                order_state->second.quantity * order_state->second.price;
            this->book_buy_exposure_per_symbol_by_account_id[order.account_id]
                                                            [order.symbol] +=
                order_state->second.quantity * order_state->second.price;
        }
    } else {
        if (is_reduce) {
            this->book_sell_exposure -=
                order_state->second.quantity * order_state->second.price;
            this->book_sell_exposure_per_symbol[order.symbol] -=
                order_state->second.quantity * order_state->second.price;

            this->book_sell_exposure_by_account_id[order.account_id] -=
                order_state->second.quantity * order_state->second.price;
            this->book_sell_exposure_per_symbol_by_account_id[order.account_id]
                                                             [order.symbol] -=
                order_state->second.quantity * order_state->second.price;
        } else {
            this->book_sell_exposure +=
                order_state->second.quantity * order_state->second.price;
            this->book_sell_exposure_per_symbol[order.symbol] +=
                order_state->second.quantity * order_state->second.price;

            this->book_sell_exposure_by_account_id[order.account_id] +=
                order_state->second.quantity * order_state->second.price;
            this->book_sell_exposure_per_symbol_by_account_id[order.account_id]
                                                             [order.symbol] +=
                order_state->second.quantity * order_state->second.price;
        }
    }
}

void Core::RiskEngine::update_exposure_on_order(const Core::Order& order) {
    const auto& order_state = this->order_states.find(order.id);
    if (order_state != this->order_states.end()) {
        if (order.side == Core::OrderSide::BUY) {
            this->book_buy_exposure -=
                order_state->second.quantity * order_state->second.price;
            this->book_buy_exposure_per_symbol[order.symbol] -=
                order_state->second.quantity * order_state->second.price;

            this->book_buy_exposure_by_account_id[order.account_id] -=
                order_state->second.quantity * order_state->second.price;
            this->book_buy_exposure_per_symbol_by_account_id[order.account_id]
                                                            [order.symbol] -=
                order_state->second.quantity * order_state->second.price;
        } else {
            this->book_sell_exposure -=
                order_state->second.quantity * order_state->second.price;
            this->book_sell_exposure_per_symbol[order.symbol] -=
                order_state->second.quantity * order_state->second.price;

            this->book_sell_exposure_by_account_id[order.account_id] -=
                order_state->second.quantity * order_state->second.price;
            this->book_sell_exposure_per_symbol_by_account_id[order.account_id]
                                                             [order.symbol] -=
                order_state->second.quantity * order_state->second.price;
        }

        order_state->second.price = order.price;
        order_state->second.quantity = order.quantity - order.filled_quantity;
        this->order_states_by_account_id[order.account_id][order.id] = {
            order.price, order.quantity - order.filled_quantity};

        if (order.side == Core::OrderSide::BUY) {
            this->book_buy_exposure +=
                order_state->second.quantity * order_state->second.price;
            this->book_buy_exposure_per_symbol[order.symbol] +=
                order_state->second.quantity * order_state->second.price;

            this->book_buy_exposure_by_account_id[order.account_id] +=
                order_state->second.quantity * order_state->second.price;
            this->book_buy_exposure_per_symbol_by_account_id[order.account_id]
                                                            [order.symbol] +=
                order_state->second.quantity * order_state->second.price;
        } else {
            this->book_sell_exposure +=
                order_state->second.quantity * order_state->second.price;
            this->book_sell_exposure_per_symbol[order.symbol] +=
                order_state->second.quantity * order_state->second.price;

            this->book_sell_exposure_by_account_id[order.account_id] +=
                order_state->second.quantity * order_state->second.price;
            this->book_sell_exposure_per_symbol_by_account_id[order.account_id]
                                                             [order.symbol] +=
                order_state->second.quantity * order_state->second.price;
        }
    }
}

void Core::RiskEngine::update_exposure_on_trade(const Core::Trade& trade,
                                                double price) {
    const auto& trade_state = this->trade_states.find(trade.id);
    if (trade_state != this->trade_states.end()) {
        if (trade.type == Core::TradeType::LONG) {
            this->trade_long_exposure -=
                trade_state->second.quantity * trade_state->second.price;
            this->trade_long_exposure_per_symbol[trade.symbol] -=
                trade_state->second.quantity * trade_state->second.price;

            this->trade_long_exposure_by_account_id[trade.account_id] -=
                trade_state->second.quantity * trade_state->second.price;
            this->trade_long_exposure_per_symbol_by_account_id[trade.account_id]
                                                              [trade.symbol] -=
                trade_state->second.quantity * trade_state->second.price;
        } else {
            this->trade_short_exposure -=
                trade_state->second.quantity * trade_state->second.price;
            this->trade_short_exposure_per_symbol[trade.symbol] -=
                trade_state->second.quantity * trade_state->second.price;

            this->trade_short_exposure_by_account_id[trade.account_id] -=
                trade_state->second.quantity * trade_state->second.price;
            this->trade_short_exposure_per_symbol_by_account_id
                [trade.account_id][trade.symbol] -=
                trade_state->second.quantity * trade_state->second.price;
        }

        trade_state->second.price = price;
        trade_state->second.quantity =
            trade.open_quantity - trade.closed_quantity;

        this->trade_states_by_account_id[trade.account_id][trade.id] = {
            price, trade.open_quantity - trade.closed_quantity};

        if (trade.type == Core::TradeType::LONG) {
            this->trade_long_exposure +=
                trade_state->second.quantity * trade_state->second.price;
            this->trade_long_exposure_per_symbol[trade.symbol] +=
                trade_state->second.quantity * trade_state->second.price;

            this->trade_long_exposure_by_account_id[trade.account_id] +=
                trade_state->second.quantity * trade_state->second.price;
            this->trade_long_exposure_per_symbol_by_account_id[trade.account_id]
                                                              [trade.symbol] +=
                trade_state->second.quantity * trade_state->second.price;
        } else {
            this->trade_short_exposure +=
                trade_state->second.quantity * trade_state->second.price;
            this->trade_short_exposure_per_symbol[trade.symbol] +=
                trade_state->second.quantity * trade_state->second.price;

            this->trade_short_exposure_by_account_id[trade.account_id] +=
                trade_state->second.quantity * trade_state->second.price;
            this->trade_short_exposure_per_symbol_by_account_id
                [trade.account_id][trade.symbol] +=
                trade_state->second.quantity * trade_state->second.price;
        }
    }
}

void Core::RiskEngine::add_orders(const std::vector<Core::Order>& orders) {
    std::lock_guard<std::mutex> lock(this->mutex);
    this->orders.insert(this->orders.end(), orders.begin(), orders.end());
    for (const auto& order : orders) {
        this->order_states[order.id] = {order.price, order.quantity};
        this->orders_by_account_id[order.account_id].push_back(order);
        this->order_states_by_account_id[order.account_id][order.id] = {
            order.price, order.quantity};
        this->add_or_rm_exposures_from_order(order, false);
    }
}

void Core::RiskEngine::remove_order(unsigned long long order_id) {
    std::lock_guard<std::mutex> lock(this->mutex);
    auto order_it = std::find_if(
        this->orders.begin(), this->orders.end(),
        [&](const Core::Order& order) { return order.id == order_id; });

    if (order_it == this->orders.end()) {
        return;
    }

    const auto account_id = order_it->account_id;

    this->add_or_rm_exposures_from_order(*order_it, true);
    this->orders.erase(order_it);
    this->order_states.erase(order_id);

    const auto& orders_by_account_id =
        this->orders_by_account_id.find(account_id);
    if (orders_by_account_id != this->orders_by_account_id.end()) {
        auto it2 = std::find_if(orders_by_account_id->second.begin(),
                                orders_by_account_id->second.end(),
                                [&](const Core::Order& existing_order) {
                                    return existing_order.id == order_id;
                                });
        if (it2 != orders_by_account_id->second.end()) {
            this->orders_by_account_id[account_id].erase(it2);
        }

        auto order_state_it = this->order_states_by_account_id.find(account_id);
        if (order_state_it != this->order_states_by_account_id.end()) {
            auto order_state_it2 = order_state_it->second.find(order_id);
            if (order_state_it2 != order_state_it->second.end()) {
                order_state_it->second.erase(order_state_it2);
            }
        }
    }
}

void Core::RiskEngine::remove_trade(unsigned long long trade_id,
                                    bool acquire_lock) {
    auto remove_impl = [this, trade_id]() {
        auto trade_it = std::find_if(this->trades.begin(), this->trades.end(),
                                     [trade_id](const Core::Trade& trade) {
                                         return trade.id == trade_id;
                                     });

        if (trade_it == this->trades.end()) {
            return;
        }

        const auto account_id = trade_it->account_id;
        const auto symbol = trade_it->symbol;
        const auto type = trade_it->type;

        mpfr::mpreal quantity = trade_it->open_quantity;
        mpfr::mpreal price = trade_it->open_price;

        const auto& trade_state = this->trade_states.find(trade_id);
        if (trade_state != this->trade_states.end()) {
            quantity = trade_state->second.quantity;
            price = trade_state->second.price;
        }

        if (type == Core::TradeType::LONG) {
            this->trade_long_exposure -= quantity * price;
            this->trade_long_exposure_per_symbol[symbol] -= quantity * price;
            this->trade_long_exposure_by_account_id[account_id] -=
                quantity * price;
            this->trade_long_exposure_per_symbol_by_account_id[account_id]
                                                              [symbol] -=
                quantity * price;
        } else {
            this->trade_short_exposure -= quantity * price;
            this->trade_short_exposure_per_symbol[symbol] -= quantity * price;
            this->trade_short_exposure_by_account_id[account_id] -=
                quantity * price;
            this->trade_short_exposure_per_symbol_by_account_id[account_id]
                                                               [symbol] -=
                quantity * price;
        }

        this->trades.erase(trade_it);
        this->trade_states.erase(trade_id);

        const auto& trades_by_account_id =
            this->trades_by_account_id.find(account_id);
        if (trades_by_account_id != this->trades_by_account_id.end()) {
            auto it2 =
                std::find_if(trades_by_account_id->second.begin(),
                             trades_by_account_id->second.end(),
                             [trade_id](const Core::Trade& existing_trade) {
                                 return existing_trade.id == trade_id;
                             });

            if (it2 != trades_by_account_id->second.end()) {
                this->trades_by_account_id[account_id].erase(it2);
            }

            auto trade_state_it =
                this->trade_states_by_account_id.find(account_id);
            if (trade_state_it != this->trade_states_by_account_id.end()) {
                auto trade_state_it2 = trade_state_it->second.find(trade_id);
                if (trade_state_it2 != trade_state_it->second.end()) {
                    trade_state_it->second.erase(trade_state_it2);
                }
            }
        }
    };

    if (acquire_lock) {
        std::lock_guard<std::mutex> lock(this->mutex);
        remove_impl();
    } else {
        remove_impl();
    }
}

void Core::RiskEngine::update_order(const Core::Order& order) {
    std::lock_guard<std::mutex> lock(this->mutex);
    auto it = std::find_if(this->orders.begin(), this->orders.end(),
                           [&](const Core::Order& existing_order) {
                               return existing_order.id == order.id;
                           });

    if (it != this->orders.end()) {
        *it = order;
        const auto& orders_by_account_id =
            this->orders_by_account_id.find(order.account_id);
        if (orders_by_account_id != this->orders_by_account_id.end()) {
            auto it2 = std::find_if(orders_by_account_id->second.begin(),
                                    orders_by_account_id->second.end(),
                                    [&](const Core::Order& existing_order) {
                                        return existing_order.id == order.id;
                                    });
            if (it2 != orders_by_account_id->second.end()) {
                *it2 = order;
            }
        }

        this->update_exposure_on_order(order);
    }
}

void Core::RiskEngine::update_trade(const Core::Trade& trade) {
    // First, get the price before acquiring any locks
    const auto price_opt = Core::Engine::getInstance().get_price(
        trade.symbol, trade.exchange_name, trade.market_type);
    mpfr::mpreal price = 0;

    if (!price_opt.has_value()) {
        price = trade.open_price;
    } else {
        price = price_opt.value();
    }

    // Now acquire the mutex and update the trade, to prevent nested locks
    std::lock_guard<std::mutex> lock(this->mutex);
    auto it = std::find_if(this->trades.begin(), this->trades.end(),
                           [&](const Core::Trade& existing_trade) {
                               return existing_trade.id == trade.id;
                           });

    if (it != this->trades.end()) {
        *it = trade;

        const auto& trades_by_account_id =
            this->trades_by_account_id.find(trade.account_id);
        if (trades_by_account_id != this->trades_by_account_id.end()) {
            auto it2 = std::find_if(trades_by_account_id->second.begin(),
                                    trades_by_account_id->second.end(),
                                    [&](const Core::Trade& existing_trade) {
                                        return existing_trade.id == trade.id;
                                    });
            if (it2 != trades_by_account_id->second.end()) {
                *it2 = trade;
            }
        }

        // Call update_exposure_on_trade with the pre-fetched price
        this->update_exposure_on_trade(trade, price.toDouble());
    }
}

void Core::RiskEngine::remove_orders_and_trades_by_symbol(
    const std::string& symbol) {
    std::lock_guard<std::mutex> lock(this->mutex);

    // Pass 1: subtract exposures and clean up state maps for matching orders
    for (const auto& order : this->orders) {
        if (order.symbol != symbol) continue;

        this->add_or_rm_exposures_from_order(order, true);
        this->order_states.erase(order.id);

        auto acct_state_it =
            this->order_states_by_account_id.find(order.account_id);
        if (acct_state_it != this->order_states_by_account_id.end()) {
            acct_state_it->second.erase(order.id);
        }
    }

    // Pass 2: batch-remove matching orders from vectors (single O(n) sweep)
    std::erase_if(this->orders, [&symbol](const Core::Order& o) {
        return o.symbol == symbol;
    });

    for (auto& [account_id, acct_orders] : this->orders_by_account_id) {
        std::erase_if(acct_orders, [&symbol](const Core::Order& o) {
            return o.symbol == symbol;
        });
    }

    // Pass 1: subtract exposures and clean up state maps for matching trades
    for (const auto& trade : this->trades) {
        if (trade.symbol != symbol) continue;

        mpfr::mpreal quantity = trade.open_quantity;
        mpfr::mpreal price = trade.open_price;

        const auto& trade_state = this->trade_states.find(trade.id);
        if (trade_state != this->trade_states.end()) {
            quantity = trade_state->second.quantity;
            price = trade_state->second.price;
        }

        if (trade.type == Core::TradeType::LONG) {
            this->trade_long_exposure -= quantity * price;
            this->trade_long_exposure_per_symbol[symbol] -= quantity * price;
            this->trade_long_exposure_by_account_id[trade.account_id] -=
                quantity * price;
            this->trade_long_exposure_per_symbol_by_account_id[trade.account_id]
                                                              [symbol] -=
                quantity * price;
        } else {
            this->trade_short_exposure -= quantity * price;
            this->trade_short_exposure_per_symbol[symbol] -= quantity * price;
            this->trade_short_exposure_by_account_id[trade.account_id] -=
                quantity * price;
            this->trade_short_exposure_per_symbol_by_account_id
                [trade.account_id][symbol] -= quantity * price;
        }

        this->trade_states.erase(trade.id);

        auto acct_state_it =
            this->trade_states_by_account_id.find(trade.account_id);
        if (acct_state_it != this->trade_states_by_account_id.end()) {
            acct_state_it->second.erase(trade.id);
        }
    }

    // Pass 2: batch-remove matching trades from vectors
    std::erase_if(this->trades, [&symbol](const Core::Trade& t) {
        return t.symbol == symbol;
    });

    for (auto& [account_id, acct_trades] : this->trades_by_account_id) {
        std::erase_if(acct_trades, [&symbol](const Core::Trade& t) {
            return t.symbol == symbol;
        });
    }
}

void Core::RiskEngine::clear() {
    std::lock_guard<std::mutex> lock(this->mutex);
    this->orders.clear();
    this->trades.clear();
    this->order_states.clear();
    this->trade_states.clear();
    this->book_buy_exposure = 0;
    this->book_sell_exposure = 0;
    this->book_buy_exposure_per_symbol.clear();
    this->book_sell_exposure_per_symbol.clear();
    this->trade_long_exposure = 0;
    this->trade_short_exposure = 0;
    this->trade_long_exposure_per_symbol.clear();
    this->trade_short_exposure_per_symbol.clear();
    this->orders_by_account_id.clear();
    this->trades_by_account_id.clear();
    this->order_states_by_account_id.clear();
    this->trade_states_by_account_id.clear();
    this->book_buy_exposure_by_account_id.clear();
    this->book_sell_exposure_by_account_id.clear();
    this->trade_long_exposure_by_account_id.clear();
    this->trade_short_exposure_by_account_id.clear();
    this->book_buy_exposure_per_symbol_by_account_id.clear();
    this->book_sell_exposure_per_symbol_by_account_id.clear();
    this->trade_long_exposure_per_symbol_by_account_id.clear();
    this->trade_short_exposure_per_symbol_by_account_id.clear();
}

const std::vector<Core::Order> Core::RiskEngine::get_orders() const {
    std::lock_guard<std::mutex> lock(this->mutex);
    return this->orders;
}

const std::vector<Core::Trade> Core::RiskEngine::get_trades() const {
    std::lock_guard<std::mutex> lock(this->mutex);
    return this->trades;
}

bool Core::RiskEngine::is_order_allowed_to_pass(Core::Engine& engine,
                                                const Core::Order& order) {
    return true;
}

mpfr::mpreal Core::RiskEngine::get_long_exposure(bool require_lock) const {
    if (require_lock) {
        std::lock_guard<std::mutex> lock(this->mutex);
        return this->trade_long_exposure + this->book_buy_exposure;
    }
    return this->trade_long_exposure + this->book_buy_exposure;
}

mpfr::mpreal Core::RiskEngine::get_short_exposure(bool require_lock) const {
    if (require_lock) {
        std::lock_guard<std::mutex> lock(this->mutex);
        return this->trade_short_exposure + this->book_sell_exposure;
    }
    return this->trade_short_exposure + this->book_sell_exposure;
}

mpfr::mpreal Core::RiskEngine::get_long_exposure_per_symbol(
    const std::string& symbol, bool require_lock) const {
    if (require_lock) {
        std::lock_guard<std::mutex> lock(this->mutex);

        mpfr::mpreal long_exposure_per_symbol = 0;
        mpfr::mpreal buy_exposure_per_symbol = 0;

        auto long_exposure_per_symbol_it =
            this->trade_long_exposure_per_symbol.find(symbol);
        auto buy_exposure_per_symbol_it =
            this->book_buy_exposure_per_symbol.find(symbol);

        if (long_exposure_per_symbol_it !=
            this->trade_long_exposure_per_symbol.end()) {
            long_exposure_per_symbol = long_exposure_per_symbol_it->second;
        }

        if (buy_exposure_per_symbol_it !=
            this->book_buy_exposure_per_symbol.end()) {
            buy_exposure_per_symbol = buy_exposure_per_symbol_it->second;
        }

        return long_exposure_per_symbol + buy_exposure_per_symbol;
    }

    mpfr::mpreal long_exposure_per_symbol = 0;
    mpfr::mpreal buy_exposure_per_symbol = 0;

    auto long_exposure_per_symbol_it =
        this->trade_long_exposure_per_symbol.find(symbol);
    auto buy_exposure_per_symbol_it =
        this->book_buy_exposure_per_symbol.find(symbol);

    if (long_exposure_per_symbol_it !=
        this->trade_long_exposure_per_symbol.end()) {
        long_exposure_per_symbol = long_exposure_per_symbol_it->second;
    }

    if (buy_exposure_per_symbol_it !=
        this->book_buy_exposure_per_symbol.end()) {
        buy_exposure_per_symbol = buy_exposure_per_symbol_it->second;
    }

    return long_exposure_per_symbol + buy_exposure_per_symbol;
}

mpfr::mpreal Core::RiskEngine::get_short_exposure_per_symbol(
    const std::string& symbol, bool require_lock) const {
    if (require_lock) {
        std::lock_guard<std::mutex> lock(this->mutex);

        mpfr::mpreal short_exposure_per_symbol = 0;
        mpfr::mpreal sell_exposure_per_symbol = 0;

        auto short_exposure_per_symbol_it =
            this->trade_short_exposure_per_symbol.find(symbol);
        auto sell_exposure_per_symbol_it =
            this->book_sell_exposure_per_symbol.find(symbol);

        if (short_exposure_per_symbol_it !=
            this->trade_short_exposure_per_symbol.end()) {
            short_exposure_per_symbol = short_exposure_per_symbol_it->second;
        }

        if (sell_exposure_per_symbol_it !=
            this->book_sell_exposure_per_symbol.end()) {
            sell_exposure_per_symbol = sell_exposure_per_symbol_it->second;
        }

        return short_exposure_per_symbol + sell_exposure_per_symbol;
    }

    mpfr::mpreal short_exposure_per_symbol = 0;
    mpfr::mpreal sell_exposure_per_symbol = 0;

    auto short_exposure_per_symbol_it =
        this->trade_short_exposure_per_symbol.find(symbol);
    auto sell_exposure_per_symbol_it =
        this->book_sell_exposure_per_symbol.find(symbol);

    if (short_exposure_per_symbol_it !=
        this->trade_short_exposure_per_symbol.end()) {
        short_exposure_per_symbol = short_exposure_per_symbol_it->second;
    }

    if (sell_exposure_per_symbol_it !=
        this->book_sell_exposure_per_symbol.end()) {
        sell_exposure_per_symbol = sell_exposure_per_symbol_it->second;
    }

    return short_exposure_per_symbol + sell_exposure_per_symbol;
}

mpfr::mpreal Core::RiskEngine::get_new_order_exposure_ratio(
    Core::Engine& engine, const Core::Strategy& strategy,
    const Core::Order& order) {
    const auto& quote_sizing = order.quantity * order.price;
    const auto& nav = engine.get_nav();

    if (nav == 0) {
        return 0;
    }

    return quote_sizing / nav;
}

mpfr::mpreal Core::RiskEngine::get_new_order_exposure_ratio_by_account_id(
    Core::Engine& engine, const Core::Strategy& strategy,
    const Core::Order& order) {
    const auto& quote_sizing = order.quantity * order.price;
    const auto& nav = this->navs_by_account_id.find(order.account_id);

    if (nav == this->navs_by_account_id.end() || nav->second == 0) {
        return 0;
    }

    return quote_sizing / nav->second;
}

void Core::RiskEngine::apply_risk_bounds_resizing(Core::Engine& engine,
                                                  Core::Strategy& strategy,
                                                  Core::Order& order) {
    if (strategy.strategy_config.discretionary_multi_account) {
        const auto account = Core::DiscretionaryAccounts::instance().get(order.account_id);
        const auto nav = this->navs_by_account_id.find(order.account_id);
        if (!account || !account->enabled || account->paper != strategy.strategy_config.is_paper_trading ||
            account->exchange != order.exchange_name || account->market != order.market_type ||
            nav == this->navs_by_account_id.end() || nav->second <= 0 || order.price <= 0) {
            order.quantity = 0;
            return;
        }
        const bool sell = order.side == Core::OrderSide::SELL;
        const auto exposure = sell ? this->trade_short_exposure_by_account_id[order.account_id] +
            this->book_sell_exposure_by_account_id[order.account_id] :
            this->trade_long_exposure_by_account_id[order.account_id] + this->book_buy_exposure_by_account_id[order.account_id];
        const double cap = std::min(account->max_exposure, strategy.strategy_config.max_directional_exposure);
        const double available = std::max(0.0, nav->second.toDouble() * cap - exposure.toDouble());
        this->resize_to_fit_max_order_notional_quantity(std::min(available, account->max_order_notional), order);
        return;
    }
    const bool is_short_sale = order.side == Core::OrderSide::SELL;
    const auto& new_order_exposure_ratio =
        this->get_new_order_exposure_ratio(engine, strategy, order);

    const auto& nav = engine.get_nav();

    if (is_short_sale) {
        const auto& short_ratio = engine.get_short_ratio(false);
        const auto& short_ratio_by_symbol =
            engine.get_short_ratio_by_symbol(order.symbol, false);

        const auto& remaining_short_ratio =
            strategy.strategy_config.max_directional_exposure - short_ratio -
            new_order_exposure_ratio;

        if (remaining_short_ratio < 0) {
            order.quantity = 0;
        } else if (remaining_short_ratio < new_order_exposure_ratio) {
            const auto old_quantity = order.quantity;

            if (order.price == 0) {
                return;
            }

            order.quantity = (remaining_short_ratio * nav) / order.price;

            if (old_quantity != 0) {
                order.sizing_coeff_delta = order.quantity / old_quantity;
            }
        }
    } else {
        const auto& long_ratio = engine.get_long_ratio(false);
        const auto& long_ratio_by_symbol =
            engine.get_long_ratio_by_symbol(order.symbol, false);

        const auto& remaining_long_ratio =
            strategy.strategy_config.max_directional_exposure - long_ratio -
            new_order_exposure_ratio;

        if (remaining_long_ratio < 0) {
            order.quantity = 0;
        } else if (remaining_long_ratio < new_order_exposure_ratio) {
            const auto old_quantity = order.quantity;
            if (order.price == 0) {
                return;
            }
            order.quantity = (remaining_long_ratio * nav) / order.price;

            if (old_quantity != 0) {
                order.sizing_coeff_delta = order.quantity / old_quantity;
            }
        }
    }
}

void Core::RiskEngine::resize_to_fit_max_order_notional_quantity(
    double max_notional_quantity, Core::Order& order) {
    const auto& quote_sizing = order.quantity * order.price;
    if (quote_sizing > max_notional_quantity) {
        if (order.price == 0) {
            order.quantity = 0;
            return;
        }
        const auto& new_quantity = max_notional_quantity / order.price;
        if (new_quantity != 0) {
            order.sizing_coeff_delta =
                (new_quantity / order.quantity) * order.sizing_coeff_delta;
        }
        order.quantity = new_quantity;
    }
}

// this is the hot path function, the complex design is to keep everything O(1)
// in this function
std::tuple<Core::OrderConstructionResult, std::optional<Core::Order>>
Core::RiskEngine::construct_order(Core::Engine& engine,
                                  Core::Strategy& strategy,
                                  const Core::CreateOrderParams& params) {
    if (!params.account_id.has_value()) {
        return {Core::MISSING_ACCOUNT_ID_RISK_ENGINE, std::nullopt};
    }
    std::lock_guard<std::mutex> lock(this->mutex);

    auto order = Order::create(
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
        params.kv_cache, params.hedge_for_trade_id, params.account_id.value(),
        params.position_side, params.is_prediction_market_order, params.clob_id,
        params.outcome_name, params.markout_config);

    if ((!params.is_trade_close_order &&
         !params.hedge_for_trade_id.has_value()) &&
        !this->is_order_allowed_to_pass(engine, order)) {
        if (params.strategy_id == 44) {
        }
        return {Core::ORDER_NOT_ALLOWED_TO_PASS_RISK_ENGINE, std::nullopt};
    }

    if (params.maximum_notional_quantity.has_value()) {
        this->resize_to_fit_max_order_notional_quantity(
            params.maximum_notional_quantity.value(), order);
    }

    const auto quantity_before_risk_bounds = order.quantity;
    if (!params.is_trade_close_order && !params.allowed_to_bypass_risk_engine) {
        this->apply_risk_bounds_resizing(engine, strategy, order);
    }

    engine.round_quantity_and_price(order);
    if (params.must_pass_min_notional_filter &&
        !engine.passes_min_notional_filter(order)) {
        return {Core::MIN_NOTIONAL_FILTER_FAILED, std::nullopt};
    }

    // apply_risk_bounds_resizing can zero the quantity (cap hit) or shrink
    // it below any reasonable minimum. Such orders cannot be placed; we also
    // do not commit them to risk_engine state. State commit happens in
    // register_order, called from ticker.cpp::mark_order_to_be_created when
    // the order is actually dispatched.
    if (order.quantity == 0) {
        return {Core::MIN_NOTIONAL_FILTER_FAILED, std::nullopt};
    }

    return {Core::VALID, order};
}

void Core::RiskEngine::rebuild_from_state(
    const std::vector<Core::Order>& new_orders,
    const std::vector<Core::Trade>& new_trades) {
    std::lock_guard<std::mutex> lock(this->mutex);

    this->orders = new_orders;
    this->trades = new_trades;

    this->order_states.clear();
    this->orders_by_account_id.clear();
    this->order_states_by_account_id.clear();
    this->trade_states.clear();
    this->trades_by_account_id.clear();
    this->trade_states_by_account_id.clear();

    this->book_buy_exposure = 0;
    this->book_sell_exposure = 0;
    this->trade_long_exposure = 0;
    this->trade_short_exposure = 0;
    this->book_buy_exposure_per_symbol.clear();
    this->book_sell_exposure_per_symbol.clear();
    this->trade_long_exposure_per_symbol.clear();
    this->trade_short_exposure_per_symbol.clear();
    this->book_buy_exposure_by_account_id.clear();
    this->book_sell_exposure_by_account_id.clear();
    this->trade_long_exposure_by_account_id.clear();
    this->trade_short_exposure_by_account_id.clear();
    this->book_buy_exposure_per_symbol_by_account_id.clear();
    this->book_sell_exposure_per_symbol_by_account_id.clear();
    this->trade_long_exposure_per_symbol_by_account_id.clear();
    this->trade_short_exposure_per_symbol_by_account_id.clear();

    for (const auto& order : this->orders) {
        if (order.is_prediction_market_order) {
            continue;
        }
        this->order_states[order.id] = {order.price, order.quantity};
        this->orders_by_account_id[order.account_id].push_back(order);
        this->order_states_by_account_id[order.account_id][order.id] = {
            order.price, order.quantity};

        const auto notional = order.quantity * order.price;
        if (order.side == Core::OrderSide::BUY) {
            this->book_buy_exposure += notional;
            this->book_buy_exposure_per_symbol[order.symbol] += notional;
            this->book_buy_exposure_by_account_id[order.account_id] += notional;
            this->book_buy_exposure_per_symbol_by_account_id[order.account_id]
                                                            [order.symbol] +=
                notional;
        } else {
            this->book_sell_exposure += notional;
            this->book_sell_exposure_per_symbol[order.symbol] += notional;
            this->book_sell_exposure_by_account_id[order.account_id] += notional;
            this->book_sell_exposure_per_symbol_by_account_id[order.account_id]
                                                             [order.symbol] +=
                notional;
        }
    }

    for (const auto& trade : this->trades) {
        const auto remaining_qty =
            trade.open_quantity - trade.closed_quantity;
        this->trade_states[trade.id] = {trade.open_price, remaining_qty};
        this->trades_by_account_id[trade.account_id].push_back(trade);
        this->trade_states_by_account_id[trade.account_id][trade.id] = {
            trade.open_price, remaining_qty};

        const auto exposure = remaining_qty * trade.open_price;
        if (trade.type == Core::TradeType::LONG) {
            this->trade_long_exposure += exposure;
            this->trade_long_exposure_per_symbol[trade.symbol] += exposure;
            this->trade_long_exposure_by_account_id[trade.account_id] += exposure;
            this->trade_long_exposure_per_symbol_by_account_id
                [trade.account_id][trade.symbol] += exposure;
        } else {
            this->trade_short_exposure += exposure;
            this->trade_short_exposure_per_symbol[trade.symbol] += exposure;
            this->trade_short_exposure_by_account_id[trade.account_id] +=
                exposure;
            this->trade_short_exposure_per_symbol_by_account_id
                [trade.account_id][trade.symbol] += exposure;
        }
    }
}

void Core::RiskEngine::register_order(const Core::Order& order) {
    if (order.is_prediction_market_order) {
        return;
    }
    if (order.quantity == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(this->mutex);

    this->orders.push_back(order);
    this->order_states[order.id] = {order.price, order.quantity};
    this->orders_by_account_id[order.account_id].push_back(order);
    this->order_states_by_account_id[order.account_id][order.id] = {
        order.price, order.quantity};

    const auto notional = order.quantity * order.price;
    if (order.side == Core::OrderSide::BUY) {
        this->book_buy_exposure += notional;
        this->book_buy_exposure_per_symbol[order.symbol] += notional;
        this->book_buy_exposure_by_account_id[order.account_id] += notional;
        this->book_buy_exposure_per_symbol_by_account_id[order.account_id]
                                                        [order.symbol] +=
            notional;
    } else {
        this->book_sell_exposure += notional;
        this->book_sell_exposure_per_symbol[order.symbol] += notional;
        this->book_sell_exposure_by_account_id[order.account_id] += notional;
        this->book_sell_exposure_per_symbol_by_account_id[order.account_id]
                                                         [order.symbol] +=
            notional;
    }
}

std::optional<Core::Order> Core::RiskEngine::get_order_from_account_state(
    long long account_id, unsigned long long order_id) {
    std::lock_guard<std::mutex> lock(this->mutex);
    auto orders_it = this->orders_by_account_id.find(account_id);
    if (orders_it != this->orders_by_account_id.end()) {
        auto order_it = std::find_if(
            orders_it->second.begin(), orders_it->second.end(),
            [&](const Core::Order& order) { return order.id == order_id; });
        if (order_it != orders_it->second.end()) {
            return *order_it;
        }
    }
    return std::nullopt;
}

std::optional<Core::Trade> Core::RiskEngine::get_trade_from_account_state(
    long long account_id, unsigned long long trade_id) {
    std::lock_guard<std::mutex> lock(this->mutex);
    auto trades_it = this->trades_by_account_id.find(account_id);
    if (trades_it != this->trades_by_account_id.end()) {
        auto trade_it = std::find_if(
            trades_it->second.begin(), trades_it->second.end(),
            [&](const Core::Trade& trade) { return trade.id == trade_id; });
        if (trade_it != trades_it->second.end()) {
            return *trade_it;
        }
    }
    return std::nullopt;
}
std::optional<Core::ExposureState>
Core::RiskEngine::get_order_state_from_account_state(
    long long account_id, unsigned long long order_id) {
    std::lock_guard<std::mutex> lock(this->mutex);
    auto order_state_it = this->order_states_by_account_id.find(account_id);
    if (order_state_it != this->order_states_by_account_id.end()) {
        auto order_state_it2 = order_state_it->second.find(order_id);
        if (order_state_it2 != order_state_it->second.end()) {
            return order_state_it2->second;
        }
    }
    return std::nullopt;
}

std::optional<Core::ExposureState>
Core::RiskEngine::get_trade_state_from_account_state(
    long long account_id, unsigned long long trade_id) {
    std::lock_guard<std::mutex> lock(this->mutex);
    auto trade_state_it = this->trade_states_by_account_id.find(account_id);
    if (trade_state_it != this->trade_states_by_account_id.end()) {
        auto trade_state_it2 = trade_state_it->second.find(trade_id);
        if (trade_state_it2 != trade_state_it->second.end()) {
            return trade_state_it2->second;
        }
    }
    return std::nullopt;
}

void Core::RiskEngine::update_navs_by_account_id(
    const std::unordered_map<long long, mpfr::mpreal>& navs) {
    std::lock_guard<std::mutex> lock(this->mutex);
    this->navs_by_account_id = navs;
}
// Diag size-readers — lock-free; only safe from sim-event handlers.
size_t Core::RiskEngine::peek_trades_size() const { return trades.size(); }
size_t Core::RiskEngine::peek_orders_size() const { return orders.size(); }
size_t Core::RiskEngine::peek_trade_states_size() const {
    return trade_states.size();
}
size_t Core::RiskEngine::peek_order_states_size() const {
    return order_states.size();
}
