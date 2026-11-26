#include "market_making/discretionary_accounts.h"
#include "market_making/backtest_state.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <unordered_set>

#include "market_making/fold_schedule_loader.h"

#include "chrono_utils.h"
#include "common_types/log_event_types.h"
#include "datetime_utils.h"
#include "disk_logging/log.h"
#include "market_data/bar_aggregator.h"
#include "market_data/bar_close_policy.h"
#include "market_data/dataset_manager.h"
#include "market_data/ohlcv_bar_meta.h"
#include "market_data/ohlcv_dataset.h"
#include "market_data/utils.h"
#include "market_making/backtest_trade_ticker.h"
#include "market_making/dtypes/market_data_dataset.h"
#include "market_making/prediction_market_utils.h"
#include "market_making/utils.h"
#include "ohlcv_dataset_tick.h"
#include "parquet_utils.h"
#include "polymarket_trade_tick.h"
#include "sqlite_datasets.h"
#include "uuid.h"

namespace {

bool removed_affects_buy_extremum(double limit_price, double max_buy_limit) {
    if (!std::isfinite(max_buy_limit)) {
        return false;
    }
    return limit_price >=
           std::nextafter(max_buy_limit,
                          -std::numeric_limits<double>::infinity());
}

bool removed_affects_sell_extremum(double limit_price, double min_sell_limit) {
    if (!std::isfinite(min_sell_limit)) {
        return false;
    }
    return limit_price <=
           std::nextafter(min_sell_limit,
                          std::numeric_limits<double>::infinity());
}

}  // namespace

Core::BacktestState::BacktestState() {}

Core::BacktestState::~BacktestState() {}

Core::BacktestState& Core::BacktestState::getInstance() {
    static BacktestState instance;
    return instance;
}

void Core::BacktestState::stop() {
    ::Core::Engine::getInstance().gracefully_stop();
}

void Core::BacktestState::update_quote_asset(
    const std::string& exchange_name, const std::string& market_type,
    long long account_id, const std::string& asset,
    const mpfr::mpreal& quote_asset_delta) {
    const auto& key = Utils::get_symbol_exchange_market_account_key(
        asset, exchange_name, market_type, account_id);
    auto it = this->backtest_quote_assets.find(key);

    QuoteAsset new_quote_asset;
    if (it == this->backtest_quote_assets.end()) {
        new_quote_asset = QuoteAsset{
            .asset = asset,
            .quantity = quote_asset_delta,
            .price = mpfr::mpreal(1.0),
            .account_id = account_id,
            .exchange_name = exchange_name,
            .market_type = market_type,
        };
    } else {
        new_quote_asset = QuoteAsset{
            .asset = it->second.asset,
            .quantity = it->second.quantity + quote_asset_delta,
            .price = it->second.price,
            .account_id = account_id,
            .exchange_name = exchange_name,
            .market_type = market_type,
        };
    }
    this->backtest_quote_assets[key] = new_quote_asset;
    ::Core::Engine::getInstance().update_quote_asset(
        exchange_name, market_type, new_quote_asset.asset, account_id,
        new_quote_asset);
}

void Core::BacktestState::update_fees(const Order& order,
                                      const mpfr::mpreal& fill_price,
                                      bool is_taker_execution) {
    const std::string& exchange_name = order.exchange_name;
    const std::string& market_type = order.market_type;
    const auto& fees =
        order.quantity * fill_price *
        this->get_fees_coeff(exchange_name, market_type, is_taker_execution);
    this->total_fees += fees;
    this->update_quote_asset(order.exchange_name, order.market_type,
                             order.account_id, order.get_backtest_quote_asset(),
                             -1 * fees);
}

void Core::BacktestState::update_pnl(const Order& order,
                                     const mpfr::mpreal& fill_price) {
    if (!order.trade_id.has_value()) {
        return;
    }

    // order.symbol is correct here
    auto trade = ::Core::Engine::getInstance().find_trade(
        order.strategy_id, order.symbol, order.trade_id.value());

    if (!trade.has_value()) {
        std::cout << "Order: " << order.to_json().dump() << std::endl;
        LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_BACKTEST_PHASE,
                 "Trade not found for order: " + order.symbol +
                     " Trade id: " + std::to_string(order.trade_id.value()),
                 (nlohmann::json{}));
        return;
    }

    bool is_short_close = order.side == Core::OrderSide::BUY;
    const auto& pnl =
        is_short_close
            ? (trade.value().get_average_open_price() - fill_price) *
                  order.quantity
            : (fill_price - trade.value().get_average_open_price()) *
                  order.quantity;

    auto total_pnl_before = this->total_pnl;

    this->total_pnl += pnl;
    this->total_trades++;
    this->total_volume += fill_price * order.quantity;
    if (pnl > 0) {
        this->winning_trades++;
        this->total_winning_pnl += pnl;
    } else {
        this->losing_trades++;
        this->total_losing_pnl += pnl;
    }
    this->update_quote_asset(order.exchange_name, order.market_type,
                             order.account_id, order.get_backtest_quote_asset(),
                             pnl);
}

void Core::BacktestState::backtest_update_inventory(const Order& order,
                                                    const mpfr::mpreal& fill_price,
                                                    bool is_taker_execution) {
    std::string key = Utils::get_symbol_exchange_market_account_key(
        order.get_potentially_alt_routed_symbol(), order.exchange_name,
        order.market_type, order.account_id);

    if (this->backtest_inventories.find(key) ==
        this->backtest_inventories.end()) {
        Core::Inventory inventory;

        inventory.asset = order.get_potentially_alt_routed_symbol();
        inventory.exchange_name = order.exchange_name;
        inventory.market_type = order.market_type;
        inventory.quote_conversion_symbol =
            order.get_potentially_alt_routed_base_asset() + "USDT";
        inventory.account_id = order.account_id;
        this->backtest_inventories[key] = inventory;
    }

    std::optional<mpfr::mpreal> used_margin = std::nullopt;

    if (order.market_type == SWAP_MARKET_TYPE) {
        used_margin =
            order.quantity * fill_price * this->sim_params.swap_leverage;
    }

    this->backtest_inventories[key].total += order.side == Core::OrderSide::BUY
                                                 ? order.quantity
                                                 : -1 * order.quantity;

    if (used_margin.has_value()) {
        auto price = ::Core::Engine::getInstance().get_price(
            order.get_potentially_alt_routed_symbol(), order.exchange_name,
            order.market_type);
        if (!price.has_value()) {
            return;
        }
        this->backtest_inventories[key].used_margin =
            std::abs(this->backtest_inventories[key].total.toDouble()) *
            price.value() * (1 / this->sim_params.swap_leverage);
    }

    this->update_fees(order, fill_price, is_taker_execution);
    this->update_pnl(order, fill_price);

    auto backtest_inventory = this->backtest_inventories.find(key);

    ::Core::Engine::getInstance().update_inventory(
        order.exchange_name, order.market_type,
        order.get_potentially_alt_routed_symbol(), order.account_id,
        backtest_inventory->second);
}

void Core::BacktestState::update_engine_state_snapshot(
    const EngineStateSnapshot& snapshot) {
    for (const auto& [exchange_name, trades] : snapshot.new_trades) {
        this->trades.insert(this->trades.end(), trades.begin(), trades.end());
    }
    if (snapshot.timestamp_ms - this->last_daily_state_sample_ms >=
        DAILY_STATE_SAMPLE_INTERVAL_MS) {
        this->daily_state_samples.push_back({
            snapshot.timestamp_ms,
            snapshot.nav.toDouble(),
            snapshot.long_exposure.toDouble(),
            snapshot.short_exposure.toDouble(),
        });
        this->last_daily_state_sample_ms = snapshot.timestamp_ms;
        if (!this->equity_curve_dump_path.empty() &&
            this->daily_state_samples.size() %
                    EQUITY_CURVE_FLUSH_EVERY_N_SAMPLES ==
                0) {
            this->dump_equity_curve_json(this->equity_curve_dump_path);
        }
    }

    const double nav = snapshot.nav.toDouble();
    if (nav > 0) {
        const double long_pct = 100.0 * snapshot.long_exposure.toDouble() / nav;
        const double short_pct =
            100.0 * snapshot.short_exposure.toDouble() / nav;
        const double gross_pct = long_pct + short_pct;
        if (long_pct > this->peak_long_exposure_pct) {
            this->peak_long_exposure_pct = long_pct;
            this->peak_long_exposure_ts_ms = snapshot.timestamp_ms;
        }
        if (short_pct > this->peak_short_exposure_pct) {
            this->peak_short_exposure_pct = short_pct;
            this->peak_short_exposure_ts_ms = snapshot.timestamp_ms;
        }
        if (gross_pct > this->peak_gross_exposure_pct) {
            this->peak_gross_exposure_pct = gross_pct;
            this->peak_gross_exposure_ts_ms = snapshot.timestamp_ms;
        }
    }
}

void Core::BacktestState::dump_summary_json(const std::string& path) const {
    const long long total_closed = this->winning_trades + this->losing_trades;
    const double win_rate =
        total_closed > 0
            ? static_cast<double>(this->winning_trades) / total_closed
            : 0.0;
    const double avg_win =
        this->winning_trades > 0
            ? this->total_winning_pnl.toDouble() / this->winning_trades
            : 0.0;
    const double avg_loss =
        this->losing_trades > 0
            ? this->total_losing_pnl.toDouble() / this->losing_trades
            : 0.0;
    const double total_pnl_d = this->total_pnl.toDouble();
    const double total_volume_d = this->total_volume.toDouble();
    const double ev_pct =
        total_volume_d > 0 ? total_pnl_d / total_volume_d : 0.0;
    const double profit_factor = this->total_losing_pnl.toDouble() < 0
                                     ? this->total_winning_pnl.toDouble() /
                                           -this->total_losing_pnl.toDouble()
                                     : 0.0;
    const double final_nav = ::Core::Engine::getInstance().get_nav();

    // Sharpe (annualized, log-returns of daily NAV samples × √365). Needs
    // ≥ 2 samples to define a single return; ≥ 3 for a non-zero stdev.
    double sharpe = 0.0;
    double mean_daily_log_ret = 0.0;
    double stdev_daily_log_ret = 0.0;
    const auto& s = this->daily_state_samples;
    if (s.size() >= 2) {
        std::vector<double> rets;
        rets.reserve(s.size() - 1);
        for (size_t i = 1; i < s.size(); ++i) {
            if (s[i - 1].nav > 0 && s[i].nav > 0) {
                rets.push_back(std::log(s[i].nav / s[i - 1].nav));
            }
        }
        if (!rets.empty()) {
            double sum = 0.0;
            for (double r : rets) sum += r;
            mean_daily_log_ret = sum / rets.size();
            if (rets.size() >= 2) {
                double sq = 0.0;
                for (double r : rets) {
                    const double d = r - mean_daily_log_ret;
                    sq += d * d;
                }
                stdev_daily_log_ret = std::sqrt(sq / (rets.size() - 1));
                if (stdev_daily_log_ret > 0) {
                    sharpe = mean_daily_log_ret / stdev_daily_log_ret *
                             std::sqrt(365.0);
                }
            }
        }
    }

    nlohmann::json j;
    j["window"] = {
        {"start_ms", this->simulation_start_timestamp_ms},
        {"end_ms", this->simulation_end_timestamp_ms},
    };
    j["pnl"] = {
        {"total", total_pnl_d},
        {"winning", this->total_winning_pnl.toDouble()},
        {"losing", this->total_losing_pnl.toDouble()},
        {"profit_factor", profit_factor},
    };
    j["fees"] = this->total_fees.toDouble();
    j["volume"] = total_volume_d;
    j["ev_pct_of_volume"] = ev_pct;
    j["nav"] = {
        {"final", final_nav},
        {"peak", this->peak_balance.toDouble()},
        {"max_drawdown_pct", this->max_drawdown.toDouble() * 100.0},
    };
    j["risk_peaks"] = {
        {"max_drawdown",
         {
             {"pct", this->max_drawdown.toDouble() * 100.0},
             {"ts_ms", this->max_drawdown_ts_ms},
         }},
        {"peak_long_exposure",
         {
             {"pct", this->peak_long_exposure_pct},
             {"ts_ms", this->peak_long_exposure_ts_ms},
         }},
        {"peak_short_exposure",
         {
             {"pct", this->peak_short_exposure_pct},
             {"ts_ms", this->peak_short_exposure_ts_ms},
         }},
        {"peak_gross_exposure",
         {
             {"pct", this->peak_gross_exposure_pct},
             {"ts_ms", this->peak_gross_exposure_ts_ms},
         }},
    };
    j["trades"] = {
        {"closed", total_closed},      {"won", this->winning_trades},
        {"lost", this->losing_trades}, {"win_rate", win_rate},
        {"avg_win", avg_win},          {"avg_loss", avg_loss},
    };
    j["sharpe"] = {
        {"annualized", sharpe},
        {"daily_samples", static_cast<long long>(s.size())},
        {"mean_daily_log_return", mean_daily_log_ret},
        {"stdev_daily_log_return", stdev_daily_log_ret},
    };

    std::ofstream out(path);
    out << j.dump(2);
}

void Core::BacktestState::dump_equity_curve_json(
    const std::string& path) const {
    nlohmann::json curve = nlohmann::json::array();
    for (const auto& sample : this->daily_state_samples) {
        curve.push_back({
            {"ts_ms", sample.ts_ms},
            {"nav", sample.nav},
            {"long_exposure", sample.long_exposure},
            {"short_exposure", sample.short_exposure},
        });
    }
    // Atomic write: tmp + rename so mid-run readers never see a truncated file.
    const std::string tmp_path = path + ".tmp";
    {
        std::ofstream out(tmp_path);
        out << curve.dump(2);
    }
    std::rename(tmp_path.c_str(), path.c_str());
}

std::string Core::BacktestState::get_fee_rates_key(
    const std::string& exchange_name, const std::string& market_type,
    bool is_taker) {
    return Utils::get_exchange_market_key(exchange_name, market_type) +
           (is_taker ? "_taker" : "_maker");
}

mpfr::mpreal Core::BacktestState::get_fees_coeff(
    const std::string& exchange_name, const std::string& market_type,
    bool is_taker) {
    const auto& key =
        this->get_fee_rates_key(exchange_name, market_type, is_taker);
    return this->fees_coeff_by_exchange_and_market[key];
}

void Core::BacktestState::add_sim_params(const Core::SimulationInput& params) {
    this->sim_params = params;
    this->fees_coeff = params.fee_rate_coeff;
    this->simulation_start_timestamp_ms = params.simulation_start_date_ms;
    this->simulation_end_timestamp_ms = params.simulation_end_date_ms;
    this->is_autofilling_orders = params.autofill_orders;
    this->exchange_latency_enabled_ = params.latency_to_exchange > 0;
    if (this->sim_params.fill_bars_aggregation_ticks <= 0) {
        this->sim_params.fill_bars_aggregation_ticks = 1;
    }
    ::Core::Engine::getInstance()
        .skip_historical_order_accumulation_on_backtest =
        params.skip_historical_order_accumulation_on_backtest;

    this->read_market_events_chunk_start_timestamp_ms =
        this->simulation_start_timestamp_ms -
        this->sim_params.market_data_reading_offset_ms;
    if (params.market_data_replay.load_mode == "streaming") {
        this->read_market_events_chunk_size =
            params.market_data_replay.chunk_duration_ms;
        this->streaming_ohlcv_cache_.clear();
        this->streaming_ohlcv_cache_initialized_ = false;
    }
    this->read_market_events_chunk_end_timestamp_ms =
        this->read_market_events_chunk_start_timestamp_ms +
        this->read_market_events_chunk_size;

    if (params.market_data_replay.load_mode != "streaming" &&
        params.read_all_market_data_at_once) {
        this->read_market_events_chunk_end_timestamp_ms =
            this->simulation_end_timestamp_ms;
    }

    std::unordered_set<long long> account_ids;
    for (const auto& exchange : this->sim_params.exchanges) {
        for (const auto& account : exchange.accounts) {
            account_ids.insert(account.account_id);
        }
    }
    this->account_ids =
        std::vector<long long>(account_ids.begin(), account_ids.end());

    this->init_exchanges();
    this->init_ohlcv_datasets();
    this->init_funding_rate_datasets();
}

void Core::BacktestState::init_exchanges() {
    for (const auto& exchange : this->sim_params.exchanges) {
        for (const auto& account : exchange.accounts) {
            for (const auto& quote_asset : account.quote_assets) {
                this->backtest_quote_assets
                    [Utils::get_symbol_exchange_market_account_key(
                        quote_asset.asset, exchange.exchange_name,
                        exchange.market_type, account.account_id)] =
                    QuoteAsset{
                        .asset = quote_asset.asset,
                        .quantity = quote_asset.initial_balance,
                        .price = quote_asset.price_multiplier,
                        .account_id = account.account_id,
                    };
                ::Core::Engine::getInstance().update_quote_asset(
                    exchange.exchange_name, exchange.market_type,
                    quote_asset.asset, account.account_id,
                    QuoteAsset{
                        .asset = quote_asset.asset,
                        .quantity = quote_asset.initial_balance,
                        .price = quote_asset.price_multiplier,
                        .account_id = account.account_id,
                    });

                const std::string key_taker = this->get_fee_rates_key(
                    exchange.exchange_name, exchange.market_type, true);
                this->fees_coeff_by_exchange_and_market[key_taker] =
                    exchange.taker_fee_rate;
                const std::string key_maker = this->get_fee_rates_key(
                    exchange.exchange_name, exchange.market_type, false);
                this->fees_coeff_by_exchange_and_market[key_maker] =
                    exchange.maker_fee_rate;

                if (exchange.maker_rebate_rate > 0) {
                    const std::string rebate_key =
                        exchange.exchange_name + ":" + exchange.market_type;
                    this->maker_rebate_rates[rebate_key] =
                        exchange.maker_rebate_rate;
                }
            }
        }
    }
}

void Core::BacktestState::init_funding_rate_datasets() {
    std::unordered_set<std::string> loaded_paths;
    for (const auto& ohlcv_dataset :
         this->sim_params.market_data.ohlcv_klines) {
        if (this->sim_params.market_data_replay.load_mode == "streaming" &&
            ohlcv_dataset.market_type == "stock") {
            continue;
        }
        Core::BacktestFundingRateDataset backtest_funding_rate_dataset;

        backtest_funding_rate_dataset.symbol = ohlcv_dataset.symbol;
        backtest_funding_rate_dataset.exchange_name =
            ohlcv_dataset.exchange_name;
        backtest_funding_rate_dataset.market_type = ohlcv_dataset.market_type;
        backtest_funding_rate_dataset.dataset_path =
            ::MarketData::get_funding_rate_base_dir(
                ohlcv_dataset.symbol, ohlcv_dataset.exchange_name,
                ohlcv_dataset.market_type, false);
        if (!loaded_paths.insert(backtest_funding_rate_dataset.dataset_path)
                 .second) {
            continue;
        }
        this->funding_rate_datasets.push_back(backtest_funding_rate_dataset);
    }
}

void Core::BacktestState::init_ohlcv_datasets() {
    for (const auto& ohlcv_dataset :
         this->sim_params.market_data.ohlcv_klines) {
        Core::BacktestOhlcvDataset backtest_ohlcv_dataset;
        const auto& bar_close_policy =
            ::MarketData::parse_bar_type_from_path(ohlcv_dataset.path);

        ::MarketData::OhlcvBarMeta ohlcv_bar_meta;
        ohlcv_bar_meta.bar_close_policy = bar_close_policy;
        ohlcv_bar_meta.exchange_name = ohlcv_dataset.exchange_name;
        ohlcv_bar_meta.market_type = ohlcv_dataset.market_type;
        ohlcv_bar_meta.symbol = ohlcv_dataset.symbol;

        backtest_ohlcv_dataset.symbol = ohlcv_dataset.symbol;
        backtest_ohlcv_dataset.exchange_name = ohlcv_dataset.exchange_name;
        backtest_ohlcv_dataset.market_type = ohlcv_dataset.market_type;
        backtest_ohlcv_dataset.ohlcv_bar_meta = ohlcv_bar_meta;
        if (this->sim_params.market_data.ohlcv_dataset_selection
                .has_value()) {
            backtest_ohlcv_dataset.dataset_path = ohlcv_dataset.path;
        } else {
            backtest_ohlcv_dataset.dataset_path =
                ::MarketData::get_derived_bars_base_dir(
                    ohlcv_dataset.symbol, ohlcv_dataset.exchange_name,
                    ohlcv_dataset.market_type, bar_close_policy, false);
        }

        if (this->sim_params.market_data_replay.load_mode != "streaming") {
            std::cout << "Dataset path: "
                      << backtest_ohlcv_dataset.dataset_path << std::endl;
        }

        auto dataset = std::make_shared<::MarketData::OhlcvDataset>(
            ohlcv_bar_meta,
            ChronoUtils::make_timepoint(2000, 1, 1, 0, 0, 0), false);

        dataset->mark_init_sync_done();
        ::MarketData::BarAggregator::getInstance().add_ohlcv_dataset(
            dataset);
        this->ohlcv_datasets.push_back(backtest_ohlcv_dataset);
    }
}

void Core::BacktestState::prefeed_market_events(
    const MarketEvent& market_event) {
    if (market_event.type == MarketEventType::OHLCV_UPDATE) {
        ::MarketData::DatasetsManager::get_instance().add_ohlcv_tick(
            market_event.bar_flush_info.value());
    } else if (market_event.type ==
               MarketEventType::POLYMARKET_MARKET_CREATED) {
        this->handle_polymarket_market_created(market_event);
    } else if (market_event.type == MarketEventType::POLYMARKET_MARKET_CLOSED) {
        this->handle_polymarket_market_closed(market_event);
    }
}

void Core::BacktestState::loop_market_events() {
    long long num_market_events = 0;
    while (this->read_market_events_chunk_start_timestamp_ms <
           this->simulation_end_timestamp_ms) {
        this->read_market_events();
        for (long long i = 0; i < this->market_events.size(); i++) {
            auto& market_event = this->market_events[i];

            if (market_event.event_time > this->simulation_start_timestamp_ms &&
                market_event.event_time < this->simulation_end_timestamp_ms) {
                const bool is_last_on_interval =
                    (i + 1 >= static_cast<long long>(this->market_events.size())) ||
                    (this->market_events[i + 1].event_time >
                     market_event.event_time);
                this->process_market_event(this->market_events[i],
                                           is_last_on_interval);
            } else {
                this->prefeed_market_events(market_event);
            }
            ++num_market_events;

            if (num_market_events % 1000 == 0) {
                while (this->is_paused.load(std::memory_order_relaxed)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
            }

            if (num_market_events % 100000 == 0) {
                LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_BACKTEST_PHASE,
                         "Processed " + std::to_string(num_market_events) +
                             " market events",
                         (nlohmann::json{}));
                this->print_hotpath_profile(market_event.event_time);
            }
        }
    }
    this->flush_pending_fill_checks();
    this->process_scheduled_book_removals(this->simulation_end_timestamp_ms);
    ::Core::Engine::getInstance().run_periodic_strategy_cleanup();
    this->print_hotpath_profile(this->simulation_end_timestamp_ms, true);
}

bool Core::BacktestState::ensure_paced_market_event_available() {
    while (this->paced_market_event_index_ >= this->market_events.size()) {
        if (this->read_market_events_chunk_start_timestamp_ms >=
            this->simulation_end_timestamp_ms) {
            return false;
        }
        this->read_market_events();
        this->paced_market_event_index_ = 0;
        if (!this->market_events.empty()) return true;
    }
    return true;
}

void Core::BacktestState::observe_paced_ohlcv(
    const MarketEvent& event, const PacedOhlcvObserver& observer) const {
    if (observer && event.type == MarketEventType::OHLCV_UPDATE &&
        event.bar_flush_info.has_value()) {
        observer(event);
    }
}

void Core::BacktestState::prepare_paced_backtest(
    const PacedOhlcvObserver& observer) {
    if (this->paced_backtest_prepared_) {
        throw std::runtime_error("paced backtest is already prepared");
    }
    this->init_state();
    ::Core::Engine::getInstance().state_is_ready();
    this->market_events.clear();
    this->paced_market_event_index_ = 0;
    this->paced_market_events_processed_ = 0;
    this->paced_backtest_prepared_ = true;
    this->paced_backtest_finished_ = false;

    while (this->ensure_paced_market_event_available()) {
        const MarketEvent& event =
            this->market_events[this->paced_market_event_index_];
        if (event.event_time > this->simulation_start_timestamp_ms) break;
        this->prefeed_market_events(event);
        this->observe_paced_ohlcv(event, observer);
        ++this->paced_market_event_index_;
        ++this->paced_market_events_processed_;
    }
}

bool Core::BacktestState::advance_paced_backtest_through(
    long long target_time_ms, const PacedOhlcvObserver& observer) {
    if (!this->paced_backtest_prepared_) {
        throw std::runtime_error("paced backtest is not prepared");
    }
    if (this->paced_backtest_finished_) return false;

    const long long clamped_target =
        std::min(target_time_ms, this->simulation_end_timestamp_ms);
    while (this->ensure_paced_market_event_available()) {
        MarketEvent& event =
            this->market_events[this->paced_market_event_index_];
        if (event.event_time > clamped_target ||
            event.event_time >= this->simulation_end_timestamp_ms) {
            break;
        }

        if (event.event_time > this->simulation_start_timestamp_ms) {
            const bool is_last_on_interval =
                this->paced_market_event_index_ + 1 >=
                    this->market_events.size() ||
                this->market_events[this->paced_market_event_index_ + 1]
                        .event_time > event.event_time;
            this->process_market_event(event, is_last_on_interval);
            this->observe_paced_ohlcv(event, observer);
        } else {
            this->prefeed_market_events(event);
            this->observe_paced_ohlcv(event, observer);
        }

        ++this->paced_market_event_index_;
        ++this->paced_market_events_processed_;
        if (this->paced_market_events_processed_ % 100000 == 0) {
            LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_BACKTEST_PHASE,
                     "Processed " +
                         std::to_string(this->paced_market_events_processed_) +
                         " paced market events",
                     (nlohmann::json{}));
            this->print_hotpath_profile(event.event_time);
        }
    }

    if (clamped_target < this->simulation_end_timestamp_ms) return true;

    while (this->ensure_paced_market_event_available()) {
        const MarketEvent& event =
            this->market_events[this->paced_market_event_index_];
        if (event.event_time >= this->simulation_end_timestamp_ms) break;
        throw std::runtime_error(
            "paced replay did not process all events through its target");
    }
    this->paced_backtest_finished_ = true;
    return false;
}

void Core::BacktestState::finish_paced_backtest() {
    if (!this->paced_backtest_prepared_ ||
        !this->paced_backtest_finished_) {
        throw std::runtime_error("paced backtest has not reached its end");
    }
    this->flush_pending_fill_checks();
    this->process_scheduled_book_removals(this->simulation_end_timestamp_ms);
    ::Core::Engine::getInstance().run_periodic_strategy_cleanup();
    this->print_hotpath_profile(this->simulation_end_timestamp_ms, true);
    this->print_debug_state(this->last_market_event_time_ms);
    this->stop();
}

void Core::BacktestState::run_backtest() {
    this->init_state();
    ::Core::Engine::getInstance().state_is_ready();
    this->loop_market_events();
    this->print_debug_state(this->last_market_event_time_ms);
    this->stop();
}

void Core::BacktestState::read_aggr_trades() {
    for (const auto& dataset : this->sim_params.market_data.aggr_trades) {
        if (this->sim_params.skip_reading_data_with_offset_on_trades &&
            this->read_market_events_chunk_end_timestamp_ms <
                this->simulation_start_timestamp_ms) {
            LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_BACKTEST_PHASE,
                     "Skipping aggr trades for " + dataset.symbol +
                         " because the end of the chunk is before the "
                         "simulation start timestamp",
                     (nlohmann::json{}));
            continue;
        }
        const auto& ticks = SqliteDatasets::load_trades_dataset_dir(
            dataset.path, this->read_market_events_chunk_start_timestamp_ms,
            this->read_market_events_chunk_end_timestamp_ms);

        for (const auto& tick : ticks) {
            Core::MarketEvent market_event;
            market_event.type = MarketEventType::AGGR_TRADE;
            market_event.symbol = dataset.symbol;
            market_event.event_time = tick.timestamp;
            market_event.aggr_trade = tick;
            market_event.exchange_name = dataset.exchange_name;
            market_event.market_type = dataset.market_type;
            market_event.price = tick.price;
            this->market_events.push_back(market_event);

            if (market_event.event_time > this->price_data_end_timestamp_ms) {
                this->price_data_end_timestamp_ms = market_event.event_time;
            }
        }
    }
}

void Core::BacktestState::read_funding_rates() {
    // Create a thread pool with number of threads equal to available CPU
    // cores
    ThreadPool thread_pool(std::thread::hardware_concurrency());
    std::vector<std::future<std::vector<Core::MarketEvent>>> futures;

    // Submit tasks for each dataset to the thread pool
    for (const auto& dataset : this->funding_rate_datasets) {
        auto future = thread_pool.enqueue(
            [this, dataset]() -> std::vector<Core::MarketEvent> {
                std::vector<Core::MarketEvent> dataset_events;

                const auto& funding_rate_ticks =
                    ParquetUtils::load_funding_rate_ticks(
                        dataset.dataset_path,
                        this->read_market_events_chunk_start_timestamp_ms,
                        this->read_market_events_chunk_end_timestamp_ms);

                for (const auto& tick : funding_rate_ticks) {
                    if (tick.timestamp_ms < this->read_market_events_chunk_start_timestamp_ms ||
                        tick.timestamp_ms >= this->read_market_events_chunk_end_timestamp_ms) {
                        continue;
                    }
                    Core::MarketEvent market_event;
                    market_event.type = MarketEventType::FUNDING_PAYMENT;
                    market_event.symbol = dataset.symbol;
                    market_event.event_time = tick.timestamp_ms;
                    market_event.funding_rate_paid = tick.funding_rate;
                    market_event.exchange_name = dataset.exchange_name;
                    market_event.market_type = dataset.market_type;
                    dataset_events.push_back(market_event);
                }

                return dataset_events;
            });

        futures.push_back(std::move(future));
    }

    // Collect results from all threads
    for (auto& future : futures) {
        auto dataset_events = future.get();
        this->market_events.insert(this->market_events.end(),
                                   dataset_events.begin(),
                                   dataset_events.end());
    }
}

void Core::BacktestState::read_model_fold_events() {
    // Collect unique schedule paths across all loaded strategies — N strategies
    // can subscribe to one bundle, so dedupe at the file-read layer.
    std::unordered_set<std::string> schedule_paths;
    auto strategies = ::Core::Engine::getInstance().get_strategies();
    for (const auto& strategy : strategies) {
        const auto& p = strategy->strategy_config.model_schedule_path;
        if (!p.empty()) {
            schedule_paths.insert(p);
        }
    }
    if (schedule_paths.empty()) {
        return;
    }

    const long long chunk_start =
        this->read_market_events_chunk_start_timestamp_ms;
    const long long chunk_end =
        this->read_market_events_chunk_end_timestamp_ms;

    for (const auto& schedule_path : schedule_paths) {
        auto entries = Core::FoldScheduleLoader::load(schedule_path);
        if (entries.empty()) {
            LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_BACKTEST_PHASE,
                     "fold_schedule empty/unreadable: " + schedule_path,
                     (nlohmann::json{}));
            continue;
        }

        // Warm-up: latest entry whose timestamp <= chunk_start becomes the
        // strategy's starting bundle for this chunk, emitted AT chunk_start so
        // the strategy has a model loaded before any market events fire. This
        // makes each chunk self-contained (no cross-chunk state assumption).
        const Core::FoldScheduleLoader::LoadedEntry* warmup = nullptr;
        for (const auto& le : entries) {
            if (le.timestamp <= chunk_start &&
                (warmup == nullptr || le.timestamp > warmup->timestamp)) {
                warmup = &le;
            }
        }
        if (warmup != nullptr) {
            Core::MarketEvent ev;
            ev.type = MarketEventType::MODEL_UPDATE;
            ev.event_time = chunk_start;
            ev.model_update = warmup->event;
            this->market_events.push_back(std::move(ev));
        }

        // Entries strictly within (chunk_start, chunk_end) fire at their
        // natural timestamps. Boundary at chunk_start is owned by the warm-up
        // event above; boundary at chunk_end belongs to the next chunk.
        for (const auto& le : entries) {
            if (le.timestamp > chunk_start && le.timestamp < chunk_end) {
                Core::MarketEvent ev;
                ev.type = MarketEventType::MODEL_UPDATE;
                ev.event_time = le.timestamp;
                ev.model_update = le.event;
                this->market_events.push_back(std::move(ev));
            }
        }
    }
}

void Core::BacktestState::read_ohlcv_klines() {
    if (this->sim_params.market_data_replay.load_mode == "streaming") {
        this->read_streaming_ohlcv_klines();
        return;
    }
    // Create a thread pool with number of threads equal to available CPU
    // cores
    ThreadPool thread_pool(std::thread::hardware_concurrency());
    std::vector<std::future<std::vector<Core::MarketEvent>>> futures;

    // Submit tasks for each dataset to the thread pool
    for (const auto& dataset : this->ohlcv_datasets) {
        auto future = thread_pool.enqueue(
            [this, dataset]() -> std::vector<Core::MarketEvent> {
                std::vector<Core::MarketEvent> dataset_events;

                // sqlite retired — read bars from parquet partitions. The
                // dataset path is the symbol base dir; bars live under
                // <base>/parquets/year=*/month=*/day=*/<YYYY-MM-DD>.parquet.
                // scan_partitions returns an arrow Table for the [start, end)
                // window; from_arrow_table converts to OhlcvDatasetTick vec.
                const std::string parquets_path =
                    dataset.dataset_path + "/parquets";
                const long long start_ms =
                    this->read_market_events_chunk_start_timestamp_ms;
                const long long end_ms =
                    this->read_market_events_chunk_end_timestamp_ms;
                const auto start_tp =
                    ChronoUtils::make_utc_timepoint_from_epoch_ms(start_ms);
                const auto end_tp =
                    ChronoUtils::make_utc_timepoint_from_epoch_ms(end_ms);
                std::vector<::MarketData::OhlcvDatasetTick> ohlcv_ticks;
                try {
                    auto arrow_table = ParquetUtils::scan_partitions(
                        parquets_path, start_tp, end_tp);
                    if (arrow_table) {
                        ohlcv_ticks =
                            ::MarketData::OhlcvDatasetTick::from_arrow_table(
                                arrow_table);
                        // scan_partitions returns whole-day partitions; trim
                        // to the exact chunk window (the sqlite reader did
                        // this in-query via SQL BETWEEN).
                        ohlcv_ticks.erase(
                            std::remove_if(
                                ohlcv_ticks.begin(), ohlcv_ticks.end(),
                                [start_ms, end_ms](
                                    const ::MarketData::OhlcvDatasetTick& t) {
                                    return t.kline_close_time < start_ms ||
                                           t.kline_close_time >= end_ms;
                                }),
                            ohlcv_ticks.end());
                        std::sort(
                            ohlcv_ticks.begin(), ohlcv_ticks.end(),
                            [](const ::MarketData::OhlcvDatasetTick& a,
                               const ::MarketData::OhlcvDatasetTick& b) {
                                return a.kline_close_time < b.kline_close_time;
                            });
                    }
                } catch (const std::exception& e) {
                    std::cerr << "[read_ohlcv_klines] parquet read failed "
                              << dataset.symbol << ": " << e.what() << "\n";
                }

                const std::string& ohlcv_identifier =
                    ::MarketData::get_derived_bars_base_dir(
                        dataset.symbol, dataset.exchange_name,
                        dataset.market_type,
                        dataset.ohlcv_bar_meta.bar_close_policy, true);

                dataset_events.reserve(ohlcv_ticks.size());
                for (const auto& tick : ohlcv_ticks) {
                    Core::MarketEvent market_event;
                    market_event.type = MarketEventType::OHLCV_UPDATE;
                    market_event.symbol = dataset.symbol;
                    market_event.event_time =
                        tick.kline_close_time + this->sim_params.ohlcv_delay_ms;
                    market_event.exchange_name = dataset.exchange_name;
                    market_event.market_type = dataset.market_type;
                    market_event.bar_flush_info = BarFlushInfo{
                        .tick = tick,
                        .bar_identifier = ohlcv_identifier,
                    };
                    dataset_events.push_back(market_event);
                }
                // A dataset can have zero bars in this chunk's window (e.g.
                // a symbol listed mid-simulation, or delisted before chunk
                // start). `dataset_events.back()` on an empty vector is UB —
                // guard the debug print, return the empty vector cleanly.
                if (!dataset_events.empty()) {
                    std::cout << "ohlcv " << dataset.symbol
                              << " last event time: "
                              << dataset_events.back().event_time << std::endl;
                } else {
                    std::cout
                        << "ohlcv " << dataset.symbol << " no bars in window ["
                        << this->read_market_events_chunk_start_timestamp_ms
                        << ", "
                        << this->read_market_events_chunk_end_timestamp_ms
                        << ")" << std::endl;
                }

                return dataset_events;
            });

        futures.push_back(std::move(future));
    }

    // Collect results from all threads
    for (auto& future : futures) {
        auto dataset_events = future.get();
        this->market_events.insert(this->market_events.end(),
                                   dataset_events.begin(),
                                   dataset_events.end());
    }
}

void Core::BacktestState::initialize_streaming_ohlcv_cache() {
    const auto window_ms = this->sim_params.market_data_replay.cache_window_ms;
    if (this->streaming_ohlcv_cache_initialized_ &&
        (window_ms == 0 || this->read_market_events_chunk_start_timestamp_ms <
                               this->streaming_ohlcv_cache_end_ms_)) return;
    this->streaming_ohlcv_cache_.clear();
    this->streaming_ohlcv_cache_.resize(this->ohlcv_datasets.size());
    this->streaming_datasets_loaded_.store(0);
    this->streaming_datasets_total_.store(this->ohlcv_datasets.size());
    if (this->ohlcv_datasets.empty()) {
        this->streaming_ohlcv_cache_initialized_ = true;
        return;
    }

    const int configured_threads =
        this->sim_params.market_data_replay.reader_threads;
    const unsigned int reader_threads =
        configured_threads > 0
            ? static_cast<unsigned int>(configured_threads)
            : std::max(1u, std::thread::hardware_concurrency());
    ThreadPool thread_pool(reader_threads);
    std::vector<std::future<std::vector<CompactOhlcvBar>>> futures;
    futures.reserve(this->ohlcv_datasets.size());
    const long long start_ms = window_ms > 0
        ? this->read_market_events_chunk_start_timestamp_ms
        : this->simulation_start_timestamp_ms -
              this->sim_params.market_data_reading_offset_ms;
    const long long end_ms = window_ms > 0
        ? std::min(this->simulation_end_timestamp_ms, start_ms + window_ms)
        : this->simulation_end_timestamp_ms;
    this->streaming_ohlcv_cache_end_ms_ = end_ms;

    for (const auto& dataset : this->ohlcv_datasets) {
        futures.push_back(thread_pool.enqueue(
            [this, dataset, start_ms,
             end_ms]() -> std::vector<CompactOhlcvBar> {
                const auto start_tp =
                    ChronoUtils::make_utc_timepoint_from_epoch_ms(start_ms);
                const auto end_tp =
                    ChronoUtils::make_utc_timepoint_from_epoch_ms(end_ms);
                std::vector<::MarketData::OhlcvDatasetTick> ticks;
                try {
                    auto table = ParquetUtils::scan_partitions(
                        dataset.dataset_path + "/parquets", start_tp, end_tp);
                    if (table) {
                        ticks = ::MarketData::OhlcvDatasetTick::from_arrow_table(
                            table);
                    }
                } catch (const std::exception& error) {
                    this->streaming_datasets_loaded_.fetch_add(1);
                    throw std::runtime_error(
                        "OHLCV parquet read failed for " + dataset.symbol +
                        ": " + error.what());
                }
                ticks.erase(
                    std::remove_if(
                        ticks.begin(), ticks.end(),
                        [start_ms, end_ms](
                            const ::MarketData::OhlcvDatasetTick& tick) {
                            return tick.kline_close_time < start_ms ||
                                   tick.kline_close_time >= end_ms;
                        }),
                    ticks.end());
                std::sort(
                    ticks.begin(), ticks.end(),
                    [](const ::MarketData::OhlcvDatasetTick& left,
                       const ::MarketData::OhlcvDatasetTick& right) {
                        return left.kline_close_time < right.kline_close_time;
                    });

                std::vector<CompactOhlcvBar> bars;
                bars.reserve(ticks.size());
                for (const auto& tick : ticks) {
                    bars.push_back({
                        .open_time_ms = tick.kline_open_time,
                        .close_time_ms = tick.kline_close_time,
                        .open = tick.open_price,
                        .high = tick.high_price,
                        .low = tick.low_price,
                        .close = tick.close_price,
                        .volume = tick.volume,
                        .quote_asset_volume = tick.quote_asset_volume,
                        .number_of_trades = tick.number_of_trades,
                        .taker_buy_base_volume =
                            tick.taker_buy_base_volume,
                        .taker_buy_quote_volume =
                            tick.taker_buy_quote_volume,
                        .is_closed = tick.is_closed,
                        .is_read_from_slow_storage =
                            tick.is_read_from_slow_storage,
                        .is_read_from_fast_storage =
                            tick.is_read_from_fast_storage,
                        .last_trade_timestamp_ms =
                            tick.last_trade_timestamp_ms,
                    });
                }
                this->streaming_datasets_loaded_.fetch_add(1);
                return bars;
            }));
    }

    std::size_t total_bars = 0;
    for (std::size_t index = 0; index < futures.size(); ++index) {
        this->streaming_ohlcv_cache_[index] = futures[index].get();
        total_bars += this->streaming_ohlcv_cache_[index].size();
    }
    this->streaming_ohlcv_cache_initialized_ = true;
    std::cout << "[backtest_runner] compact OHLCV cache ready: "
              << total_bars << " bars across " << this->ohlcv_datasets.size()
              << " datasets" << std::endl;
}

void Core::BacktestState::read_streaming_ohlcv_klines() {
    this->initialize_streaming_ohlcv_cache();
    for (std::size_t index = 0; index < this->ohlcv_datasets.size(); ++index) {
        const BacktestOhlcvDataset& dataset = this->ohlcv_datasets[index];
        const auto& bars = this->streaming_ohlcv_cache_[index];
        auto bar = std::lower_bound(
            bars.begin(), bars.end(),
            this->read_market_events_chunk_start_timestamp_ms,
            [](const CompactOhlcvBar& candidate, long long timestamp) {
                return candidate.close_time_ms < timestamp;
            });
        const std::string ohlcv_identifier =
            ::MarketData::get_derived_bars_base_dir(
                dataset.symbol, dataset.exchange_name, dataset.market_type,
                dataset.ohlcv_bar_meta.bar_close_policy, true);
        while (bar != bars.end() &&
               bar->close_time_ms <
                   this->read_market_events_chunk_end_timestamp_ms) {
            ::MarketData::OhlcvDatasetTick tick;
            tick.kline_open_time = bar->open_time_ms;
            tick.kline_close_time = bar->close_time_ms;
            tick.open_price = bar->open;
            tick.high_price = bar->high;
            tick.low_price = bar->low;
            tick.close_price = bar->close;
            tick.volume = bar->volume;
            tick.quote_asset_volume = bar->quote_asset_volume;
            tick.number_of_trades = bar->number_of_trades;
            tick.taker_buy_base_volume = bar->taker_buy_base_volume;
            tick.taker_buy_quote_volume = bar->taker_buy_quote_volume;
            tick.is_closed = bar->is_closed;
            tick.is_read_from_slow_storage =
                bar->is_read_from_slow_storage;
            tick.is_read_from_fast_storage =
                bar->is_read_from_fast_storage;
            tick.last_trade_timestamp_ms = bar->last_trade_timestamp_ms;

            Core::MarketEvent event;
            event.type = MarketEventType::OHLCV_UPDATE;
            event.symbol = dataset.symbol;
            event.event_time =
                tick.kline_close_time + this->sim_params.ohlcv_delay_ms;
            event.exchange_name = dataset.exchange_name;
            event.market_type = dataset.market_type;
            event.bar_flush_info = BarFlushInfo{
                .tick = std::move(tick),
                .bar_identifier = ohlcv_identifier,
            };
            this->market_events.push_back(std::move(event));
            ++bar;
        }
    }
}

void Core::BacktestState::init_strategies() {
    const auto& strategies = this->sim_params.strategies;

    std::vector<Core::DiscretionaryAccount> discretionary_accounts;
    if (std::any_of(strategies.begin(), strategies.end(),
                    [](const auto& strategy) {
                        return strategy.discretionary_multi_account;
                    })) {
        for (const auto& exchange : this->sim_params.exchanges) {
            for (const auto& account : exchange.accounts) {
                for (const auto& config :
                     this->sim_params.discretionary_accounts) {
                    if (config.account_id != account.account_id) continue;
                    double capital = 0;
                    for (const auto& quote : account.quote_assets) {
                        capital += quote.initial_balance * quote.price_multiplier;
                    }
                    discretionary_accounts.push_back(
                        {.id = account.account_id,
                         .paper = config.paper,
                         .enabled = config.enabled,
                         .exchange = exchange.exchange_name,
                         .market = exchange.market_type,
                         .capital = capital,
                         .max_exposure = config.max_directional_exposure,
                         .max_order_notional = config.max_order_notional,
                         .rate_10s = config.num_max_orders_10_sec,
                         .rate_1m = config.num_max_orders_1_min,
                         .rate_1h = config.num_max_orders_1_hour});
                }
            }
        }
        Core::DiscretionaryAccounts::instance().configure(discretionary_accounts,
                                                          false);
    }
    for (auto strategy : strategies) {
        if (strategy.discretionary_multi_account &&
            (strategy.strategy_name == "crypto-discretionary-paper" ||
             strategy.strategy_name == "crypto-discretionary-realmoney")) {
            strategy.exchanges_list.clear();
            for (const auto& account : discretionary_accounts) {
                if (account.paper == strategy.is_paper_trading) {
                    strategy.exchanges_list.push_back(
                        {account.exchange, account.market, account.id});
                }
            }
        }

        auto strategy_ptr = std::make_unique<Core::Strategy>(strategy);

        for (const auto& trade_ticker : strategy.trade_tickers) {
            LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_BACKTEST_PHASE,
                     "Adding trade ticker: " + trade_ticker.symbol,
                     (nlohmann::json{}));
            auto ticker_ptr = std::make_shared<Core::BacktestTradeTicker>(
                strategy.exchanges_list, trade_ticker.symbol,
                trade_ticker.quote_asset, trade_ticker.base_asset, strategy.id);
            ticker_ptr->configure_from_strategy(strategy);
            ticker_ptr->populate_polymarket_fields(trade_ticker);
            strategy_ptr->add_trade_ticker(trade_ticker.symbol,
                                           std::move(ticker_ptr));
            LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_BACKTEST_PHASE,
                     "Trade ticker added", (nlohmann::json{}));
        }
        ::Core::Engine::getInstance().add_strategy(std::move(strategy_ptr));
    }
}

void Core::BacktestState::read_trades() {
    for (const auto& dataset : this->sim_params.market_data.trades) {
        if (dataset.exchange_name == "polymarket") continue;

        if (this->sim_params.skip_reading_data_with_offset_on_trades &&
            this->read_market_events_chunk_end_timestamp_ms <
                this->simulation_start_timestamp_ms) {
            LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_BACKTEST_PHASE,
                     "Skipping trades for " + dataset.symbol +
                         " because the end of the chunk is before the "
                         "simulation start timestamp",
                     (nlohmann::json{}));
            continue;
        }

        // Detect whether data is parquet (hive-partitioned) or sqlite
        bool is_parquet = std::filesystem::exists(dataset.path + "/parquets");

        std::vector<AggrTradeDatasetTick> ticks;
        if (is_parquet) {
            ticks = ParquetUtils::load_aggr_trades_parquet(
                dataset.path, this->read_market_events_chunk_start_timestamp_ms,
                this->read_market_events_chunk_end_timestamp_ms);
        } else {
            ticks = SqliteDatasets::load_trades_dataset_dir(
                dataset.path, this->read_market_events_chunk_start_timestamp_ms,
                this->read_market_events_chunk_end_timestamp_ms);
        }

        for (const auto& tick : ticks) {
            Core::MarketEvent market_event;
            market_event.type = MarketEventType::AGGR_TRADE;
            market_event.symbol = dataset.symbol;
            market_event.event_time = tick.timestamp;
            market_event.aggr_trade = tick;
            market_event.exchange_name = dataset.exchange_name;
            market_event.market_type = dataset.market_type;
            market_event.price = tick.price;
            this->market_events.push_back(market_event);

            if (market_event.event_time > this->price_data_end_timestamp_ms) {
                this->price_data_end_timestamp_ms = market_event.event_time;
            }
        }
    }
}

void Core::BacktestState::read_polymarket_events() {
    const auto& polymarket = this->sim_params.market_data.polymarket;
    if (polymarket.series_slugs.empty() || polymarket.series_base_dir.empty()) {
        return;
    }

    for (const auto& series_slug : polymarket.series_slugs) {
        const auto& events = SqliteDatasets::load_polymarket_markets(
            polymarket.series_base_dir, series_slug,
            this->read_market_events_chunk_start_timestamp_ms,
            this->read_market_events_chunk_end_timestamp_ms);

        long long max_closed_market_event_time = 0;

        for (auto event_meta : events) {
            event_meta.series_slug = series_slug;

            // Store condition_id -> clob_token_ids mapping for trade resolution
            this->condition_id_to_clob_ids[event_meta.condition_id] =
                event_meta.clob_token_ids;

            // Create MARKET_CREATED event at accepting_orders_timestamp
            Core::MarketEvent created_event;
            created_event.type = MarketEventType::POLYMARKET_MARKET_CREATED;
            created_event.event_time = event_meta.accepting_orders_timestamp_ms;
            created_event.symbol = event_meta.condition_id;
            created_event.exchange_name = "polymarket";
            created_event.market_type = "spot";
            created_event.polymarket_event_meta = event_meta;
            this->market_events.push_back(created_event);

            // Create MARKET_CLOSED event at event_end_time
            Core::MarketEvent closed_event;
            closed_event.type = MarketEventType::POLYMARKET_MARKET_CLOSED;
            closed_event.event_time = event_meta.event_end_time_ms;
            closed_event.symbol = event_meta.condition_id;
            closed_event.exchange_name = "polymarket";
            closed_event.market_type = "spot";
            closed_event.polymarket_event_meta = event_meta;
            this->market_events.push_back(closed_event);

            if (closed_event.event_time > max_closed_market_event_time) {
                max_closed_market_event_time = closed_event.event_time;
            }
        }

        std::cout << "polymarket events max closed event time: "
                  << max_closed_market_event_time << std::endl;

        LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_BACKTEST_PHASE,
                 "Read " + std::to_string(events.size()) +
                     " polymarket events for series " + series_slug,
                 (nlohmann::json{}));
    }
}

void Core::BacktestState::read_polymarket_trades() {
    if (this->sim_params.skip_reading_data_with_offset_on_trades &&
        this->read_market_events_chunk_end_timestamp_ms <
            this->simulation_start_timestamp_ms) {
        return;
    }

    for (const auto& dataset : this->sim_params.market_data.trades) {
        if (dataset.exchange_name != "polymarket") continue;

        const auto& trades = ParquetUtils::load_polymarket_trades(
            dataset.path, this->read_market_events_chunk_start_timestamp_ms,
            this->read_market_events_chunk_end_timestamp_ms);

        for (auto trade : trades) {
            // Resolve clob_token_id from event metadata
            auto meta_it =
                this->condition_id_to_clob_ids.find(trade.condition_id);
            if (meta_it != this->condition_id_to_clob_ids.end() &&
                trade.outcome_index >= 0 &&
                trade.outcome_index <
                    static_cast<int>(meta_it->second.size())) {
                trade.clob_token_id = meta_it->second[trade.outcome_index];
            }

            Core::MarketEvent market_event;
            market_event.type = MarketEventType::POLYMARKET_TRADE;
            market_event.event_time = trade.timestamp;
            market_event.symbol = trade.condition_id;
            market_event.exchange_name = "polymarket";
            market_event.market_type = "spot";
            market_event.price = trade.price;
            market_event.polymarket_trade = trade;
            this->market_events.push_back(market_event);
        }

        long long max_pm_trade_ts = 0;
        for (const auto& me : this->market_events) {
            if (me.type == MarketEventType::POLYMARKET_TRADE &&
                me.event_time > max_pm_trade_ts) {
                max_pm_trade_ts = me.event_time;
            }
        }
        std::cout << "polymarket trades max event time: " << max_pm_trade_ts
                  << std::endl;

        LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_BACKTEST_PHASE,
                 "Read " + std::to_string(trades.size()) +
                     " polymarket trades for " + dataset.symbol,
                 (nlohmann::json{}));
    }
}

void Core::BacktestState::read_predictions() {
    for (const auto& dataset : this->sim_params.market_data.predictions) {
        const auto& ticks = ParquetUtils::load_prediction_ticks(
            dataset.path, dataset.model_name,
            this->read_market_events_chunk_start_timestamp_ms,
            this->read_market_events_chunk_end_timestamp_ms);

        for (auto tick : ticks) {
            tick.timestamp_ms += this->sim_params.ohlcv_delay_ms;

            Core::MarketEvent event;
            event.type = MarketEventType::PREDICTION_UPDATE;
            event.event_time = tick.timestamp_ms;
            event.backtest_prediction = tick;
            this->market_events.push_back(event);
        }

        LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_BACKTEST_PHASE,
                 "Read " + std::to_string(ticks.size()) +
                     " prediction ticks for model " + dataset.model_name,
                 (nlohmann::json{}));
    }
}

void Core::BacktestState::apply_pending_predictions() {
    auto& engine = ::Core::Engine::getInstance();
    for (auto& strategy : engine.get_strategies()) {
        if (strategy->is_disabled_now()) continue;
        if (strategy->apply_precomputed_predictions_callback) {
            strategy->apply_precomputed_predictions_callback(
                engine, *strategy, this->pending_predictions);
        }
    }
    this->pending_predictions.clear();
}

void Core::BacktestState::handle_polymarket_market_created(
    const MarketEvent& market_event) {
    if (!market_event.polymarket_event_meta.has_value()) {
        return;
    }

    const auto& meta = market_event.polymarket_event_meta.value();
    auto& engine = ::Core::Engine::getInstance();

    auto strategies = engine.get_strategies();
    for (auto& strategy : strategies) {
        bool is_polymarket_strategy = false;
        for (const auto& exchange : strategy->strategy_config.exchanges_list) {
            if (exchange.exchange_name == "polymarket") {
                is_polymarket_strategy = true;
                break;
            }
        }

        if (!is_polymarket_strategy) continue;

        if (!strategy->strategy_config.accepts_series_slug(
                meta.series_slug)) {
            continue;
        }

        if (strategy->trade_tickers.count(meta.condition_id)) continue;

        auto ticker_ptr = std::make_shared<Core::BacktestTradeTicker>(
            strategy->strategy_config.exchanges_list, meta.condition_id,
            "USDC", "OUTCOME", strategy->strategy_config.id);
        ticker_ptr->configure_from_strategy(strategy->strategy_config);
        ticker_ptr->populate_polymarket_fields_from_meta(meta);
        ticker_ptr->series_slug = meta.series_slug;

        strategy->add_trade_ticker(meta.condition_id, std::move(ticker_ptr));

        auto* ticker_raw = strategy->trade_tickers.at(meta.condition_id).get();

        auto mint_requests = strategy->get_mint_contracts(engine);
        if (!mint_requests.empty()) {
            this->mint_polymarket_contracts_from_requests(
                strategy.get(), ticker_raw, meta, market_event.event_time,
                mint_requests);
        } else {
            // this->mint_polymarket_contracts(strategy.get(), ticker_raw, meta,
            //                                 market_event.event_time);
        }
    }

    // Register event in engine-level polymarket event store
    auto pm_event = std::make_shared<Core::PolymarketActiveEvent>();
    pm_event->condition_id = meta.condition_id;
    pm_event->series_slug = meta.series_slug;
    pm_event->slug = meta.slug;
    pm_event->event_start_time_ms = meta.event_start_time_ms;
    pm_event->event_end_time_ms = meta.event_end_time_ms;
    pm_event->num_outcomes = meta.num_outcomes;
    pm_event->outcome_names = meta.outcome_names;
    pm_event->clob_token_ids = meta.clob_token_ids;
    engine.add_polymarket_event(pm_event);
}

void Core::BacktestState::settle_polymarket_trade(
    Core::TradeTicker* ticker, Core::Trade& trade, double settlement_price,
    long long settlement_time_ms) {
    mpfr::mpreal remaining = trade.open_quantity - trade.closed_quantity;
    if (remaining <= 0) return;

    Order settlement_order =
        PolymarketOrderUtils::create_polymarket_settlement_order(
            trade, settlement_price, settlement_time_ms);

    ticker->add_synthethic_order_for_event_creation(settlement_order);
    this->add_order(settlement_order);
    this->on_order_fill(settlement_order, mpfr::mpreal(settlement_price),
                        settlement_time_ms, false);
}

void Core::BacktestState::mint_polymarket_contracts(
    Core::Strategy* strategy, Core::TradeTicker* ticker,
    const PolymarketEventMeta& meta, long long timestamp) {
    auto account_id_opt =
        strategy->get_exchange_and_market_type_account_id("polymarket", "spot");
    if (!account_id_opt.has_value()) return;
    long long account_id = account_id_opt.value();

    const double mint_price = 0.50;
    const double mint_quantity = 100;

    for (const auto& clob_id : meta.clob_token_ids) {
        Order mint_order = PolymarketOrderUtils::create_polymarket_mint_order(
            meta.condition_id, clob_id, strategy->strategy_config.id,
            account_id, timestamp, mint_price, mint_quantity);

        ticker->add_synthethic_order_for_event_creation(mint_order);
        this->add_order(mint_order);
        this->on_order_fill(mint_order, mpfr::mpreal(mint_price), timestamp,
                            true);
    }
}

void Core::BacktestState::mint_polymarket_contracts_from_requests(
    Core::Strategy* strategy, Core::TradeTicker* ticker,
    const PolymarketEventMeta& meta, long long timestamp,
    const std::vector<MintContractsRequest>& requests) {
    auto account_id_opt =
        strategy->get_exchange_and_market_type_account_id("polymarket", "spot");
    if (!account_id_opt.has_value()) return;
    long long account_id = account_id_opt.value();

    std::unordered_map<std::string, double> qty_by_condition;
    for (const auto& req : requests) {
        qty_by_condition[req.condition_id] = req.quantity;
    }

    if (qty_by_condition.find(meta.condition_id) == qty_by_condition.end()) {
        return;
    }
    double mint_quantity = qty_by_condition.at(meta.condition_id);
    if (mint_quantity <= 0) return;

    const double mint_price = 0.50;

    for (const auto& clob_id : meta.clob_token_ids) {
        Order mint_order = PolymarketOrderUtils::create_polymarket_mint_order(
            meta.condition_id, clob_id, strategy->strategy_config.id,
            account_id, timestamp, mint_price, mint_quantity, 0.001);

        ticker->add_synthethic_order_for_event_creation(mint_order);
        this->add_order(mint_order);
        this->on_order_fill(mint_order, mpfr::mpreal(mint_price), timestamp,
                            true);
    }
}

void Core::BacktestState::handle_polymarket_market_closed(
    const MarketEvent& market_event) {
    if (!market_event.symbol.has_value() ||
        !market_event.polymarket_event_meta.has_value()) {
        return;
    }

    const auto& symbol = market_event.symbol.value();
    const auto& meta = market_event.polymarket_event_meta.value();
    auto& engine = ::Core::Engine::getInstance();

    // Update engine price to primary outcome price (for dashboards/logging)
    double primary_price = 0.0;
    if (!meta.outcome_prices.empty()) {
        primary_price = meta.outcome_prices[0];
    }
    engine.update_price(symbol, "polymarket", "spot", primary_price);

    auto strategies = engine.get_strategies();
    for (auto& strategy : strategies) {
        auto ticker_it = strategy->trade_tickers.find(symbol);
        if (ticker_it == strategy->trade_tickers.end()) continue;

        auto* ticker = ticker_it->second.get();
        ticker->is_closed.store(true);
        ticker->is_accepting_orders.store(false);
        ticker->is_active.store(false);

        // Cancel any resting orders
        const std::string key = "polymarket:spot:" + symbol;
        this->resting_books_.erase(key);

        // Settle all active trades — look up correct price per clob_token_id
        auto trades_copy = ticker->active_trades;

        for (auto& trade : trades_copy) {
            double trade_settlement_price = 0.0;
            if (!meta.outcome_prices.empty() && !trade.clob_token_id.empty()) {
                auto idx_it =
                    ticker->clob_id_to_outcome_index.find(trade.clob_token_id);
                if (idx_it != ticker->clob_id_to_outcome_index.end() &&
                    idx_it->second < (int)meta.outcome_prices.size()) {
                    trade_settlement_price =
                        meta.outcome_prices[idx_it->second];
                }
            } else if (!meta.outcome_prices.empty()) {
                trade_settlement_price = meta.outcome_prices[0];  // fallback
            }

            this->settle_polymarket_trade(ticker, trade, trade_settlement_price,
                                          market_event.event_time);
        }

        // Clean up inventories for this closed market
        for (const auto& account_id : this->account_ids) {
            const auto& inv_key = Utils::get_symbol_exchange_market_account_key(
                symbol, "polymarket", "spot", account_id);
            this->backtest_inventories.erase(inv_key);
        }
        engine.remove_inventories_by_symbol(symbol, "polymarket", "spot");

        // Remove the closed ticker from the strategy
        strategy->remove_trade_ticker(symbol);
    }

    engine.remove_polymarket_event(symbol);
}

void Core::BacktestState::read_market_events() {
    this->market_events.clear();
    if (this->sim_params.market_data_replay.load_mode == "streaming") {
        const std::size_t estimated_bars_per_dataset =
            static_cast<std::size_t>(
                this->sim_params.market_data_replay.chunk_duration_ms / 60000) +
            2;
        this->market_events.reserve(std::max<std::size_t>(
            1024, this->ohlcv_datasets.size() * estimated_bars_per_dataset));
    } else {
        this->market_events.reserve(10000000);
    }
    LOG_INFO(
        Core::TRADING_LOG_EVENT_TYPE_BACKTEST_PHASE,
        "Reading market events chunk start timestamp: " +
            std::to_string(this->read_market_events_chunk_start_timestamp_ms),
        (nlohmann::json{}));
    LOG_INFO(
        Core::TRADING_LOG_EVENT_TYPE_BACKTEST_PHASE,
        "Reading market events chunk end timestamp: " +
            std::to_string(this->read_market_events_chunk_end_timestamp_ms),
        (nlohmann::json{}));
    this->read_ohlcv_klines();
    this->read_funding_rates();
    this->read_trades();
    this->read_polymarket_events();
    this->read_polymarket_trades();
    this->read_predictions();
    this->read_model_fold_events();
    LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_BACKTEST_PHASE,
             "Sorting market events " +
                 std::to_string(this->market_events.size()) + " events",
             (nlohmann::json{}));
    if (this->sim_params.market_data_replay.load_mode == "streaming") {
        std::sort(
            this->market_events.begin(), this->market_events.end(),
            [](const Core::MarketEvent& left,
               const Core::MarketEvent& right) {
                return std::tie(left.event_time, left.type,
                                left.exchange_name, left.market_type,
                                left.symbol) <
                       std::tie(right.event_time, right.type,
                                right.exchange_name, right.market_type,
                                right.symbol);
            });
    } else {
        std::sort(this->market_events.begin(), this->market_events.end(),
                  [](const Core::MarketEvent& a,
                     const Core::MarketEvent& b) {
                      return a.event_time < b.event_time;
                  });
    }
    LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_BACKTEST_PHASE,
             "Sorted market events", (nlohmann::json{}));

    this->read_market_events_chunk_start_timestamp_ms =
        this->read_market_events_chunk_end_timestamp_ms;
    this->read_market_events_chunk_end_timestamp_ms =
        this->read_market_events_chunk_start_timestamp_ms +
        this->read_market_events_chunk_size;

    LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_BACKTEST_PHASE,
             "Finished reading market events on range " +
                 TimeUtils::ms_to_timestamptz(
                     this->read_market_events_chunk_start_timestamp_ms) +
                 " - " +
                 TimeUtils::ms_to_timestamptz(
                     this->read_market_events_chunk_end_timestamp_ms),
             (nlohmann::json{}));
}

void Core::BacktestState::init_state() {
    LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_BACKTEST_PHASE,
             "Initializing state...", (nlohmann::json{}));
    this->init_strategies();
}

void Core::BacktestState::bounds_on_add(SymbolRestingBook& book,
                                        const Order& order) {
    if (order.is_taker_in_simulation) {
        ++book.taker_count;
        return;
    }
    const double limit_price = order.price.toDouble();
    if (order.side == Core::OrderSide::BUY) {
        book.max_buy_limit = std::max(book.max_buy_limit, limit_price);
    } else if (order.side == Core::OrderSide::SELL) {
        book.min_sell_limit = std::min(book.min_sell_limit, limit_price);
    }
}

void Core::BacktestState::recompute_side_bounds(SymbolRestingBook& book,
                                                Core::OrderSide side) {
    if (side == Core::OrderSide::BUY) {
        book.max_buy_limit = -std::numeric_limits<double>::infinity();
        for (const auto& order : book.orders) {
            if (order.is_taker_in_simulation ||
                order.side != Core::OrderSide::BUY) {
                continue;
            }
            book.max_buy_limit =
                std::max(book.max_buy_limit, order.price.toDouble());
        }
    } else if (side == Core::OrderSide::SELL) {
        book.min_sell_limit = std::numeric_limits<double>::infinity();
        for (const auto& order : book.orders) {
            if (order.is_taker_in_simulation ||
                order.side != Core::OrderSide::SELL) {
                continue;
            }
            book.min_sell_limit =
                std::min(book.min_sell_limit, order.price.toDouble());
        }
    }
}

void Core::BacktestState::bounds_on_remove(SymbolRestingBook& book,
                                           const Order& removed) {
    if (removed.is_taker_in_simulation) {
        if (book.taker_count > 0) {
            --book.taker_count;
        }
        return;
    }
    const double limit_price = removed.price.toDouble();
    if (removed.side == Core::OrderSide::BUY &&
        removed_affects_buy_extremum(limit_price, book.max_buy_limit)) {
        this->recompute_side_bounds(book, Core::OrderSide::BUY);
    } else if (removed.side == Core::OrderSide::SELL &&
               removed_affects_sell_extremum(limit_price,
                                             book.min_sell_limit)) {
        this->recompute_side_bounds(book, Core::OrderSide::SELL);
    }
}

void Core::BacktestState::enqueue_scheduled_book_removal(
    long long effective_time_ms, const std::string& exchange_name,
    const std::string& market_type, const std::string& symbol,
    unsigned long long order_id) {
    const ScheduledBookRemoval entry{
        .effective_time_ms = effective_time_ms,
        .exchange_name = exchange_name,
        .market_type = market_type,
        .symbol = symbol,
        .order_id = order_id};
    this->active_book_removals_[order_id] = entry;
    this->scheduled_book_removals_.push(entry);
}

void Core::BacktestState::cancel_scheduled_book_removal(
    unsigned long long order_id) {
    this->active_book_removals_.erase(order_id);
}

bool Core::BacktestState::is_active_scheduled_book_removal(
    const ScheduledBookRemoval& entry) const {
    const auto it = this->active_book_removals_.find(entry.order_id);
    if (it == this->active_book_removals_.end()) {
        return false;
    }
    return it->second.effective_time_ms == entry.effective_time_ms;
}

void Core::BacktestState::process_scheduled_book_removals(
    long long event_time) {
    if (this->scheduled_book_removals_.empty()) {
        return;
    }
    if (this->scheduled_book_removals_.top().effective_time_ms > event_time) {
        return;
    }

    while (!this->scheduled_book_removals_.empty() &&
           this->scheduled_book_removals_.top().effective_time_ms <=
               event_time) {
        const ScheduledBookRemoval entry =
            this->scheduled_book_removals_.top();
        this->scheduled_book_removals_.pop();
        if (!this->is_active_scheduled_book_removal(entry)) {
            continue;
        }
        this->active_book_removals_.erase(entry.order_id);
        this->remove_order(entry.exchange_name, entry.market_type, entry.symbol,
                           entry.order_id);
        auto ack = this->deferred_cancel_acks_.extract(entry.order_id);
        if (!ack.empty()) {
            ::Core::Engine::getInstance().order_canceled_ack(ack.mapped());
        }
    }
}

void Core::BacktestState::add_order(const Order& order) {
    const std::string& key = order.get_key();

    auto& book = this->resting_books_[key];
    book.orders.push_back(order);
    this->bounds_on_add(book, order);

    if (order.expires_on_ms > 0) {
        this->enqueue_scheduled_book_removal(
            order.expires_on_ms, order.exchange_name, order.market_type,
            order.symbol, order.id);
    }
}

void Core::BacktestState::hotpath_record_ns(BacktestHotpathKind kind,
                                            uint64_t ns) {
    auto& bucket = this->hotpath_[static_cast<size_t>(kind)];
    bucket.total_ns += ns;
    ++bucket.calls;
    if (ns > bucket.max_ns) {
        bucket.max_ns = ns;
    }
}

namespace {

const char* hotpath_kind_label(Core::BacktestHotpathKind kind) {
    switch (kind) {
        case Core::BacktestHotpathKind::ScheduledRemovals:
            return "scheduled_removals";
        case Core::BacktestHotpathKind::EngineProcessMarketEvent:
            return "engine_process_market_event";
        case Core::BacktestHotpathKind::FillCheck:
            return "fill_check";
        case Core::BacktestHotpathKind::GeneratePredictions:
            return "generate_predictions";
        case Core::BacktestHotpathKind::PollStrategyStates:
            return "poll_strategy_states";
        case Core::BacktestHotpathKind::Reconcile:
            return "reconcile";
        case Core::BacktestHotpathKind::Count:
            return "count";
    }
    return "unknown";
}

double hotpath_ns_to_ms(uint64_t ns) {
    return static_cast<double>(ns) / 1'000'000.0;
}

}  // namespace

void Core::BacktestState::print_hotpath_profile(long long event_time,
                                                bool final_report) {
    const uint64_t events_since_report =
        this->hotpath_market_events_processed_ -
        this->hotpath_market_events_at_last_report_;
    const long long sim_ms_since_report =
        this->hotpath_last_report_event_time_ms_ == 0
            ? 0
            : event_time - this->hotpath_last_report_event_time_ms_;

    uint64_t window_total_ns = 0;
    for (size_t i = 0; i < static_cast<size_t>(BacktestHotpathKind::Count);
         ++i) {
        const auto& current = this->hotpath_[i];
        const auto& previous = this->hotpath_at_last_report_[i];
        window_total_ns += current.total_ns - previous.total_ns;
    }

    size_t resting_book_orders = 0;
    size_t ticker_orders = 0;
    size_t active_trades = 0;
    for (const auto& kv : this->resting_books_) {
        resting_book_orders += kv.second.orders.size();
    }
    for (const auto& strategy :
         ::Core::Engine::getInstance().get_strategies()) {
        for (const auto& [sym, ticker] : strategy->trade_tickers) {
            (void)sym;
            ticker_orders += ticker->orders.size();
            active_trades += ticker->active_trades.size();
        }
    }

    const std::string ts = TimeUtils::ms_to_timestamptz(event_time);
    std::cout << "[BACKTEST PROFILE " << ts << "] "
              << (final_report ? "final" : "window") << " sim_ms="
              << sim_ms_since_report << " events=" << events_since_report
              << " book_orders=" << resting_book_orders
              << " ticker_orders=" << ticker_orders
              << " active_trades=" << active_trades
              << " active_schedules=" << this->active_book_removals_.size()
              << " heap_size=" << this->scheduled_book_removals_.size()
              << " fill_bounds_skips="
              << (this->hotpath_fill_bounds_skips_ -
                  this->hotpath_fill_bounds_skips_at_last_report_)
              << std::endl;

    for (size_t i = 0; i < static_cast<size_t>(BacktestHotpathKind::Count);
         ++i) {
        const auto kind = static_cast<BacktestHotpathKind>(i);
        const auto& current = this->hotpath_[i];
        const auto& previous = this->hotpath_at_last_report_[i];

        const uint64_t window_ns = current.total_ns - previous.total_ns;
        const uint64_t window_calls = current.calls - previous.calls;
        if (window_calls == 0 && window_ns == 0) {
            continue;
        }

        const double avg_ms =
            window_calls > 0 ? hotpath_ns_to_ms(window_ns) /
                                   static_cast<double>(window_calls)
                             : 0.0;
        const double pct =
            window_total_ns > 0
                ? 100.0 * static_cast<double>(window_ns) /
                      static_cast<double>(window_total_ns)
                : 0.0;

        std::cout << "  " << hotpath_kind_label(kind) << ": "
                  << std::fixed << std::setprecision(1)
                  << hotpath_ns_to_ms(window_ns) << "ms"
                  << " (" << std::setprecision(1) << pct << "%)"
                  << " calls=" << window_calls << " avg=" << std::setprecision(3)
                  << avg_ms << "ms"
                  << " max=" << std::setprecision(3)
                  << hotpath_ns_to_ms(current.max_ns) << "ms" << std::endl;
    }

    this->hotpath_at_last_report_ = this->hotpath_;
    this->hotpath_market_events_at_last_report_ =
        this->hotpath_market_events_processed_;
    this->hotpath_fill_bounds_skips_at_last_report_ =
        this->hotpath_fill_bounds_skips_;
    this->hotpath_last_report_event_time_ms_ = event_time;
}

void Core::BacktestState::print_debug_state(long long event_time) {
    size_t total_active_orders = 0;
    for (const auto& kv : this->resting_books_) {
        total_active_orders += kv.second.orders.size();
    }
    size_t total_tickers = 0;
    size_t total_ticker_orders = 0;
    size_t total_ticker_trades = 0;
    for (const auto& strategy :
         ::Core::Engine::getInstance().get_strategies()) {
        for (const auto& [sym, ticker] : strategy->trade_tickers) {
            total_tickers++;
            total_ticker_orders += ticker->orders.size();
            total_ticker_trades += ticker->active_trades.size();
        }
    }

    double nav = ::Core::Engine::getInstance().get_nav();

    if (nav > this->peak_balance.toDouble()) {
        this->peak_balance = nav;
    }
    if (this->peak_balance > 0) {
        double dd = (nav - this->peak_balance.toDouble()) /
                    this->peak_balance.toDouble();
        if (dd < this->max_drawdown.toDouble()) {
            this->max_drawdown = dd;
            this->max_drawdown_ts_ms = event_time;
        }
    }

    long long total_closed = this->winning_trades + this->losing_trades;
    double win_rate =
        total_closed > 0 ? 100.0 * this->winning_trades / total_closed : 0.0;
    double avg_win =
        this->winning_trades > 0
            ? this->total_winning_pnl.toDouble() / this->winning_trades
            : 0.0;
    double avg_loss =
        this->losing_trades > 0
            ? this->total_losing_pnl.toDouble() / this->losing_trades
            : 0.0;

    std::string ts = TimeUtils::ms_to_timestamptz(event_time);

    std::cout << "[BACKTEST " << ts << "] " << "NAV: " << std::fixed
              << std::setprecision(0) << nav << " | PnL: " << std::showpos
              << std::setprecision(1) << this->total_pnl.toDouble()
              << std::noshowpos << " | Fees: " << this->total_fees.toDouble()
              << " | MaxDD: " << std::setprecision(2)
              << (this->max_drawdown.toDouble() * 100.0) << "%"
              << " | Trades: " << total_closed << " (W:" << this->winning_trades
              << " L:" << this->losing_trades << " " << std::setprecision(0)
              << win_rate << "%)" << " | AvgW: " << std::showpos
              << std::setprecision(1) << avg_win << " AvgL: " << avg_loss
              << std::noshowpos << " | Open: " << total_ticker_trades
              << " trades " << total_active_orders << " orders"
              << " | Vol: " << std::setprecision(0)
              << this->total_volume.toDouble()
              << " EV: " << std::setprecision(2)
              << (this->total_volume.toDouble() > 0
                      ? 100.0 * this->total_pnl.toDouble() /
                            this->total_volume.toDouble()
                      : 0.0)
              << "%" << std::endl;
    this->print_hotpath_profile(event_time);
}

void Core::BacktestState::mark_order_for_removal(
    const std::string& exchange_name, const std::string& market_type,
    const std::string& symbol, unsigned long long order_id,
    long long removal_timestamp) {
    const std::string& key = exchange_name + ":" + market_type + ":" + symbol;
    auto it = this->resting_books_.find(key);
    if (it == this->resting_books_.end()) return;

    for (auto& order : it->second.orders) {
        if (order.id == order_id) {
            order.removed_in_backtest_at_timestamp = removal_timestamp;
            this->enqueue_scheduled_book_removal(
                removal_timestamp, exchange_name, market_type, symbol,
                order_id);
            return;
        }
    }
}

void Core::BacktestState::remove_order(const std::string& exchange_name,
                                       const std::string& market_type,
                                       const std::string& symbol,
                                       const unsigned long long order_id) {
    const std::string& key = exchange_name + ":" + market_type + ":" + symbol;

    const auto book_it = this->resting_books_.find(key);
    if (book_it == this->resting_books_.end()) {
        return;
    }

    auto& book = book_it->second;
    auto order_it =
        std::find_if(book.orders.begin(), book.orders.end(),
                     [&order_id](const Order& o) { return o.id == order_id; });

    if (order_it != book.orders.end()) {
        this->order_queue_notional_accumulated.erase(order_id);
        this->bounds_on_remove(book, *order_it);
        book.orders.erase(order_it);
        if (book.orders.empty()) {
            this->resting_books_.erase(book_it);
        }
        this->cancel_scheduled_book_removal(order_id);
    }
}

void Core::BacktestState::on_order_fill(const Order& order,
                                        const mpfr::mpreal& price,
                                        long long timestamp,
                                        bool is_taker_execution) {
    // Default fill recording → <results_dir>/fills.jsonl. Every fill (organic
    // / mint / settlement) with condition/outcome/side/price/qty so resolved
    // PnL, markout and per-condition attribution can be reconstructed offline.
    if (!this->equity_curve_dump_path.empty()) {
        if (!this->fill_record_stream_.is_open()) {
            std::filesystem::path ep(this->equity_curve_dump_path);
            this->fill_record_stream_.open(
                (ep.parent_path() / "fills.jsonl").string(),
                std::ios::out | std::ios::trunc);
        }
        if (this->fill_record_stream_.is_open()) {
            nlohmann::json j = {
                {"ts", timestamp},
                {"condition_id", order.symbol},
                {"clob_id", order.clob_id},
                {"outcome_name", order.outcome_name},
                {"side", order.side == Core::OrderSide::BUY ? "BUY" : "SELL"},
                {"price", price.toDouble()},
                {"qty", order.quantity.toDouble()},
                {"trade_id",
                 order.trade_id.has_value()
                     ? static_cast<long long>(order.trade_id.value())
                     : -1},
                {"is_taker", is_taker_execution || order.is_taker_in_simulation},
                {"is_close", order.is_trade_close_order},
                {"is_pred_mkt", order.is_prediction_market_order},
                {"strategy_id", order.strategy_id},
            };
            this->fill_record_stream_ << j.dump() << "\n";
        }
    }

    this->deferred_cancel_acks_.erase(order.id);
    OrderFillInfo order_fill_info =
        this->create_order_fill_info(order, price, timestamp);
    this->backtest_update_inventory(order, price, is_taker_execution);
    ::Core::Engine::getInstance().order_filled_ack(order_fill_info);

    if (!is_taker_execution && !order.is_taker_in_simulation) {
        const std::string rebate_key =
            order.exchange_name + ":" + order.market_type;
        auto it = this->maker_rebate_rates.find(rebate_key);
        if (it != this->maker_rebate_rates.end()) {
            mpfr::mpreal rebate = order.quantity * order.price * it->second;
            this->total_maker_rebates += rebate;
        }
    }

    this->remove_order(order_fill_info.exchange_name,
                       order_fill_info.market_type, order_fill_info.symbol,
                       order_fill_info.order_id);
}
