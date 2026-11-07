#include "market_data/ohlcv_dataset.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>
#include <unordered_set>

#include "ccxt_proxy.h"
#include "common_types/log_event_types.h"
#include "datetime_utils.h"
#include "disk_logging/log.h"
#include "market_data/utils.h"
#include "market_making/engine.h"
#include "ohlcv_dataset_tick.h"
#include "parquet_utils.h"

static std::string convert_interval_ms_to_str(int interval_ms) {
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

namespace MarketData {

static std::mutex g_added_klines_mutex;
static std::unordered_set<std::string> g_added_klines_symbols;
static std::chrono::steady_clock::time_point g_last_klines_dump_time =
    std::chrono::steady_clock::now();

std::string get_bar_base_directory_path(
    const std::string &exchange_name, const std::string &market_type,
    const BarClosePolicy &bar_close_policy) {
    return "/derived/bars/" + exchange_name + "/" + market_type + "/" +
           bar_close_policy.get_bar_type_and_close_threshold();
}

std::string OhlcvBarMeta::get_policy_key() const {
    return this->bar_close_policy.to_string();
}

std::string OhlcvBarMeta::get_bar_type_str() const {
    if (this->bar_close_policy.is_time_bar) {
        return "time";
    }

    if (this->bar_close_policy.is_dollar_bar) {
        return "dollar_vol";
    }
    if (this->bar_close_policy.is_tick_count_bar) {
        return "tick_count";
    }
    if (this->bar_close_policy.is_exchange_bar) {
        return "exchange";
    }
    return "";
}

std::string OhlcvBarMeta::get_symbol_base_directory_path() const {
    return MarketData::get_derived_bars_base_dir(symbol, exchange_name,
                                                 market_type, bar_close_policy);
}

std::string OhlcvBarMeta::get_parquets_path() const {
    return get_symbol_base_directory_path() + "/parquets";
}

bool has_only_current_month_partition(const OhlcvBarMeta &meta) {
    namespace fs = std::filesystem;
    // Runs once per dataset inside the coverage sweep (~59k of them), so it
    // must never throw: "cannot tell" has to read as "not a stub" rather than
    // taking down the report that exists to surface bad data.
    fs::path parquets;
    try {
        parquets = fs::path(meta.get_parquets_path());
    } catch (...) {
        return false;
    }
    std::error_code ec;
    if (!fs::is_directory(parquets, ec)) return false;

    const auto now =
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm = ChronoUtils::gmtime_safe(now);
    char current[64];
    std::snprintf(current, sizeof(current), "year=%04d/month=%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1);

    int month_dirs = 0;
    bool saw_current = false;
    for (const auto &year_entry : fs::directory_iterator(parquets, ec)) {
        if (!year_entry.is_directory(ec)) continue;
        for (const auto &month_entry :
             fs::directory_iterator(year_entry.path(), ec)) {
            if (!month_entry.is_directory(ec)) continue;
            ++month_dirs;
            if (month_dirs > 1) return false;  // more than one month => real
            const std::string suffix = year_entry.path().filename().string() +
                                       "/" +
                                       month_entry.path().filename().string();
            saw_current = (suffix == current);
        }
    }
    return month_dirs == 1 && saw_current;
}

// A polygon backfill walks 60-day windows from 2020 and takes many runs to
// reach the present, so it is routinely interrupted part-way. 283 1m datasets
// ended up holding 2020 fragments plus one streamed month because "fetched
// something" was treated as "ready to stream". Crypto keeps the old rule: those
// datasets are continuous and already current, and tightening them would risk
// live trading for no benefit here.
constexpr long long kStreamIngestMaxLagMs = 7LL * 24 * 60 * 60 * 1000;

bool backfill_allows_stream_ingest(const OhlcvBarMeta &meta,
                                   long long bars_fetched, long long head_ms,
                                   long long now_ms) {
    if (has_only_current_month_partition(meta)) return false;
    if (meta.exchange_name == "polygon") {
        if (head_ms <= 0) return false;
        return (now_ms - head_ms) <= kStreamIngestMaxLagMs;
    }
    if (bars_fetched > 0) return true;
    return head_ms > 0;
}

bool OhlcvBarMeta::shares_same_symbol_exchange_market(
    const OhlcvBarMeta &other) const {
    return this->symbol == other.symbol &&
           this->exchange_name == other.exchange_name &&
           this->market_type == other.market_type;
}

void OhlcvBarMeta::read_last_tick_close_time_ms() {
    std::string path = get_parquets_path();

    long long latest_tick_close_time_ms =
        ParquetUtils::get_latest_row_timestamp(path, "kline_close_time_ms");
    this->last_storage_tick_close_time_ms = latest_tick_close_time_ms;
}

std::string OhlcvBarMeta::to_string() const {
    return "OhlcvBarMeta(symbol: " + symbol +
           ", exchange_name: " + exchange_name +
           ", market_type: " + market_type +
           ", bar_close_policy: " + bar_close_policy.to_string() +
           ", last_storage_tick_close_time_ms: " +
           std::to_string(last_storage_tick_close_time_ms) +
           ", last_storage_tick_id: " + last_storage_tick_id + ")";
}

OhlcvDataset::OhlcvDataset(const OhlcvBarMeta &input,
                           std::chrono::system_clock::time_point start_time,
                           bool read_data, bool can_fetch_from_proxy,
                           long long max_num_required_klines_after_init_sync)
    : meta(input),
      identifier(input.get_symbol_base_directory_path()),
      dataset_mutex(std::make_unique<std::shared_mutex>()),
      read_start_time(start_time),
      read_data_on_init(read_data),
      can_fetch_from_proxy(can_fetch_from_proxy),
      max_num_required_klines_after_init_sync(
          max_num_required_klines_after_init_sync) {
    const char *env = std::getenv("DATA_DIR_ROOT");
    const char *is_running_live_trading_env =
        std::getenv("IS_RUNNING_LIVE_TRADING");
    if (!env) {
        throw std::invalid_argument("DATA_DIR_ROOT is not set");
    }
    if (is_running_live_trading_env) {
        is_running_live_trading =
            std::string(is_running_live_trading_env) == "1";
    }
    if (input.exchange_name == "binance") {
        dataset_type = OHLCV_DATASET_TYPE_BINANCE;
        is_continuous_dataset = true;
    } else if (input.exchange_name == "polygon") {
        dataset_type = OHLCV_DATASET_TYPE_POLYGON;
        is_continuous_dataset = false;
        // Polygon flat files publish bars across all hours (pre-market
        // 04:00-09:30 ET + after-hours 16:00-20:00 ET). We persist the
        // full session — strategies that only trade RTH apply
        // is_time_ms_within_real_trading_hours at read time.
        include_outside_rth_ticks = true;
    } else {
        dataset_type = OHLCV_DATASET_TYPE_CRYPTO;
        is_continuous_dataset = true;
    }
    data_library_root = env;
    create_dataset_dir();
    init_state();
    meta.last_storage_tick_close_time_ms =
        klines.size() > 0 ? klines.back().kline_close_time : 0;

    if (meta.market_type == "spot" && meta.bar_close_policy.time_ms == 60000 &&
        meta.exchange_name == "binance") {
        this->rm_unused_klines(1600);
    }

    if (meta.market_type == "swap" && meta.bar_close_policy.time_ms == 900000 &&
        meta.exchange_name == "binance") {
        this->rm_unused_klines(std::max(
            6800LL, MarketData::get_min_klines_to_keep_after_init(meta)));
    }

    if (meta.market_type == "spot" && meta.bar_close_policy.time_ms == 900000 &&
        meta.exchange_name == "binance") {
        this->rm_unused_klines(std::max(
            710LL, MarketData::get_min_klines_to_keep_after_init(meta)));
    }

    if (meta.market_type == "spot" && meta.bar_close_policy.time_ms == 5000 &&
        meta.exchange_name == "gateio") {
        this->rm_unused_klines(18000);
    }

    if (meta.market_type == "spot" && meta.bar_close_policy.time_ms == 100 &&
        meta.exchange_name == "binance") {
        this->rm_unused_klines(200000);
    }
}

void OhlcvDataset::add_new_kline(const MarketData::OhlcvDatasetTick &tick,
                                 bool is_called_from_init_fetch) {
    std::unique_lock<std::shared_mutex> lock(*dataset_mutex);

    if (!is_called_from_init_fetch && !init_fetch_done) {
        return;
    }

    // Drop outside-RTH ticks only when the venue has trading sessions AND
    // this dataset wasn't constructed to keep them. Continuous-trading
    // venues (crypto) have no RTH; the filter is meaningless there.
    if (!is_continuous_dataset && !include_outside_rth_ticks) {
        if (!is_time_ms_within_real_trading_hours(tick.kline_open_time)) {
            return;
        }
    }

    auto now = std::chrono::system_clock::now();
    auto current_time_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch())
            .count();

    if (tick.kline_close_time > current_time_ms &&
        meta.bar_close_policy.is_exchange_bar) {
        return;
    }

    if (klines.size() > 0) {
        const auto &prev_tick = klines.back();
        if ((meta.bar_close_policy.is_exchange_bar &&
             prev_tick.kline_close_time >= tick.kline_close_time) ||
            prev_tick.kline_close_time > tick.kline_close_time) {
            return;
        }
    }

    klines.push_back(tick);
    kline_open_time_ms.push_back(tick.kline_open_time);
    open_prices.push_back(tick.open_price);
    high_prices.push_back(tick.high_price);
    low_prices.push_back(tick.low_price);
    close_prices.push_back(tick.close_price);
    volumes.push_back(tick.volume);
    kline_close_time_ms.push_back(tick.kline_close_time);
    quote_asset_volumes.push_back(tick.quote_asset_volume);
    number_of_trades.push_back(tick.number_of_trades);
    taker_buy_base_volumes.push_back(tick.taker_buy_base_volume);
    taker_buy_quote_volumes.push_back(tick.taker_buy_quote_volume);
    disk_dirty_.store(true, std::memory_order_relaxed);
    dirty_months_.insert(
        ParquetUtils::ohlcv_partition_month_key(tick.kline_open_time));
}

void OhlcvDataset::create_dataset_dir() {
    std::string path = get_parquets_path();
    std::filesystem::create_directories(path);
}

std::string OhlcvDataset::get_base_directory_path() {
    return meta.get_symbol_base_directory_path();
}

std::string OhlcvDataset::get_parquets_path() {
    return get_base_directory_path() + "/parquets";
}

OhlcvDataset::~OhlcvDataset() = default;

}  // namespace MarketData

bool MarketData::OhlcvDataset::is_gapped() {
    std::shared_lock<std::shared_mutex> lock(*dataset_mutex);
    if (klines.size() == 0) {
        return false;
    }
    size_t klines_size = klines.size();
    for (size_t i = 1; i < klines_size; i++) {
        if (klines[i].kline_open_time - klines[i - 1].kline_close_time >
            meta.bar_close_policy.time_ms) {
            return true;
        }
    }
    return false;
}

bool MarketData::OhlcvDataset::is_missing_klines() {
    std::shared_lock<std::shared_mutex> lock(*dataset_mutex);

    if (meta.bar_close_policy.is_time_bar) {
        if (klines.empty()) {
            return true;
        }

        long long latest_fully_completed_kline_open_ms =
            get_latest_fully_completed_kline_open_ms(
                meta.bar_close_policy.time_ms);

        if (klines.back().kline_open_time ==
            latest_fully_completed_kline_open_ms) {
            return false;
        }

        long long current_time = DatetimeUtils::get_current_time_ms();

        if (current_time - latest_fully_completed_kline_open_ms > 250) {
            return true;
        }
    }
    return false;
}

void MarketData::OhlcvDataset::dedup_and_sort_klines(bool require_lock) {
    auto perform_dedup_and_sort = [this]() {
        std::sort(klines.begin(), klines.end(),
                  [](const MarketData::OhlcvDatasetTick &a,
                     const MarketData::OhlcvDatasetTick &b) {
                      return a.kline_open_time < b.kline_open_time;
                  });
        klines.erase(std::unique(klines.begin(), klines.end(),
                                 [](const MarketData::OhlcvDatasetTick &a,
                                    const MarketData::OhlcvDatasetTick &b) {
                                     return a.kline_open_time ==
                                            b.kline_open_time;
                                 }),
                     klines.end());
    };

    if (require_lock) {
        std::unique_lock<std::shared_mutex> lock(*dataset_mutex);
        perform_dedup_and_sort();
    } else {
        perform_dedup_and_sort();
    }
}

/**
 * @brief Initialize the dataset state.
 *
 * Loads klines from disk, dedups and sorts them, called during constructor
 * and when re-reading from disk.
 */
void MarketData::OhlcvDataset::init_state() {
    if (read_data_on_init) {
        this->load_klines_from_disk();
        dedup_and_sort_klines(true);
        const long long bar_type_floor =
            MarketData::get_min_klines_to_keep_after_init(this->meta);
        long long effective_cap =
            std::max(max_num_required_klines_after_init_sync, bar_type_floor);
        // A venue ceiling overrides the caller's cap downwards — it exists
        // because the venue's universe is too wide to hold at that cap.
        const long long venue_ceiling =
            MarketData::get_max_klines_to_keep(this->meta);
        if (venue_ceiling > 0) {
            effective_cap = venue_ceiling;
        }
        if (effective_cap > 0) {
            this->rm_unused_klines(effective_cap);
        }
    }
}

// Klines allowed to accumulate mid-backfill before a flush+trim. ~16 MB per
// in-flight dataset, so a wide fetch pool stays well inside budget.
static constexpr long long kFlushBacklogKlines = 100000;

MarketData::OhlcvDataset::BackfillOutcome
MarketData::OhlcvDataset::init_sync_bars() {
    if (!can_fetch_from_proxy) {
        return BackfillOutcome::Skipped;
    }
    const long long venue_ceiling =
        MarketData::get_max_klines_to_keep(this->meta);
    int iters = 0;

    // History comes from the archive downloader (data_library.binance_swap_bars),
    // not REST. The stream still fills the tail, and add_new_kline drops every
    // live bar until init_fetch_done is set.
    if (meta.market_type == "swap" && meta.bar_close_policy.time_ms == 60000 &&
        meta.exchange_name == "binance") {
        this->mark_init_sync_done();
        return BackfillOutcome::Skipped;
    }

    auto last_kline_close_time_ms = this->get_latest_bar_close_time_ms();

    // Whether this dataset already holds history, sampled BEFORE the fetch.
    // A dataset that is merely up to date fetches zero new bars and must still
    // end up open; only one with no history at all and nothing retrieved stays
    // shut. See the init_fetch_done assignment at the end for why.
    bool had_history_before_fetch = last_kline_close_time_ms.has_value();
    if (!had_history_before_fetch) {
        this->meta.read_last_tick_close_time_ms();
        had_history_before_fetch =
            this->meta.last_storage_tick_close_time_ms > 0;
    }

    long long bars_fetched = 0;
    bool fetch_failed = false;
    // Mirrors POLYGON_BARS_WINDOW_DAYS in cpp_live_proxy's historical_data.py.
    // Stepping by less than the server window just wastes calls; more skips
    // data.
    constexpr long long kPolygonFetchWindowMs = 60LL * 24 * 60 * 60 * 1000;
    const bool is_windowed_polygon_fetch =
        meta.exchange_name == "polygon" &&
        meta.bar_close_policy.time_ms != 86400000LL;
    const long long now_ms_for_window = DatetimeUtils::get_current_time_ms();
    long long empty_windows_stepped = 0;

    if (!last_kline_close_time_ms.has_value()) {
        this->meta.read_last_tick_close_time_ms();
        last_kline_close_time_ms = this->meta.last_storage_tick_close_time_ms;

        if (this->meta.exchange_name == "bybit" ||
            this->meta.exchange_name == "bitget") {
            last_kline_close_time_ms = 1577836800000;
        }
        if (this->meta.exchange_name == "polygon") {
            if (this->meta.bar_close_policy.time_ms == 86400000) {
                last_kline_close_time_ms = 0;
            } else {
                last_kline_close_time_ms = 1577836800000;
            }
        }

        if (this->meta.exchange_name == "binance" &&
            this->meta.market_type == "spot") {
            last_kline_close_time_ms = 1735689600000;
        }
    }

    auto prev_last_kline_close_time_ms = last_kline_close_time_ms;

    while (true) {
        bool page_failed = false;
        std::vector<MarketData::OhlcvDatasetTick> ticks =
            CcxtProxy::instance().get_ohlcv_dataset_ticks(
                meta.exchange_name, meta.market_type, meta.symbol,
                convert_interval_ms_to_str(meta.bar_close_policy.time_ms),
                last_kline_close_time_ms.has_value()
                    ? last_kline_close_time_ms.value()
                    : 0,
                1000, RateLimiter::RequestClass::BAR_BACKFILL, &page_failed);
        if (page_failed) {
            fetch_failed = true;
            break;
        }
        if (ticks.size() == 0) {
            // The polygon non-daily fetch is WINDOWED (the proxy serves
            // POLYGON_BARS_WINDOW_DAYS from the cursor), so an empty result
            // means this window was quiet, NOT that the symbol has no history.
            // The 1m cursor starts at a fixed 2020-01-01, so every symbol that
            // listed later got one empty window and was marked complete with
            // zero bars: 25,495 of 29,604 1m datasets, 80% of them listed after
            // 2020. Daily escapes it by fetching the full range unwindowed.
            // Step the window forward instead of giving up.
            if (is_windowed_polygon_fetch &&
                last_kline_close_time_ms.has_value() &&
                last_kline_close_time_ms.value() < now_ms_for_window) {
                last_kline_close_time_ms =
                    last_kline_close_time_ms.value() + kPolygonFetchWindowMs;
                prev_last_kline_close_time_ms = last_kline_close_time_ms;
                ++empty_windows_stepped;
                continue;
            }
            break;
        }
        for (const auto &tick : ticks) {
            this->add_new_kline(tick, true);
        }
        bars_fetched += static_cast<long long>(ticks.size());
        ++iters;

        // Same reasoning as the empty case: on a WINDOWED fetch a short page
        // means the window was quiet, not that history ran out. Illiquid names
        // stopped here mid-2020 and never reached the present.
        if (ticks.size() < 50 && !is_windowed_polygon_fetch) {
            break;
        }

        last_kline_close_time_ms = this->get_latest_bar_close_time_ms();
        if (last_kline_close_time_ms == prev_last_kline_close_time_ms) {
            // No forward progress. For a windowed fetch that is a quiet window,
            // not the end of history: step over it while the cursor is behind.
            if (is_windowed_polygon_fetch &&
                last_kline_close_time_ms.has_value() &&
                last_kline_close_time_ms.value() < now_ms_for_window) {
                last_kline_close_time_ms =
                    last_kline_close_time_ms.value() + kPolygonFetchWindowMs;
                prev_last_kline_close_time_ms = last_kline_close_time_ms;
                ++empty_windows_stepped;
                continue;
            }
            break;
        }

        prev_last_kline_close_time_ms = last_kline_close_time_ms;

        // Flush and trim mid-backfill so peak memory is bounded by this
        // threshold rather than by how deep the symbol's history runs. The
        // page cursor reads klines.back(), and rm_unused_klines drops from
        // the front, so trimming here cannot disturb pagination. Already
        // written months survive because the partition writer merges.
        if (venue_ceiling > 0 &&
            this->get_kline_count() > kFlushBacklogKlines) {
            this->sync_to_disk();
            this->rm_unused_klines(venue_ceiling);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    this->sync_to_disk();
    if (venue_ceiling > 0) {
        this->rm_unused_klines(venue_ceiling);
    }

    meta.read_last_tick_close_time_ms();
    if (backfill_allows_stream_ingest(meta, bars_fetched,
                                      meta.last_storage_tick_close_time_ms,
                                      DatetimeUtils::get_current_time_ms())) {
        this->init_fetch_done = true;
    }

    if (bars_fetched > 0) return BackfillOutcome::Fetched;
    // A failure that yielded nothing is not evidence the symbol is empty.
    if (fetch_failed) return BackfillOutcome::Failed;
    return BackfillOutcome::NoData;
}

/**
 * @brief Sync the dataset to disk.
 *
 * Writes the dataset's klines to disk in a new partition.
 *
 * @note This function is thread-safe.
 */
void MarketData::OhlcvDataset::sync_to_disk() {
    // Declared outside the try so the catch can hand the months back.
    std::unordered_set<std::string> writing_months;
    try {
        if (!disk_dirty_.load(std::memory_order_relaxed)) {
            return;
        }
        if (is_running_live_trading && meta.exchange_name != "polygon" &&
            !Core::Engine::getInstance().is_engine_actively_trading()) {
            return;
        }
        // Take-and-clear: a bar arriving mid-write re-inserts its own month, so
        // it is picked up next cycle. Restored below if the write fails.
        std::vector<MarketData::OhlcvDatasetTick> klines_copy;
        {
            std::unique_lock<std::shared_mutex> lock(*dataset_mutex);
            this->dedup_and_sort_klines(false);
            writing_months.swap(dirty_months_);
            disk_dirty_.store(false, std::memory_order_relaxed);
            if (klines.empty() || writing_months.empty()) {
                return;
            }
            // Only the months that actually changed. A dataset holding four
            // years of daily bars used to rewrite all ~48 monthly files to
            // record one new bar.
            klines_copy.reserve(klines.size());
            for (const auto &k : klines) {
                if (writing_months.count(
                        ParquetUtils::ohlcv_partition_month_key(
                            k.kline_open_time)) > 0) {
                    klines_copy.push_back(k);
                }
            }
            if (klines_copy.empty()) {
                return;
            }
        }
        std::string path = get_parquets_path();

        int write_rc = 0;
        if (meta.bar_close_policy.store_only_high_and_low) {
            write_rc = ParquetUtils::write_ohlcv_ticks_partitioned_minimal(
                path, klines_copy);
        } else {
            write_rc =
                ParquetUtils::write_ohlcv_ticks_partitioned(path, klines_copy);
        }

        if (write_rc < 0) {
            std::unique_lock<std::shared_mutex> lock(*dataset_mutex);
            dirty_months_.insert(writing_months.begin(), writing_months.end());
            disk_dirty_.store(true, std::memory_order_relaxed);
        }
    } catch (const std::exception &e) {
        {
            std::unique_lock<std::shared_mutex> lock(*dataset_mutex);
            dirty_months_.insert(writing_months.begin(), writing_months.end());
            disk_dirty_.store(true, std::memory_order_relaxed);
        }
        nlohmann::json metadata = nlohmann::json::object();
        metadata["error"] = e.what();
        metadata["dataset_identifier"] = identifier;
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_PARQUET_WRITE_FAILED,
                  "Error syncing dataset to disk", metadata);
    }
}

std::vector<MarketData::OhlcvDatasetTick>
MarketData::OhlcvDataset::get_klines_from_parquet_storage() {
    std::string path = get_parquets_path();
    const auto &ohlcv_ticks = ParquetUtils::scan_partitions(
        path, this->read_start_time, std::nullopt);
    return OhlcvDatasetTick::from_arrow_table(ohlcv_ticks);
}

std::vector<MarketData::OhlcvDatasetTick>
MarketData::OhlcvDataset::read_full_history_from_disk(
    std::chrono::system_clock::time_point from_inclusive,
    std::chrono::system_clock::time_point to_exclusive) const {
    std::vector<MarketData::OhlcvDatasetTick> out;
    if (!(from_inclusive < to_exclusive)) return out;

    const auto to_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           to_exclusive.time_since_epoch())
                           .count();
    const auto from_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             from_inclusive.time_since_epoch())
                             .count();

    const auto table = ParquetUtils::scan_partitions(
        meta.get_parquets_path(), from_inclusive, to_exclusive);
    if (table == nullptr) return out;

    // scan_partitions keeps a file whose START is at or before to_exclusive and
    // returns every row in it, so the window is only as tight as the partition.
    // Cut per tick, on the bar's CLOSE: a bar still open at to_exclusive has
    // not happened yet.
    for (auto &tick : OhlcvDatasetTick::from_arrow_table(table)) {
        if (tick.kline_open_time < from_ms) continue;
        const long long close_ms =
            tick.kline_close_time > 0
                ? tick.kline_close_time
                : tick.kline_open_time + meta.bar_close_policy.time_ms;
        if (close_ms > to_ms) continue;
        out.push_back(std::move(tick));
    }
    std::sort(out.begin(), out.end(),
              [](const MarketData::OhlcvDatasetTick &a,
                 const MarketData::OhlcvDatasetTick &b) {
                  return a.kline_open_time < b.kline_open_time;
              });
    out.erase(std::unique(out.begin(), out.end(),
                          [](const MarketData::OhlcvDatasetTick &a,
                             const MarketData::OhlcvDatasetTick &b) {
                              return a.kline_open_time == b.kline_open_time;
                          }),
              out.end());
    return out;
}

std::vector<MarketData::OhlcvDatasetTick>
MarketData::OhlcvDataset::get_ohlcv_ticks_from_disk() {
    // sqlite retired as a market-data store. Parquet is the only on-disk
    // format. If parquet is empty for this symbol, return empty — the bar
    // sync mechanism will bootstrap from a recent timestamp via the proxy.
    return this->get_klines_from_parquet_storage();
}

/**
 * @brief Load klines from disk into memory.
 *
 * @param is_reread If true, re-read the klines from disk even if they are
 * already in memory.
 */
void MarketData::OhlcvDataset::load_klines_from_disk(bool is_reread) {
    std::string path = get_base_directory_path();

    const auto &klines_ret = this->get_ohlcv_ticks_from_disk();
    std::unique_lock<std::shared_mutex> lock(*dataset_mutex);

    if (klines.size() > 0) {
        if (klines_ret.back().kline_open_time <
            klines.back().kline_open_time + 86400000) {
            return;
        }
    }

    for (const auto &tick : klines_ret) {
        kline_open_time_ms.push_back(tick.kline_open_time);
        open_prices.push_back(tick.open_price);
        high_prices.push_back(tick.high_price);
        low_prices.push_back(tick.low_price);
        close_prices.push_back(tick.close_price);
        volumes.push_back(tick.volume);
        kline_close_time_ms.push_back(tick.kline_close_time);
        quote_asset_volumes.push_back(tick.quote_asset_volume);
        number_of_trades.push_back(tick.number_of_trades);
        taker_buy_base_volumes.push_back(tick.taker_buy_base_volume);
        taker_buy_quote_volumes.push_back(tick.taker_buy_quote_volume);
        klines.push_back(tick);
    }
}

void MarketData::OhlcvDataset::reread_from_disk() {
    this->load_klines_from_disk(true);
}

/**
 * @brief Centralized thread-safe entry point for computing a feature from this
 * dataset.
 *
 * Executes the provided callback under lock, giving full access to the
 * dataset's internal state.
 *
 * @param feature_function A callback function that computes the feature from
 * the dataset.
 * @return An optional feature value returned by the callback.
 */
std::optional<double> MarketData::OhlcvDataset::compute_feature(
    const std::function<std::optional<double>(OhlcvDataset *)>
        &feature_function) {
    std::shared_lock<std::shared_mutex> lock(*dataset_mutex);
    return feature_function(this);
}

long long MarketData::OhlcvDataset::get_kline_count() {
    std::shared_lock<std::shared_mutex> lock(*dataset_mutex);
    return klines.size();
}

double MarketData::OhlcvDataset::get_latest_price() {
    std::shared_lock<std::shared_mutex> lock(*dataset_mutex);
    if (close_prices.size() == 0) {
        return 0;
    }
    return close_prices.back();
}

double MarketData::OhlcvDataset::get_latest_quote_asset_volume() {
    std::shared_lock<std::shared_mutex> lock(*dataset_mutex);
    if (quote_asset_volumes.size() == 0) {
        return 0;
    }
    return quote_asset_volumes.back();
}

void MarketData::OhlcvDataset::add_pointer_to_realtime_bar(
    const std::shared_ptr<Bar> &bar) {
    std::unique_lock<std::shared_mutex> lock(*dataset_mutex);
    this->realtime_bar = bar;
}

std::shared_ptr<Bar> MarketData::OhlcvDataset::get_realtime_bar() const {
    return this->realtime_bar;
}

bool MarketData::OhlcvDataset::is_synced_with_realtime() {
    std::shared_lock<std::shared_mutex> lock(*dataset_mutex);
    if (realtime_bar == nullptr) {
        return false;
    }
    return realtime_bar->get_is_synced_with_live();
}

bool MarketData::OhlcvDataset::is_exchange_bar() {
    std::shared_lock<std::shared_mutex> lock(*dataset_mutex);
    return meta.bar_close_policy.is_exchange_bar;
}

std::optional<long long>
MarketData::OhlcvDataset::get_latest_bar_close_time_ms() {
    std::shared_lock<std::shared_mutex> lock(*dataset_mutex);
    if (meta.bar_close_policy.is_exchange_bar ||
        meta.bar_close_policy.is_time_bar) {
        if (klines.size() == 0) {
            return std::nullopt;
        }
        return klines.back().kline_open_time + meta.bar_close_policy.time_ms;
    }
    if (klines.size() == 0) {
        return std::nullopt;
    }
    return klines.back().kline_close_time;
}

std::optional<long long>
MarketData::OhlcvDataset::get_latest_bar_open_time_ms() {
    std::shared_lock<std::shared_mutex> lock(*dataset_mutex);
    if (meta.bar_close_policy.is_exchange_bar) {
        if (klines.size() == 0) {
            return std::nullopt;
        }
        return klines.back().kline_open_time;
    }
    if (klines.size() == 0) {
        return std::nullopt;
    }
    return klines.back().kline_open_time;
}

bool MarketData::OhlcvDataset::get_init_fetch_done() {
    std::shared_lock<std::shared_mutex> lock(*dataset_mutex);
    return init_fetch_done;
}

bool MarketData::OhlcvDataset::is_disk_dirty() const {
    return disk_dirty_.load(std::memory_order_relaxed);
}

MarketData::DatasetSummary MarketData::OhlcvDataset::build_summary() const {
    DatasetSummary summary;
    summary.identifier = identifier;
    summary.exchange_name = meta.exchange_name;
    summary.market_type = meta.market_type;
    summary.symbol = meta.symbol;
    summary.interval_ms = meta.bar_close_policy.time_ms;

    // "exchange/60000_ms" -> kind "exchange", display "exchange:60000_ms".
    const std::string bar_type =
        meta.bar_close_policy.get_bar_type_and_close_threshold();
    const auto separator = bar_type.find('/');
    summary.bar_kind = separator == std::string::npos
                           ? bar_type
                           : bar_type.substr(0, separator);
    std::string bar_type_key = bar_type;
    std::replace(bar_type_key.begin(), bar_type_key.end(), '/', ':');
    summary.display_key = meta.exchange_name + ":" + meta.market_type + ":" +
                          meta.symbol + ":" + bar_type_key;

    std::shared_lock<std::shared_mutex> lock(*dataset_mutex);
    summary.init_fetch_done = init_fetch_done;
    summary.kline_count = klines.size();
    if (!klines.empty()) {
        summary.first_bar_open_time_ms = klines.front().kline_open_time;
        summary.last_bar_open_time_ms = klines.back().kline_open_time;
        summary.last_bar_close_time_ms = klines.back().kline_close_time;
    }
    return summary;
}

void MarketData::OhlcvDataset::clear_klines_from_memory() {
    std::unique_lock<std::shared_mutex> lock(*dataset_mutex);

    klines.clear();
    klines.shrink_to_fit();

    kline_open_time_ms.clear();
    kline_open_time_ms.shrink_to_fit();

    open_prices.clear();
    open_prices.shrink_to_fit();

    high_prices.clear();
    high_prices.shrink_to_fit();

    low_prices.clear();
    low_prices.shrink_to_fit();

    close_prices.clear();
    close_prices.shrink_to_fit();

    volumes.clear();
    volumes.shrink_to_fit();

    kline_close_time_ms.clear();
    kline_close_time_ms.shrink_to_fit();

    quote_asset_volumes.clear();
    quote_asset_volumes.shrink_to_fit();

    number_of_trades.clear();
    number_of_trades.shrink_to_fit();

    taker_buy_base_volumes.clear();
    taker_buy_base_volumes.shrink_to_fit();

    taker_buy_quote_volumes.clear();
    taker_buy_quote_volumes.shrink_to_fit();

    feature_cache_store.clear();
}

std::vector<double> MarketData::OhlcvDataset::get_close_prices() const {
    std::shared_lock<std::shared_mutex> lock(*dataset_mutex);
    return close_prices;
}

std::vector<long> MarketData::OhlcvDataset::get_number_of_trades() const {
    std::shared_lock<std::shared_mutex> lock(*dataset_mutex);
    return number_of_trades;
}

std::vector<double> MarketData::OhlcvDataset::get_open_prices() const {
    std::shared_lock<std::shared_mutex> lock(*dataset_mutex);
    return open_prices;
}

std::vector<double> MarketData::OhlcvDataset::get_volumes() const {
    std::shared_lock<std::shared_mutex> lock(*dataset_mutex);
    return volumes;
}

std::vector<double> MarketData::OhlcvDataset::get_high_prices() const {
    std::shared_lock<std::shared_mutex> lock(*dataset_mutex);
    return high_prices;
}

std::vector<double> MarketData::OhlcvDataset::get_low_prices() const {
    std::shared_lock<std::shared_mutex> lock(*dataset_mutex);
    return low_prices;
}

std::vector<long long> MarketData::OhlcvDataset::get_kline_open_times_ms()
    const {
    std::shared_lock<std::shared_mutex> lock(*dataset_mutex);
    return kline_open_time_ms;
}

std::vector<double> MarketData::OhlcvDataset::get_quote_asset_volumes() const {
    std::shared_lock<std::shared_mutex> lock(*dataset_mutex);
    return quote_asset_volumes;
}

std::shared_lock<std::shared_mutex> MarketData::OhlcvDataset::lock_for_read()
    const {
    return std::shared_lock<std::shared_mutex>(*dataset_mutex);
}

void MarketData::OhlcvDataset::rescale_for_split(long long cutoff_ms,
                                                  double ratio) {
    std::unique_lock<std::shared_mutex> lock(*dataset_mutex);
    for (auto& k : klines) {
        if (k.kline_open_time >= cutoff_ms) continue;
        k.open_price /= ratio;
        k.high_price /= ratio;
        k.low_price /= ratio;
        k.close_price /= ratio;
        k.volume *= ratio;
        k.taker_buy_base_volume *= ratio;
    }
    // The column vectors are not reordered by dedup_and_sort_klines, so they
    // are matched by their own open times, not by index into klines.
    for (size_t i = 0; i < kline_open_time_ms.size(); ++i) {
        if (kline_open_time_ms[i] >= cutoff_ms) continue;
        if (i < open_prices.size()) open_prices[i] /= ratio;
        if (i < high_prices.size()) high_prices[i] /= ratio;
        if (i < low_prices.size()) low_prices[i] /= ratio;
        if (i < close_prices.size()) close_prices[i] /= ratio;
        if (i < volumes.size()) volumes[i] *= ratio;
        if (i < taker_buy_base_volumes.size())
            taker_buy_base_volumes[i] *= ratio;
    }
}

void MarketData::OhlcvDataset::rm_unused_klines(long long max_klines_to_keep) {
    std::unique_lock<std::shared_mutex> lock(*dataset_mutex);
    if (klines.size() > max_klines_to_keep) {
        size_t num_to_erase = klines.size() - max_klines_to_keep;
        klines.erase(klines.begin(), klines.begin() + num_to_erase);
        kline_open_time_ms.erase(kline_open_time_ms.begin(),
                                 kline_open_time_ms.begin() + num_to_erase);
        open_prices.erase(open_prices.begin(),
                          open_prices.begin() + num_to_erase);
        high_prices.erase(high_prices.begin(),
                          high_prices.begin() + num_to_erase);
        low_prices.erase(low_prices.begin(), low_prices.begin() + num_to_erase);
        close_prices.erase(close_prices.begin(),
                           close_prices.begin() + num_to_erase);
        volumes.erase(volumes.begin(), volumes.begin() + num_to_erase);
        kline_close_time_ms.erase(kline_close_time_ms.begin(),
                                  kline_close_time_ms.begin() + num_to_erase);
        quote_asset_volumes.erase(quote_asset_volumes.begin(),
                                  quote_asset_volumes.begin() + num_to_erase);
        number_of_trades.erase(number_of_trades.begin(),
                               number_of_trades.begin() + num_to_erase);
        taker_buy_base_volumes.erase(
            taker_buy_base_volumes.begin(),
            taker_buy_base_volumes.begin() + num_to_erase);
        taker_buy_quote_volumes.erase(
            taker_buy_quote_volumes.begin(),
            taker_buy_quote_volumes.begin() + num_to_erase);
        klines.shrink_to_fit();
        kline_open_time_ms.shrink_to_fit();
        open_prices.shrink_to_fit();
        high_prices.shrink_to_fit();
        low_prices.shrink_to_fit();
        close_prices.shrink_to_fit();
        volumes.shrink_to_fit();
        kline_close_time_ms.shrink_to_fit();
        quote_asset_volumes.shrink_to_fit();
        number_of_trades.shrink_to_fit();
        taker_buy_base_volumes.shrink_to_fit();
        taker_buy_quote_volumes.shrink_to_fit();
    }
}

void MarketData::OhlcvDataset::mark_init_sync_done() {
    // for backtest, since we dont want to actually fetch from proxy
    std::unique_lock<std::shared_mutex> lock(*dataset_mutex);
    this->init_fetch_done = true;
}

std::vector<MarketData::OhlcvDatasetTick>
MarketData::OhlcvDataset::get_all_kline_ticks() const {
    std::shared_lock<std::shared_mutex> lock(*dataset_mutex);
    return klines;
}