#pragma once
#include <atomic>
#include <memory>
#include <shared_mutex>
#include <span>
#include <unordered_set>
#include <vector>

#include "chrono_utils.h"
#include "market_data/bar.h"
#include "market_data/dataset_summary.h"
#include "market_data/feature_cache.h"
#include "market_data/ohlcv_bar_meta.h"
#include "ohlcv_dataset_tick.h"

namespace MarketData {

std::string get_dataset_identifier(const std::string &symbol,
                                   const std::string &exchange_name,
                                   const std::string &market_type,
                                   long long interval_ms);

struct CacheReturn {
    long long latest_kline_close_time_ms;
    std::optional<FeatureCacheValue> feature_cache_value;
};

enum OhlcvDatasetType {
    OHLCV_DATASET_TYPE_CRYPTO,
    OHLCV_DATASET_TYPE_BINANCE,
    OHLCV_DATASET_TYPE_POLYGON,
};

class OhlcvDataset {
   public:
    OhlcvDataset(const OhlcvBarMeta &input,
                 std::chrono::system_clock::time_point start_time =
                     ChronoUtils::make_timepoint(2000, 1, 1, 0, 0, 0),
                 bool read_data_on_init = true,
                 bool can_fetch_from_proxy = false,
                 long long max_num_required_klines_after_init_sync = 700000);
    ~OhlcvDataset();

    OhlcvBarMeta meta;
    std::vector<MarketData::OhlcvDatasetTick> klines;
    std::vector<long long> kline_open_time_ms;
    std::vector<double> open_prices;
    std::vector<double> high_prices;
    std::vector<double> low_prices;
    std::vector<double> close_prices;
    std::vector<double> volumes;
    std::vector<long long> kline_close_time_ms;
    std::vector<double> quote_asset_volumes;
    std::vector<long> number_of_trades;
    std::vector<double> taker_buy_base_volumes;
    std::vector<double> taker_buy_quote_volumes;

    std::chrono::system_clock::time_point read_start_time;
    FeatureCache feature_cache_store;

    std::string identifier;

    bool is_gapped();
    bool is_missing_klines();
    bool is_exchange_bar();
    bool is_synced_with_realtime();

    void dedup_and_sort_klines(bool require_lock);
    void sync_to_disk();
    void mark_init_sync_done();
    void load_klines_from_disk(bool is_reread = false);
    void reread_from_disk();
    // Failed is NOT NoData: a rate limit or parse error returns no bars but
    // says nothing about whether the symbol has history. Collapsing the two is
    // what let a throttled backfill count as complete and never be retried.
    enum class BackfillOutcome { Skipped, Fetched, NoData, Failed };
    BackfillOutcome init_sync_bars();
    void clear_klines_from_memory();
    void rm_unused_klines(long long max_klines_to_keep);
    // Rescales in-memory bars opening before `cutoff_ms` for a split of
    // `ratio` (prices / ratio, share volume * ratio). Leaves the disk alone:
    // the caller has already rescaled the stored bars.
    void rescale_for_split(long long cutoff_ms, double ratio);

    std::vector<double> get_close_prices() const;
    std::vector<long> get_number_of_trades() const;
    std::vector<double> get_open_prices() const;
    std::vector<double> get_volumes() const;
    std::vector<double> get_high_prices() const;
    std::vector<double> get_low_prices() const;
    std::vector<double> get_quote_asset_volumes() const;
    std::vector<long long> get_kline_open_times_ms() const;
    std::vector<MarketData::OhlcvDatasetTick> get_all_kline_ticks() const;
    std::shared_lock<std::shared_mutex> lock_for_read() const;

    std::vector<MarketData::OhlcvDatasetTick> get_ohlcv_ticks_from_disk();

    std::vector<MarketData::OhlcvDatasetTick> get_klines_from_parquet_storage();

    // History for a bounded window, straight from parquet. Does not touch
    // `klines` and is not subject to the retention cap, so a strategy can read
    // far more history than the dataset holds, reduce it, and drop it.
    //
    // `to_exclusive` is REQUIRED, and it is required because parquet on disk
    // holds the whole series: in a backtest that includes the simulation's
    // future. An optional bound would default to reading it, and the resulting
    // features would look excellent rather than wrong.
    //
    // ParquetUtils::scan_partitions prunes by FILE start time only, so a
    // monthly partition comes back whole; the row-level cut is applied here.
    std::vector<MarketData::OhlcvDatasetTick> read_full_history_from_disk(
        std::chrono::system_clock::time_point from_inclusive,
        std::chrono::system_clock::time_point to_exclusive) const;

    void add_new_kline(const MarketData::OhlcvDatasetTick &tick,
                       bool is_called_from_init_fetch = false);
    void add_pointer_to_realtime_bar(const std::shared_ptr<Bar> &bar);
    std::shared_ptr<Bar> get_realtime_bar() const;
    long long get_kline_count();
    double get_latest_price();
    double get_latest_quote_asset_volume();

    bool get_init_fetch_done();
    bool is_disk_dirty() const;

    DatasetSummary build_summary() const;

    std::optional<long long> get_latest_bar_close_time_ms();
    std::optional<long long> get_latest_bar_open_time_ms();

    std::optional<double> compute_feature(
        const std::function<std::optional<double>(OhlcvDataset *)>
            &feature_function);

    OhlcvDataset(const OhlcvDataset &) = delete;
    OhlcvDataset &operator=(const OhlcvDataset &) = delete;

    OhlcvDataset(OhlcvDataset &&) = default;
    OhlcvDataset &operator=(OhlcvDataset &&) = default;
    std::string get_base_directory_path();
    std::string get_parquets_path();

   private:
    void create_dataset_dir();
    void init_state();
    std::string data_library_root;
    OhlcvDatasetType dataset_type = OHLCV_DATASET_TYPE_CRYPTO;
    bool is_continuous_dataset = true;
    std::unique_ptr<std::shared_mutex> dataset_mutex;
    bool init_fetch_done = false;
    bool read_data_on_init = true;
    bool can_fetch_from_proxy = false;
    bool is_running_live_trading = false;
    // When true, add_new_kline keeps ticks that fall outside Regular
    // Trading Hours (e.g. Polygon stock pre-/post-market). Set
    // automatically by the constructor based on venue — Polygon stock
    // datasets keep all hours; other equity venues drop outside-RTH.
    // Continuous-trading venues (crypto) have no RTH concept so the
    // filter never fires regardless.
    bool include_outside_rth_ticks = false;
    std::shared_ptr<Bar> realtime_bar;
    long long max_num_required_klines_after_init_sync = 0;
    // disk_dirty_ stays atomic as the lock-free fast path: the flush loop asks
    // every one of ~59k datasets each cycle and must not take a lock to do it.
    // dirty_months_ carries the detail and is guarded by dataset_mutex.
    std::atomic<bool> disk_dirty_{false};
    std::unordered_set<std::string> dirty_months_;
};

}  // namespace MarketData
