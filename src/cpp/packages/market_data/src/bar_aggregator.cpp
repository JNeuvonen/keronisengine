#include "market_data/bar_aggregator.h"

#include <arrow/type_traits.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <shared_mutex>
#include <sstream>

#include "ccxt_proxy.h"

namespace {
std::atomic<long long> g_total_trades_processed{0};
std::chrono::steady_clock::time_point g_sync_start_time;

// Read resident set size in KB from /proc/self/status. Returns -1 if
// unavailable.
long long read_vm_rss_kb() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            std::istringstream iss(line);
            std::string label, unit;
            long long kb = -1;
            iss >> label >> kb >> unit;
            return kb;
        }
    }
    return -1;
}
}  // namespace
#include "chrono_utils.h"
#include "common_types/log_event_types.h"
#include "datetime_utils.h"
#include "disk_logging/log.h"
#include "market_data/bar.h"
#include "market_data/dataset_manager.h"
#include "market_data/normalized_trade.h"
#include "market_data/ohlcv_bar_meta.h"
#include "market_data/ohlcv_dataset.h"
#include "market_data/utils.h"
#include "market_making/utils.h"
#include "ohlcv_dataset_tick.h"
#include "parquet_utils.h"
#include "threadpool.h"

namespace MarketData {

BarAggregator::BarAggregator() {}

BarAggregator &BarAggregator::getInstance() {
    static BarAggregator instance;
    return instance;
}

std::vector<BarFlushInfo> BarAggregator::emit_closed_bars() {
    std::shared_lock<std::shared_mutex> lock(this->mutex);
    std::vector<BarFlushInfo> ret;
    for (const auto &bar : this->open_bars_vec) {
        if (bar->bar_close_policy.is_exchange_bar) {
            continue;
        }

        const auto &closed_bars = bar->emit_closed_bars();
        for (const auto &bar_flush_info : closed_bars) {
            ret.push_back(bar_flush_info);
        }
    }
    return ret;
}

std::vector<BarFlushInfo> BarAggregator::emit_closed_bars_live() {
    std::shared_lock<std::shared_mutex> lock(this->mutex);
    std::vector<BarFlushInfo> ret;
    for (const auto &bar : this->open_bars_vec) {
        if (bar->bar_close_policy.is_exchange_bar) {
            continue;
        }
        const auto seconds_since_last_sync = bar->get_seconds_since_last_sync();
        if (!seconds_since_last_sync.has_value()) {
            continue;
        }
        if (seconds_since_last_sync.value() < 2) {
            continue;
        }
        const auto &closed_bars = bar->emit_closed_bars();
        for (const auto &bar_flush_info : closed_bars) {
            ret.push_back(bar_flush_info);
        }
    }
    return ret;
}

std::vector<BarFlushInfo> BarAggregator::emit_closed_bars_for_rules(
    const BarClosePolicy &bar_close_policy) {
    std::shared_lock<std::shared_mutex> lock(this->mutex);
    std::vector<BarFlushInfo> ret;
    for (const auto &bar : this->open_bars_vec) {
        if (bar->bar_close_policy == bar_close_policy) {
            const auto &closed_bars = bar->emit_closed_bars();
            for (const auto &bar_flush_info : closed_bars) {
                ret.push_back(bar_flush_info);
            }
        }
    }
    return ret;
}

void BarAggregator::add_trade(const NormalizedParsedTrade &trade,
                              bool is_live_streamed) {
    std::shared_lock<std::shared_mutex> lock(this->mutex);
    const auto &symbol_exchange_market_key = trade.get_key();
    const auto &bars_it = this->open_bars_map.find(symbol_exchange_market_key);

    if (bars_it == this->open_bars_map.end()) {
        return;
    }
    for (const auto &bar : bars_it->second) {
        bar->add_trade(trade, is_live_streamed);
    }
}

void BarAggregator::add_ohlcv_dataset(
    const std::shared_ptr<OhlcvDataset> &dataset) {
    std::unique_lock<std::shared_mutex> lock(this->mutex);
    const auto &symbol_exchange_market_key =
        Core::Utils::get_symbol_exchange_market_key(dataset->meta.symbol,
                                                    dataset->meta.exchange_name,
                                                    dataset->meta.market_type);
    const auto &bars_it = this->open_bars_map.find(symbol_exchange_market_key);

    const auto &shared_ptr_bar = std::make_shared<Bar>(dataset->meta);

    if (bars_it == this->open_bars_map.end()) {
        this->open_bars_map[symbol_exchange_market_key] =
            std::vector<std::shared_ptr<Bar>>();
    }
    this->open_bars_map[symbol_exchange_market_key].push_back(shared_ptr_bar);
    this->open_bars_vec.push_back(shared_ptr_bar);
    this->unique_symbols[symbol_exchange_market_key] =
        Core::ExchangeMarketSymbol(dataset->meta.exchange_name,
                                   dataset->meta.market_type,
                                   dataset->meta.symbol);

    if (this->added_ohlcv_datasets.find(symbol_exchange_market_key) ==
        this->added_ohlcv_datasets.end()) {
        this->added_ohlcv_datasets.insert(symbol_exchange_market_key);
    }
    dataset->add_pointer_to_realtime_bar(shared_ptr_bar);
    DatasetsManager::get_instance().add_ohlcv_dataset(dataset);
}

void BarAggregator::remove_all_ohlcv_datasets() {
    std::unique_lock<std::shared_mutex> lock(this->mutex);
    this->open_bars_map.clear();
    this->open_bars_vec.clear();
    this->added_ohlcv_datasets.clear();
    this->unique_symbols.clear();
    DatasetsManager::get_instance().remove_all_ohlcv_datasets_from_state();
}

void BarAggregator::run_full_tick_level_sync() {
    if (DatasetsManager::get_instance().get_ohlcv_datasets().empty()) {
        return;
    }

    auto num_datasets =
        DatasetsManager::get_instance().get_ohlcv_datasets().size();
    LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_BAR_SYNC_PROGRESS,
             "Starting full tick-level sync for " +
                 std::to_string(num_datasets) + " datasets",
             nlohmann::json{});

    g_total_trades_processed.store(0);
    g_sync_start_time = std::chrono::steady_clock::now();

    ThreadPool pool(
        1);  // Sequential: limits peak memory to one symbol at a time
    std::vector<std::future<void>> futures;

    for (const auto &dataset :
         DatasetsManager::get_instance().get_ohlcv_datasets()) {
        futures.push_back(pool.enqueue([this, dataset]() {
            this->sync_ohlcv_dataset_historical_bars(dataset);
        }));
    }

    for (auto &future : futures) {
        future.wait();
    }

    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - g_sync_start_time)
                          .count();
    double elapsed_sec = elapsed_ms / 1000.0;
    long long total = g_total_trades_processed.load();
    double throughput = elapsed_sec > 0 ? total / elapsed_sec : 0;
    LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_BAR_SYNC_PROGRESS,
             "Sync complete: " + std::to_string(total) + " trades in " +
                 std::to_string(elapsed_sec) + "s (" +
                 std::to_string(static_cast<long long>(throughput)) +
                 " trades/sec)",
             nlohmann::json{});
}

void BarAggregator::log_bar_sync_status() {
    std::shared_lock<std::shared_mutex> lock(this->mutex);
    int total_bars = 0;
    int already_synced_bars = 0;
    for (const auto &bar : this->open_bars_vec) {
        if (bar->bar_close_policy.is_exchange_bar) {
            continue;
        }
        total_bars++;
        if (bar->get_is_synced_with_live()) {
            already_synced_bars++;
        }
    }

    if (total_bars == 0) {
        return;
    }
    LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_BAR_SYNC_PROGRESS,
             "Bar sync status: " + std::to_string(already_synced_bars) + "/" +
                 std::to_string(total_bars) + " synced",
             nlohmann::json{});
}

void BarAggregator::sync_live_streamed_bars() {
    if (this->currently_live_syncing.exchange(true)) {
        return;
    }
    const auto now_ms = DatetimeUtils::get_current_time_ms();

    log_bar_sync_status();
    try {
        std::vector<std::shared_ptr<Bar>> bars_to_sync;
        {
            std::shared_lock<std::shared_mutex> lock(this->mutex);
            for (const auto &bar : this->open_bars_vec) {
                std::lock_guard<std::mutex> bar_lock(bar->mutex);
                if (bar->is_synced_with_live ||
                    bar->first_live_stream_trade_timestamp_ms == 0 ||
                    bar->bar_close_policy.is_exchange_bar) {
                    continue;
                }
                bars_to_sync.push_back(bar);
            }
        }

        if (bars_to_sync.empty()) {
            this->currently_live_syncing.store(false);
            return;
        }

        LOG_INFO(
            Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_BAR_SYNC_PROGRESS,
            "Found " + std::to_string(bars_to_sync.size()) + " bars to sync",
            (nlohmann::json{{"phase", "bars_to_sync_found"},
                            {"num_bars", bars_to_sync.size()}}));

        ThreadPool pool(1);
        std::vector<std::future<void>> futures;

        for (const auto &bar : bars_to_sync) {
            futures.push_back(pool.enqueue([this, bar]() {
                int iters = 0;
                int zero_trades_returned_count = 0;
                long long total_trades_fetched = 0;
                auto sync_start_time = std::chrono::steady_clock::now();

                const std::string sym_key = bar->bar_meta.symbol + " " +
                                            bar->bar_meta.exchange_name + " " +
                                            bar->bar_meta.market_type;
                // Re-scan disk to refresh last_storage_tick_close_time_ms.
                // The field is captured at Bar construction (binary startup)
                // and does NOT advance as the WS path writes bars to disk
                // during normal operation. Without this rescan, every
                // post-reconnect sync invocation reads a stale anchor and
                // computes a multi-hour gap that doesn't actually exist on
                // disk.
                bar->bar_meta.read_last_tick_close_time_ms();
                const long long disk_end_ts =
                    bar->bar_meta.last_storage_tick_close_time_ms;
                const long long first_ws_ts =
                    bar->first_live_stream_trade_timestamp_ms;
                long long latest_fed_trade_ts = 0;
                const std::string prior_cursor =
                    bar->get_sync_walk_cursor_trade_id();

                // Use the dedicated REST-walk cursor — NOT bar->last_trade_id,
                // which gets clobbered by WS trades arriving before the
                // gap-fill walk even starts. Using last_trade_id meant the walk
                // would begin at the WS-current id, fetch trades already past
                // first_live_stream_trade_timestamp_ms, and instantly mark the
                // bar synced without bridging the disk-to-WS gap.
                std::string last_trade_id = prior_cursor;

                if (last_trade_id.empty()) {
                    // Bootstrap the walk from disk-end-of-bars timestamp.
                    // bar_meta.last_storage_tick_close_time_ms is captured at
                    // Bar construction from the parquet-loaded klines vector
                    // (OhlcvDataset:170-172) — it's the timestamp of the last
                    // bar persisted to disk, i.e. exactly where the gap starts.
                    long long bootstrap_ts =
                        bar->bar_meta.last_storage_tick_close_time_ms;
                    if (bootstrap_ts == 0) {
                        // No prior on-disk bars exist (fresh symbol with no
                        // historical data on this box). Nothing to gap-fill;
                        // bar will simply mark synced via the first WS trade.
                        LOG_INFO(
                            Core::
                                TRADING_LOG_EVENT_TYPE_MARKET_DATA_BAR_SYNC_PROGRESS,
                            "[bar_sync] skip " + sym_key +
                                " no prior disk bars to bridge from",
                            (nlohmann::json{{"phase", "skipped_no_disk"},
                                            {"symbol", bar->bar_meta.symbol}}));
                        return;
                    }
                    auto initial_trades =
                        CcxtProxy::instance().get_historical_trades(
                            bar->bar_meta.exchange_name,
                            bar->bar_meta.market_type, bar->bar_meta.symbol,
                            bootstrap_ts,
                            RateLimiter::RequestClass::BAR_BACKFILL);
                    if (!initial_trades.empty()) {
                        last_trade_id = initial_trades.back().trade_id;
                        bar->set_sync_walk_cursor_trade_id(last_trade_id);
                        for (const auto &trade : initial_trades) {
                            bar->add_trade(trade, false);
                        }
                        total_trades_fetched += initial_trades.size();
                        latest_fed_trade_ts = initial_trades.back().timestamp;
                        LOG_INFO(
                            Core::
                                TRADING_LOG_EVENT_TYPE_MARKET_DATA_BAR_SYNC_PROGRESS,
                            "[bar_sync] bootstrap " + sym_key + " got " +
                                std::to_string(initial_trades.size()) +
                                " trades, first_ts=" +
                                std::to_string(
                                    initial_trades.front().timestamp) +
                                " last_ts=" +
                                std::to_string(latest_fed_trade_ts) +
                                " remaining_ms=" +
                                std::to_string(first_ws_ts -
                                               latest_fed_trade_ts),
                            (nlohmann::json{
                                {"phase", "bootstrapped"},
                                {"symbol", bar->bar_meta.symbol},
                                {"trades_in_bootstrap", initial_trades.size()},
                                {"first_fetched_ts_ms",
                                 initial_trades.front().timestamp},
                                {"last_fetched_ts_ms", latest_fed_trade_ts},
                                {"remaining_ms",
                                 first_ws_ts - latest_fed_trade_ts}}));
                    }
                }

                if (last_trade_id.empty()) {
                    return;
                }

                while (!bar->get_is_synced_with_live()) {
                    std::vector<MarketData::NormalizedParsedTrade> trades =
                        CcxtProxy::instance().get_historical_trades_by_id(
                            bar->bar_meta.exchange_name,
                            bar->bar_meta.market_type, bar->bar_meta.symbol,
                            last_trade_id,
                            RateLimiter::RequestClass::BAR_BACKFILL);

                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(1000));

                    total_trades_fetched += trades.size();
                    if (trades.size() == 0) {
                        LOG_INFO(
                            Core::
                                TRADING_LOG_EVENT_TYPE_MARKET_DATA_BAR_SYNC_PROGRESS,
                            "[bar_sync] zero trades " + sym_key +
                                " at cursor=" + last_trade_id + " (iter=" +
                                std::to_string(iters) + ", consec_zero=" +
                                std::to_string(zero_trades_returned_count + 1) +
                                ")",
                            (nlohmann::json{{"phase", "zero_trades"},
                                            {"symbol", bar->bar_meta.symbol},
                                            {"cursor", last_trade_id},
                                            {"iter", iters},
                                            {"consec_zero",
                                             zero_trades_returned_count + 1}}));
                        zero_trades_returned_count++;
                        if (zero_trades_returned_count > 2) {
                            break;
                        }
                        continue;
                    }

                    if (!trades.empty()) {
                        last_trade_id = trades.back().trade_id;
                        latest_fed_trade_ts = trades.back().timestamp;
                        // Persist the walk cursor on the bar so the next
                        // sync_live_streamed_bars invocation resumes from
                        // here, not from a WS-tainted last_trade_id.
                        bar->set_sync_walk_cursor_trade_id(last_trade_id);
                    }

                    for (const auto &trade : trades) {
                        bar->add_trade(trade, false);
                    }

                    // Periodic progress log — every 25 iterations (~12.5k
                    // trades, ~2.5s of wall clock). Lets the operator see the
                    // walk is making forward progress and how much remains.
                    if (iters > 0 && iters % 25 == 0) {
                        long long remaining_ms =
                            first_ws_ts > latest_fed_trade_ts
                                ? first_ws_ts - latest_fed_trade_ts
                                : 0;
                        LOG_INFO(
                            Core::
                                TRADING_LOG_EVENT_TYPE_MARKET_DATA_BAR_SYNC_PROGRESS,
                            "[bar_sync] progress " + sym_key +
                                " iter=" + std::to_string(iters) + " trades=" +
                                std::to_string(total_trades_fetched) +
                                " latest_ts=" +
                                std::to_string(latest_fed_trade_ts) +
                                " remaining_ms=" + std::to_string(remaining_ms),
                            (nlohmann::json{
                                {"phase", "progress"},
                                {"symbol", bar->bar_meta.symbol},
                                {"iter", iters},
                                {"total_trades", total_trades_fetched},
                                {"latest_fed_ts_ms", latest_fed_trade_ts},
                                {"remaining_ms", remaining_ms}}));
                    }

                    auto closed_bars = bar->emit_closed_bars();
                    std::sort(closed_bars.begin(), closed_bars.end(),
                              [](const BarFlushInfo &a, const BarFlushInfo &b) {
                                  return a.tick.kline_close_time <
                                         b.tick.kline_close_time;
                              });
                    for (const auto &bar_flush_info : closed_bars) {
                        DatasetsManager::get_instance().add_ohlcv_tick(
                            bar_flush_info);
                    }

                    // Pacing now lives in CcxtProxy via
                    // RateLimiter::should_throttle.
                    ++iters;
                    if (iters == 100) {
                        auto now_ms = DatetimeUtils::get_current_time_ms();
                        auto lag_ms = now_ms - bar->last_trade_timestamp_ms;
                        double lag_minutes =
                            static_cast<double>(lag_ms) / 60000.0;
                        LOG_INFO(
                            Core::
                                TRADING_LOG_EVENT_TYPE_MARKET_DATA_BAR_SYNC_PROGRESS,
                            "Temporarily breaking live stream sync for " +
                                bar->bar_meta.symbol + " " +
                                bar->bar_meta.exchange_name + " " +
                                bar->bar_meta.market_type + " " +
                                bar->bar_meta.get_bar_type_str() + " after " +
                                std::to_string(iters) + " iterations" +
                                ", lag: " + std::to_string(lag_minutes) +
                                " min" + " (last trade: " +
                                TimeUtils::ms_to_timestamptz(
                                    bar->last_trade_timestamp_ms) +
                                ")",
                            nlohmann::json{});
                        break;
                    }
                }

                auto sync_end_time = std::chrono::steady_clock::now();
                auto sync_duration =
                    std::chrono::duration_cast<std::chrono::seconds>(
                        sync_end_time - sync_start_time)
                        .count();
                double trades_per_second =
                    static_cast<double>(total_trades_fetched) /
                    (sync_duration > 0 ? sync_duration : 1);

                const bool ok = bar->get_is_synced_with_live();
                const long long bridged_ms =
                    latest_fed_trade_ts > disk_end_ts && disk_end_ts > 0
                        ? latest_fed_trade_ts - disk_end_ts
                        : 0;
                LOG_INFO(
                    Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_BAR_SYNC_PROGRESS,
                    std::string("[bar_sync] ") +
                        (ok ? "SYNCED " : "STOPPED unsynced ") + sym_key +
                        " iters=" + std::to_string(iters) +
                        " trades=" + std::to_string(total_trades_fetched) +
                        " bridged_ms=" + std::to_string(bridged_ms) +
                        " bridged_h=" + std::to_string(bridged_ms / 3600000.0) +
                        " duration_s=" + std::to_string(sync_duration) +
                        " rate=" + std::to_string(trades_per_second) +
                        " trades/s" + " latest_fed_ts=" +
                        std::to_string(latest_fed_trade_ts) +
                        " first_ws_ts=" + std::to_string(first_ws_ts) +
                        " final_cursor=" + last_trade_id,
                    (nlohmann::json{
                        {"phase", ok ? "synced" : "stopped_unsynced"},
                        {"symbol", bar->bar_meta.symbol},
                        {"iters", iters},
                        {"total_trades", total_trades_fetched},
                        {"bridged_ms", bridged_ms},
                        {"duration_s", sync_duration},
                        {"trades_per_second", trades_per_second},
                        {"latest_fed_ts_ms", latest_fed_trade_ts},
                        {"first_ws_ts_ms", first_ws_ts},
                        {"final_cursor", last_trade_id}}));
            }));
        }

        for (auto &future : futures) {
            future.wait();
        }

        log_bar_sync_status();
    } catch (...) {
        this->currently_live_syncing.store(false);
    }
    this->currently_live_syncing.store(false);
}

void BarAggregator::sync_ohlcv_dataset_historical_bars(
    const std::shared_ptr<OhlcvDataset> &dataset) {
    dataset->meta.read_last_tick_close_time_ms();
    auto start_time = dataset->meta.last_storage_tick_close_time_ms != 0
                          ? ChronoUtils::make_utc_timepoint_from_epoch_ms(
                                dataset->meta.last_storage_tick_close_time_ms)
                          : ChronoUtils::make_timepoint(2021, 1, 1, 0, 0, 0);
    // CREATE_BARS_START=YYYY-MM-DD forces the backfill origin, so a targeted
    // rebuild doesn't replay years of history it doesn't need. Opt-in only,
    // and it leaves a gap below the given date by design.
    if (const char *from = std::getenv("CREATE_BARS_START"); from && *from) {
        int y = 0, mo = 0, d = 0;
        if (std::sscanf(from, "%d-%d-%d", &y, &mo, &d) == 3) {
            start_time = ChronoUtils::make_timepoint(y, mo, d, 0, 0, 0);
        }
    }

    auto end_time = ChronoUtils::make_now_utc_timepoint();

    // Dedup window must match the per-iteration lookback (1 day — see the
    // `current_start -= 24h` line below). Each iteration covers ~3 days of
    // trades but only the first 24h overlap with the prior iteration's tail;
    // dedup is needed only for that overlap. Once we've processed past
    // `current_start + 1 day` the prior-iter IDs in the set are no longer
    // reachable as duplicates and can be cleared — the set then rebuilds
    // with the current iter's tail, which is what the *next* iter needs for
    // its overlap. Prior value was 5 days, which exceeded the iteration
    // window and made the clear unreachable; the set grew unbounded across
    // iterations (confirmed: 43M entries / 3.9 GB RSS at iter 86 of a long
    // backfill run, growing ~750k entries / ~62 MB per iter). Bound after
    // fix: ~1 day of trade IDs at any instant.
    const int DEDUP_CLEAR_DAYS = 1;
    const long long DEDUP_CLEAR_SECONDS = DEDUP_CLEAR_DAYS * 24 * 60 * 60;

    std::unordered_set<std::string> processed_trade_ids;
    int instr_iter_count = 0;
    long long instr_rss_start_kb = read_vm_rss_kb();

    long long trade_count = 0;     // Moved outside the loop
    long long skipped_trades = 0;  // Track skipped trades for debugging

    auto current_start = start_time;
    const auto trades_base_dir = MarketData::get_raw_trade_ticks_base_dir(
        dataset->meta.symbol, dataset->meta.exchange_name,
        dataset->meta.market_type);

    const std::string sym = dataset->meta.symbol;

    while (current_start < end_time) {
        auto time_t_start = std::chrono::system_clock::to_time_t(current_start);
        std::tm tm = ChronoUtils::gmtime_safe(time_t_start);

        tm.tm_mday += 2;
        std::time_t time_t_end = timegm(&tm);
        auto current_end = std::chrono::system_clock::from_time_t(time_t_end);

        current_start -= std::chrono::hours(24);

        LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_BAR_SYNC_PROGRESS,
                 "[" + sym + "] Syncing period: " +
                     ChronoUtils::chrono_timepoint_to_string(current_start) +
                     " to " +
                     ChronoUtils::chrono_timepoint_to_string(current_end),
                 nlohmann::json{});

        auto t0 = std::chrono::steady_clock::now();
        std::vector<MarketData::NormalizedParsedTrade> trades;
        {
            auto trades_arrow = ParquetUtils::scan_partitions(
                trades_base_dir, current_start, current_end);
            auto t1_inner = std::chrono::steady_clock::now();

            if (!trades_arrow) {
                current_start = current_end;
                continue;
            }

            trades = MarketData::NormalizedParsedTrade::from_arrow_table(
                trades_arrow, dataset->meta.symbol, dataset->meta.exchange_name,
                dataset->meta.market_type);
            // trades_arrow freed here — Arrow table memory released
        }
        auto t2 = std::chrono::steady_clock::now();

        if (trades.empty()) {
            if (trade_count > 0) {
                break;
            }
            current_start = current_end;
            continue;
        }

        bool is_cleared_dedup_set = false;
        dataset->meta.read_last_tick_close_time_ms();

        auto realtime_bar = dataset->get_realtime_bar();
        if (!realtime_bar) {
            current_start = current_end;
            continue;
        }

        for (const auto &trade : trades) {
            if (trade.timestamp <=
                dataset->meta.last_storage_tick_close_time_ms) {
                continue;
            }

            if (processed_trade_ids.find(trade.trade_id) !=
                processed_trade_ids.end()) {
                skipped_trades++;
                continue;
            }

            if (!is_cleared_dedup_set &&
                trade.timestamp >
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        current_start.time_since_epoch())
                            .count() +
                        DEDUP_CLEAR_SECONDS * 1000) {
                processed_trade_ids.clear();
                is_cleared_dedup_set = true;
            }

            processed_trade_ids.insert(trade.trade_id);
            realtime_bar->_add_trade_lockless(trade);
            trade_count++;

            if (trade_count % 25000 == 0) {
                auto closed_bars = realtime_bar->emit_closed_bars_lockless();
                std::sort(closed_bars.begin(), closed_bars.end(),
                          [](const BarFlushInfo &a, const BarFlushInfo &b) {
                              return a.tick.kline_close_time <
                                     b.tick.kline_close_time;
                          });
                for (const auto &bar_flush_info : closed_bars) {
                    dataset->add_new_kline(bar_flush_info.tick);
                }
            }
        }
        auto t3 = std::chrono::steady_clock::now();

        auto closed_bars = realtime_bar->emit_closed_bars_lockless();
        std::sort(closed_bars.begin(), closed_bars.end(),
                  [](const BarFlushInfo &a, const BarFlushInfo &b) {
                      return a.tick.kline_close_time < b.tick.kline_close_time;
                  });
        for (const auto &bar_flush_info : closed_bars) {
            dataset->add_new_kline(bar_flush_info.tick);
        }
        auto t4 = std::chrono::steady_clock::now();

        dataset->sync_to_disk();
        dataset->clear_klines_from_memory();
        auto t5 = std::chrono::steady_clock::now();

        g_total_trades_processed.fetch_add(trades.size());

        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              t5 - g_sync_start_time)
                              .count();
        double elapsed_sec = elapsed_ms / 1000.0;
        long long total = g_total_trades_processed.load();
        double throughput = elapsed_sec > 0 ? total / elapsed_sec : 0;

        auto ms = [](auto a, auto b) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(b - a)
                .count();
        };
        instr_iter_count++;
        long long instr_rss_kb = read_vm_rss_kb();
        long long instr_rss_delta_kb =
            (instr_rss_start_kb >= 0 && instr_rss_kb >= 0)
                ? instr_rss_kb - instr_rss_start_kb
                : 0;
        size_t instr_dedup_size = processed_trade_ids.size();

        LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_BAR_SYNC_PROGRESS,
                 "[" + sym + "] " + std::to_string(trades.size()) +
                     " trades | load:" + std::to_string(ms(t0, t2)) +
                     "ms process:" + std::to_string(ms(t2, t3)) +
                     "ms emit:" + std::to_string(ms(t3, t4)) +
                     "ms disk:" + std::to_string(ms(t4, t5)) +
                     "ms | total:" + std::to_string(total) + " throughput:" +
                     std::to_string(static_cast<long long>(throughput)) +
                     " trades/sec | MEMINSTR iter=" +
                     std::to_string(instr_iter_count) +
                     " rss_kb=" + std::to_string(instr_rss_kb) +
                     " rss_delta_kb=" + std::to_string(instr_rss_delta_kb) +
                     " dedup_set_size=" + std::to_string(instr_dedup_size),
                 nlohmann::json{});

        current_start = current_end;
    }
    dataset->rm_unused_klines(10);
    LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_BAR_SYNC_PROGRESS,
             "[" + sym + "] Sync complete: " + std::to_string(trade_count) +
                 " trades processed, " + std::to_string(skipped_trades) +
                 " skipped",
             nlohmann::json{});
}

void BarAggregator::live_trade_stream_disconnected_reset(
    const std::string &exchange_name, const std::string &market_type) {
    std::shared_lock<std::shared_mutex> lock(this->mutex);
    for (const auto &bar : this->open_bars_vec) {
        if (bar->bar_meta.exchange_name == exchange_name &&
            bar->bar_meta.market_type == market_type) {
            bar->stream_disconnected_ack();
        }
    }
}

void BarAggregator::live_trade_stream_disconnected_reset(
    const std::string &exchange_name, const std::string &market_type,
    const std::string &symbol) {
    std::shared_lock<std::shared_mutex> lock(this->mutex);
    for (const auto &bar : this->open_bars_vec) {
        if (bar->bar_meta.exchange_name == exchange_name &&
            bar->bar_meta.market_type == market_type &&
            bar->bar_meta.symbol == symbol) {
            bar->stream_disconnected_ack();
        }
    }
}

void BarAggregator::set_reread_klines_from_disk_config(
    bool reread_klines_from_disk_config) {
    this->reread_klines_from_disk_config = reread_klines_from_disk_config;
}

void BarAggregator::reread_klines_from_disk() {
    if (this->reread_klines_from_disk_config) {
        if (std::chrono::steady_clock::now() -
                this->last_reread_klines_from_disk_time >
            std::chrono::hours(1)) {
            this->last_reread_klines_from_disk_time =
                std::chrono::steady_clock::now();
            for (const auto &dataset :
                 DatasetsManager::get_instance().get_ohlcv_datasets()) {
                dataset->reread_from_disk();
            }
        }
    }
}

}  // namespace MarketData
