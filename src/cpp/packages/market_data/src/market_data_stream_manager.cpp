#include "market_data/market_data_stream_manager.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>

#include "ccxt_proxy.h"
#include "chrono_utils.h"
#include "common_types/log_event_types.h"
#include "datetime_utils.h"
#include "disk_logging/log.h"
#include "env_utils.h"
#include "market_data/backfill_attempt_cache.h"
#include "market_data/bar_aggregator.h"
#include "market_data/config.h"
#include "market_data/data_recorder.h"
#include "market_data/dataset_manager.h"
#include "market_data/market_data_streams.h"
#include "market_data/ohlcv_dataset.h"
#include "market_data/polygon_second_bars.h"
#include "market_data/state_machine_dispatcher.h"
#include "market_data/stream_info.h"
#include "market_data/utils.h"
#include "market_making/dtypes/exchange_info.h"
#include "market_making/engine.h"
#include "nlohmann/json.hpp"
#include "threadpool.h"
#include "ws_stream.h"

namespace MarketData {

MarketDataStreamManager::MarketDataStreamManager(
    const std::function<void(const std::string&)>& error_log_callback,
    const std::function<void(const std::string&)>& info_log_callback,
    const std::chrono::system_clock::time_point data_start_date,
    bool is_called_from_trading_client)
    : is_called_from_trading_client(is_called_from_trading_client),
      error_log_callback(error_log_callback),
      info_log_callback(info_log_callback),
      data_start_date(data_start_date),
      process_start_ms_(DatetimeUtils::get_current_time_ms()) {}

namespace {

std::string dataset_identity(const std::string& exchange_name,
                             const std::string& market_type,
                             const std::string& symbol,
                             const BarClosePolicy& policy) {
    return get_derived_bars_base_dir(symbol, exchange_name, market_type,
                                     policy, true);
}

BarClosePolicy exchange_bar_policy(long long interval_ms) {
    return BarClosePolicy::create(false, false, true, true, 0, 0, interval_ms);
}

BarClosePolicy trade_bar_policy(long long interval_ms) {
    return BarClosePolicy::create(false, false, true, false, 0, 0, interval_ms,
                                  false);
}

std::vector<StreamsConfig> to_streams_configs(
    const std::vector<ResolvedConnection>& connections) {
    std::vector<StreamsConfig> out;
    for (const auto& connection : connections) {
        StreamsConfig config;
        for (const auto& symbol : connection.symbols) {
            StreamInfo info;
            info.exchange_name = connection.exchange_name;
            info.market_type = connection.market_type;
            info.symbol = symbol;
            info.interval = connection.interval;
            if (connection.exchange_name == "polygon")
                info.api_key = Config::getInstance().get_polygon_api_key();
            config.streams.push_back(std::move(info));
        }
        out.push_back(std::move(config));
    }
    return out;
}

bool covers(const std::vector<ResolvedConnection>& connections,
            const std::string& exchange_name, const std::string& market_type,
            const std::string& symbol) {
    for (const auto& c : connections) {
        if (c.exchange_name != exchange_name || c.market_type != market_type)
            continue;
        for (const auto& s : c.symbols) {
            if (s == symbol) return true;
        }
    }
    return false;
}

void require_polygon_key_if_streamed(const ResolvedCoverage& coverage) {
    for (const auto& connection : coverage.exchange_bar_connections) {
        if (connection.exchange_name == "polygon" &&
            Config::getInstance().get_polygon_api_key().empty())
            throw std::invalid_argument(
                "coverage streams polygon but POLYGON_API_KEY is not set");
    }
}

// A recording process keeps only recent bars in memory; everything older is
// on disk. Without a bound a 24/7 process grows by every bar it receives.
constexpr long long RECORDER_MEMORY_WINDOW_MS = 3LL * 86'400'000;

long long recorder_kline_cap(long long interval_ms) {
    return std::max<long long>(RECORDER_MEMORY_WINDOW_MS / interval_ms, 100);
}

// Trims only after a successful flush, so a dropped bar is always on disk.
void trim_recorder_memory_loop() {
    while (true) {
        std::this_thread::sleep_for(std::chrono::minutes(10));
        for (const auto& dataset :
             DatasetsManager::get_instance().get_ohlcv_datasets()) {
            const long long cap =
                recorder_kline_cap(dataset->meta.bar_close_policy.time_ms);
            if (dataset->get_kline_count() <= 2 * cap) continue;
            dataset->sync_to_disk();
            if (dataset->is_disk_dirty()) continue;
            dataset->rm_unused_klines(cap);
        }
    }
}

constexpr long long HOUR_MS = 3'600'000LL;
constexpr int SECOND_BAR_SESSIONS_IN_MEMORY = 2;

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Reads back the latest sessions' hourly files so a restart keeps serving
// them; only the sessions kept in memory are read.
void load_recent_second_bars(const std::string& data_dir_root) {
    std::map<long long, std::vector<std::string>> files_by_session;
    const long long newest_hour = now_ms() / HOUR_MS * HOUR_MS;
    for (long long hour = newest_hour - 7 * 24 * HOUR_MS; hour <= newest_hour;
         hour += HOUR_MS) {
        auto path = PolygonSecondBarStore::hour_file_path(data_dir_root, hour);
        if (std::filesystem::exists(path))
            files_by_session[PolygonSecondBarStore::session_day(hour)]
                .push_back(std::move(path));
    }
    while (files_by_session.size() > SECOND_BAR_SESSIONS_IN_MEMORY)
        files_by_session.erase(files_by_session.begin());
    SecondBarsBySymbol loaded;
    std::size_t files = 0;
    for (const auto& [session, paths] : files_by_session) {
        for (const auto& path : paths) {
            if (PolygonSecondBarStore::read_hour_file(path, loaded)) {
                ++files;
            } else {
                LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_INIT_PHASE,
                          "unreadable second-bar file",
                          (nlohmann::json{{"path", path}}));
            }
        }
    }
    PolygonSecondBarStore::get_instance().merge(std::move(loaded));
    LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_INIT_PHASE,
             "loaded second bars from disk", (nlohmann::json{{"files", files}}));
}

// Writes each UTC hour once, two minutes after it ends. An hour that already
// has a file (the nightly REST rebuild, or a previous process) is left alone,
// as is every hour before this process started.
void second_bar_writer_loop(bool load_from_disk) {
    const std::string root = Config::getInstance().get_data_dir_root();
    auto& store = PolygonSecondBarStore::get_instance();
    long long next_hour = now_ms() / HOUR_MS * HOUR_MS;
    if (load_from_disk) load_recent_second_bars(root);
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        while (next_hour + HOUR_MS + 120'000 <= now_ms()) {
            const auto path = PolygonSecondBarStore::hour_file_path(root, next_hour);
            const auto bars = store.hour(next_hour);
            if (!bars.empty() && !std::filesystem::exists(path)) {
                if (!PolygonSecondBarStore::write_hour_file(path, bars, "stream"))
                    LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_PARQUET_WRITE_FAILED,
                              "second-bar hour write failed",
                              (nlohmann::json{{"path", path}}));
            }
            next_hour += HOUR_MS;
        }
        store.keep_latest_sessions(SECOND_BAR_SESSIONS_IN_MEMORY);
    }
}

std::string polymarket_ws_url() {
    return EnvUtils::env_or(
        "POLYMARKET_WS_URL",
        "wss://ws-subscriptions-clob.polymarket.com/ws/market");
}

}  // namespace

// Bar-backfill fan-out per venue. The RateLimiter is the actual throttle
// (kPolygonRate 100/s vs kBinanceRate 30/s), so this only decides whether a
// venue's budget can be reached at all: at ~200ms/request, 2 threads cap out
// near 10 req/s and leave 90% of Polygon's budget unused. A ~12.5k-symbol
// equity backfill never drains at that width.
//
// Deliberately NOT applied to the pools in bar_aggregator.cpp — those sit on
// the trades->bars path and are memory-bound by design ("Sequential: limits
// peak memory to one symbol at a time"), and Polygon does not use that path.
static size_t backfill_pool_size(const std::string& exchange_name) {
    return exchange_name == "polygon" ? 64 : 1;
}

// Report how much of the requested universe actually has bars on disk.
//
// A backfill that half-finishes is indistinguishable from one that succeeded
// unless something counts, which is how a 39%-complete polygon store looked
// finished for weeks. Reads the durable signal (last tick close time recorded
// on disk) rather than in-memory klines, which get trimmed after the fetch.

// Close time of the most recent daily bar that can already be final. A 1d bar
// for day D carries close_time D+1 00:00 UTC, so yesterday's bar closes at
// today's UTC midnight. Weekend days are stepped over because no bar exists for
// them; a holiday is not, which costs one wasted request per symbol on the day
// after and never a missed bar.
static long long last_final_daily_close_ms(long long now_ms) {
    constexpr long long kDay = 86400000LL;
    long long candidate = (now_ms / kDay) * kDay;
    for (int i = 0; i < 7; ++i) {
        const std::time_t session_start =
            static_cast<std::time_t>((candidate - kDay) / 1000);
        std::tm tm{};
        gmtime_r(&session_start, &tm);
        if (tm.tm_wday != 0 && tm.tm_wday != 6) break;  // not Sun/Sat
        candidate -= kDay;
    }
    return candidate;
}

// True when a fetch provably cannot return anything new. Scoped to polygon
// daily bars: crypto venues are continuous and carry different boundary
// semantics, so their backfill is untouched. Every rule here is written so that
// being wrong costs a redundant request rather than a missing bar.
static bool can_skip_backfill(
    const std::shared_ptr<MarketData::OhlcvDataset>& dataset,
    long long now_ms) {
    if (dataset->meta.exchange_name != "polygon") return false;

    // Daily only: the head rule needs a session boundary, and intraday bars
    // exist only 04:00-20:00 ET so "current" means something different there.
    if (dataset->meta.bar_close_policy.time_ms == 86400000LL) {
        dataset->meta.read_last_tick_close_time_ms();
        const long long head = dataset->meta.last_storage_tick_close_time_ms;
        if (head > 0 && head >= last_final_daily_close_ms(now_ms)) return true;
    }

    // The empty-window step makes a symbol with no history walk every window
    // from 2020 before giving up, so repeatedly-empty ones must be remembered
    // or they cost ~40 requests each on every start.

    const std::string key =
        dataset->meta.exchange_name + ":" + dataset->meta.market_type + ":" +
        dataset->meta.symbol + ":" +
        std::to_string(dataset->meta.bar_close_policy.time_ms);
    return MarketData::BackfillAttemptCache::instance().should_skip(key,
                                                                    now_ms);
}

static void record_backfill_outcome(
    const std::shared_ptr<MarketData::OhlcvDataset>& dataset,
    MarketData::OhlcvDataset::BackfillOutcome outcome, long long now_ms) {
    using Outcome = MarketData::OhlcvDataset::BackfillOutcome;
    if (outcome != Outcome::Fetched && outcome != Outcome::NoData) return;
    if (MarketData::has_only_current_month_partition(dataset->meta)) return;
    const std::string key =
        dataset->meta.exchange_name + ":" + dataset->meta.market_type + ":" +
        dataset->meta.symbol + ":" +
        std::to_string(dataset->meta.bar_close_policy.time_ms);
    auto& cache = MarketData::BackfillAttemptCache::instance();
    if (outcome == Outcome::Fetched) {
        cache.record_data(key, now_ms);
    } else {
        cache.record_empty(key, now_ms);
    }
}

// True when the dataset failed, which is the only outcome worth another round:
// nothing-new is an answer, and a success has already reached the present.
static bool backfill_dataset_once(
    const std::shared_ptr<MarketData::OhlcvDataset>& dataset) {
    const long long now_ms = DatetimeUtils::get_current_time_ms();
    if (can_skip_backfill(dataset, now_ms)) return false;
    const auto outcome = dataset->init_sync_bars();
    record_backfill_outcome(dataset, outcome, now_ms);
    return outcome == MarketData::OhlcvDataset::BackfillOutcome::Failed;
}

static void log_backfill_coverage(
    const std::vector<std::shared_ptr<MarketData::OhlcvDataset>>& datasets,
    const std::string& phase) {
    std::map<std::string, std::array<int, 3>> by_exchange;  // {ok, empty, stub}
    std::map<std::string, std::vector<std::string>> empty_symbols;
    std::map<std::string, std::vector<std::string>> stub_symbols;

    for (const auto& dataset : datasets) {
        auto& counts = by_exchange[dataset->meta.exchange_name];
        dataset->meta.read_last_tick_close_time_ms();
        if (dataset->meta.last_storage_tick_close_time_ms <= 0) {
            ++counts[1];
            auto& sample = empty_symbols[dataset->meta.exchange_name];
            if (sample.size() < 25) sample.push_back(dataset->meta.symbol);
            continue;
        }
        // "Has bars" is not the check. A dataset holding a single partition,
        // and that partition the current month, is a stream-only stub: the
        // backfill produced nothing and live bars were written into the gap.
        // It looks identical to a healthy symbol on a last-timestamp test,
        // which is exactly why that test missed ~6k of them.
        if (MarketData::has_only_current_month_partition(dataset->meta)) {
            ++counts[2];
            auto& sample = stub_symbols[dataset->meta.exchange_name];
            if (sample.size() < 25) sample.push_back(dataset->meta.symbol);
            continue;
        }
        ++counts[0];
    }

    for (const auto& [exchange_name, counts] : by_exchange) {
        const int ok = counts[0], empty = counts[1], stub = counts[2];
        const int total = ok + empty + stub;
        nlohmann::json meta{{"phase", phase},     {"exchange", exchange_name},
                            {"with_history", ok}, {"no_bars", empty},
                            {"stub_only", stub},  {"total", total}};
        if (empty == 0 && stub == 0) {
            LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_INIT_PHASE,
                     "[backfill_coverage] " + exchange_name + " complete: " +
                         std::to_string(ok) + "/" + std::to_string(total),
                     meta);
        } else {
            meta["no_bars_sample"] = empty_symbols[exchange_name];
            meta["stub_sample"] = stub_symbols[exchange_name];
            // New listings are legitimately stub-only; a failed backfill is
            // reported by its own outcome log.
            LOG_WARNING(Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_INIT_PHASE,
                      "[backfill_coverage] " + exchange_name +
                          " INCOMPLETE: " + std::to_string(ok) + "/" +
                          std::to_string(total) + " with history, " +
                          std::to_string(empty) + " with no bars, " +
                          std::to_string(stub) + " stub-only",
                      meta);
        }
    }
}


ResolvedCoverage MarketDataStreamManager::resolve(
    const CoverageConfig& config, const ResolvedCoverage& current) {
    return MarketDataCoverage::resolve_against(
        config,
        [](const std::string& exchange_name, const std::string& market_type) {
            return Core::ExchangeInfo::getInstance().get_active_symbols(
                exchange_name, market_type);
        },
        current);
}

void MarketDataStreamManager::validate_state_machine_dependencies(
    const ResolvedCoverage& coverage) const {
    if (!Config::getInstance().get_is_state_machines_enabled()) return;
    std::vector<std::string> missing;
    for (const auto& machine :
         StateMachineDispatcher::get_instance().get_machines()) {
        for (const auto& dep : machine->get_market_data_dependencies()) {
            if (dep.is_optional) continue;
            bool covered = true;
            switch (dep.event_type) {
                case MarketEvent::TRADE:
                    covered = covers(coverage.trade_connections,
                                     dep.exchange_name, dep.market_type,
                                     dep.symbol);
                    break;
                case MarketEvent::BBA:
                    covered = covers(coverage.book_ticker_connections,
                                     dep.exchange_name, dep.market_type,
                                     dep.symbol);
                    break;
                case MarketEvent::L2_UPDATE:
                    covered = covers(coverage.lob_connections,
                                     dep.exchange_name, dep.market_type,
                                     dep.symbol);
                    break;
                case MarketEvent::OHLCV:
                    covered = false;
                    for (const auto& d : coverage.ohlcv_datasets) {
                        if (d.exchange_name == dep.exchange_name &&
                            d.market_type == dep.market_type &&
                            d.symbol == dep.symbol)
                            covered = true;
                    }
                    for (const auto& d : coverage.trade_bar_datasets) {
                        if (d.exchange_name == dep.exchange_name &&
                            d.market_type == dep.market_type &&
                            d.symbol == dep.symbol)
                            covered = true;
                    }
                    break;
                case MarketEvent::POLYMARKET_BBA:
                case MarketEvent::POLYMARKET_TICK_SIZE_CHANGE:
                case MarketEvent::POLYMARKET_L2_SNAPSHOT:
                case MarketEvent::POLYMARKET_L2_DELTA:
                case MarketEvent::POLYMARKET_LAST_TRADE:
                case MarketEvent::POLYMARKET_MARKET_RESOLVED:
                    covered = coverage.polymarket;
                    break;
                case MarketEvent::FUNDING:
                    break;
            }
            if (!covered)
                missing.push_back(machine->get_name() + " needs " +
                                  dep.get_key());
        }
    }
    if (missing.empty()) return;
    std::string message = "coverage misses state machine dependencies:";
    for (const auto& m : missing) message += " [" + m + "]";
    LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_INIT_PHASE, message,
              (nlohmann::json{{"missing", missing}}));
    throw std::runtime_error(message);
}

void MarketDataStreamManager::init_ohlcv_datasets(
    const std::vector<ResolvedOhlcvDataset>& resolved) {
    LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_INIT_PHASE,
             "[init_ohlcv_datasets] Starting", nlohmann::json{});
    MarketData::BarAggregator& aggregator =
        MarketData::BarAggregator::getInstance();
    ThreadPool thread_pool(std::thread::hardware_concurrency() * 3);
    std::vector<std::future<void>> futures;
    std::mutex created_mutex;
    std::vector<std::shared_ptr<OhlcvDataset>> created;

    for (const auto& entry : resolved) {
        OhlcvBarMeta bar_metadata;
        bar_metadata.symbol = entry.symbol;
        bar_metadata.exchange_name = entry.exchange_name;
        bar_metadata.market_type = entry.market_type;
        bar_metadata.bar_close_policy = exchange_bar_policy(entry.interval_ms);
        {
            std::lock_guard<std::mutex> lock(coverage_mutex_);
            if (!created_datasets_
                     .insert(dataset_identity(entry.exchange_name,
                                              entry.market_type, entry.symbol,
                                              bar_metadata.bar_close_policy))
                     .second)
                continue;
        }
        const long long kline_cap =
            is_called_from_trading_client
                ? 700000
                : recorder_kline_cap(entry.interval_ms);
        futures.push_back(thread_pool.enqueue([this, &aggregator, bar_metadata,
                                               kline_cap, &created_mutex,
                                               &created]() {
            std::shared_ptr<OhlcvDataset> dataset;
            try {
                dataset = std::make_shared<OhlcvDataset>(
                    bar_metadata,
                    get_dataset_read_start_time(bar_metadata, data_start_date),
                    true, true, kline_cap);
            } catch (const std::exception& error) {
                // The pool's future would swallow this, leaving a covered
                // symbol silently unrecorded.
                LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_INIT_PHASE,
                          "[init_ohlcv_datasets] dataset failed to load: " +
                              bar_metadata.exchange_name + ":" +
                              bar_metadata.market_type + ":" +
                              bar_metadata.symbol + ": " + error.what(),
                          nlohmann::json{});
                return;
            }
            aggregator.add_ohlcv_dataset(dataset);
            std::lock_guard<std::mutex> lock(created_mutex);
            created.push_back(dataset);
            if (created.size() % 10 == 0) {
                LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_INIT_PHASE,
                         "Added " + std::to_string(created.size()) +
                             " datasets",
                         nlohmann::json{});
            }
        }));
    }

    LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_INIT_PHASE,
             "[init_ohlcv_datasets] Waiting for " +
                 std::to_string(futures.size()) + " dataset constructions",
             nlohmann::json{});
    for (auto& future : futures) {
        future.wait();
    }
    LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_INIT_PHASE,
             "[init_ohlcv_datasets] All datasets constructed",
             nlohmann::json{});

    if (this->is_called_from_trading_client) {
        std::map<std::pair<std::string, std::string>,
                 std::vector<std::shared_ptr<MarketData::OhlcvDataset>>>
            datasets_by_exchange_market;

        for (const auto& dataset :
             DatasetsManager::get_instance().get_ohlcv_datasets()) {
            auto key = std::make_pair(dataset->meta.exchange_name,
                                      dataset->meta.market_type);
            datasets_by_exchange_market[key].push_back(dataset);
        }

        // Rounds, not a single pass. A dataset whose one fetch was throttled
        // used to be counted complete and never revisited, which is how a run
        // ended with most symbols short of the present while reporting itself
        // finished.
        constexpr int kMaxRounds = 4;
        for (int round = 0; round < kMaxRounds; ++round) {
            std::atomic<long long> failed{0};
            std::vector<std::unique_ptr<ThreadPool>> fetch_pools;
            std::vector<std::future<void>> fetch_futures;

            for (auto& [exchange_market, datasets] :
                 datasets_by_exchange_market) {
                size_t pool_size = backfill_pool_size(exchange_market.first);
                auto pool = std::make_unique<ThreadPool>(pool_size);
                for (const auto& dataset : datasets) {
                    fetch_futures.push_back(pool->enqueue([dataset, &failed]() {
                        if (backfill_dataset_once(dataset)) ++failed;
                    }));
                }
                fetch_pools.push_back(std::move(pool));
            }
            for (auto& future : fetch_futures) {
                future.wait();
            }
            LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_INIT_PHASE,
                     "[backfill] round " + std::to_string(round + 1) +
                         " failed=" + std::to_string(failed.load()),
                     nlohmann::json{});
            if (failed.load() == 0) break;
        }
        log_backfill_coverage(
            DatasetsManager::get_instance().get_ohlcv_datasets(), "blocking");
        return;
    }

    // Non-blocking: streams start while history fills in.
    LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_INIT_PHASE,
             "Starting background bar fetch for " +
                 std::to_string(created.size()) + " datasets",
             nlohmann::json{});
    std::thread([datasets = std::move(created)]() {
        // Minute-history imports must not delay daily ingest. Separate queues
        // share the venue's existing per-market request budget.
        std::map<std::pair<std::string, std::string>,
                 std::vector<std::shared_ptr<MarketData::OhlcvDataset>>>
            by_market_interval;
        for (const auto& dataset : datasets) {
            by_market_interval[{dataset->meta.exchange_name,
                                dataset->meta.market_type + ":" +
                                    std::to_string(dataset->meta.bar_close_policy.time_ms)}]
                .push_back(dataset);
        }

        std::vector<std::unique_ptr<ThreadPool>> pools;
        std::vector<std::future<void>> futures;
        for (auto& [market, market_datasets] : by_market_interval) {
            auto pool =
                std::make_unique<ThreadPool>(backfill_pool_size(market.first));
            for (const auto& dataset : market_datasets) {
                futures.push_back(pool->enqueue(
                    [dataset]() { backfill_dataset_once(dataset); }));
            }
            pools.push_back(std::move(pool));
        }
        for (auto& f : futures) {
            f.wait();
        }
        log_backfill_coverage(datasets, "background");
        LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_INIT_PHASE,
                 "Background bar fetch complete", nlohmann::json{});
    }).detach();
}

void MarketDataStreamManager::init_trade_bar_datasets(
    const std::vector<ResolvedTradeBarDataset>& resolved) {
    MarketData::BarAggregator& aggregator =
        MarketData::BarAggregator::getInstance();
    for (const auto& entry : resolved) {
        OhlcvBarMeta bar_metadata;
        bar_metadata.symbol = entry.symbol;
        bar_metadata.exchange_name = entry.exchange_name;
        bar_metadata.market_type = entry.market_type;
        bar_metadata.bar_close_policy = trade_bar_policy(entry.interval_ms);
        {
            std::lock_guard<std::mutex> lock(coverage_mutex_);
            if (!created_datasets_
                     .insert(dataset_identity(entry.exchange_name,
                                              entry.market_type, entry.symbol,
                                              bar_metadata.bar_close_policy))
                     .second)
                continue;
        }
        // In the background so stream startup is not held up by disk reads.
        auto start_date = data_start_date;
        std::thread([&aggregator, bar_metadata, start_date]() {
            const std::string label =
                bar_metadata.symbol + " " + bar_metadata.exchange_name + " " +
                bar_metadata.market_type + " " +
                bar_metadata.bar_close_policy.to_string();
            LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_INIT_PHASE,
                     "[init_trade_streams] Creating trade bar dataset: " +
                         label,
                     nlohmann::json{});
            auto dataset = std::make_shared<MarketData::OhlcvDataset>(
                bar_metadata,
                MarketData::get_dataset_read_start_time(bar_metadata,
                                                        start_date),
                true, false);
            LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_INIT_PHASE,
                     "[init_trade_streams] Dataset ready: " + label,
                     nlohmann::json{});
            dataset->mark_init_sync_done();
            aggregator.add_ohlcv_dataset(dataset);
        }).detach();
    }
}

void MarketDataStreamManager::init_order_books(
    const std::vector<ResolvedMarketSymbol>& books) {
    for (const auto& book : books) {
        const auto& orderbook = CcxtProxy::instance().get_full_orderbook(
            book.exchange_name, book.market_type, book.symbol);
        if (orderbook.has_value()) {
            MarketDataStreams::get_instance().enqueue_full_refresh(
                book.exchange_name, book.market_type, book.symbol,
                orderbook.value());
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void MarketDataStreamManager::update_polymarket_subscriptions(
    const std::vector<std::string>& asset_ids) {
    if (asset_ids.empty()) return;

    nlohmann::json sub_msg;
    sub_msg["assets_ids"] = asset_ids;
    sub_msg["type"] = "market";
    sub_msg["custom_feature_enabled"] = true;
    MarketData::MarketDataStreams::get_instance().send_polymarket_message(
        sub_msg.dump());
}

void MarketDataStreamManager::init_state() {
    const auto config =
        MarketDataCoverage::load(Config::getInstance().get_coverage_path());
    const ResolvedCoverage coverage = resolve(config, ResolvedCoverage{});
    validate_state_machine_dependencies(coverage);
    require_polygon_key_if_streamed(coverage);
    DataRecorder::getInstance().set_recording_keys(coverage.recording_keys);
    {
        std::lock_guard<std::mutex> lock(coverage_mutex_);
        coverage_ = coverage;
    }
    LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_INIT_PHASE,
             "[init_state] coverage resolved",
             (nlohmann::json{
                 {"path", Config::getInstance().get_coverage_path()},
                 {"ohlcv_datasets", coverage.ohlcv_datasets.size()},
                 {"trade_bar_datasets", coverage.trade_bar_datasets.size()},
                 {"exchange_bar_connections",
                  coverage.exchange_bar_connections.size()},
                 {"trade_connections", coverage.trade_connections.size()},
                 {"book_ticker_connections",
                  coverage.book_ticker_connections.size()},
                 {"lob_connections", coverage.lob_connections.size()},
                 {"mini_ticker_connections",
                  coverage.mini_ticker_connections.size()},
                 {"recording_keys", coverage.recording_keys.size()},
                 {"polymarket", coverage.polymarket}}));

    if (MarketData::StateMachineDispatcher::get_instance()
            .get_machines()
            .empty()) {
        LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_INIT_PHASE,
                 "[init_state] No SMs registered; skipping dispatcher start",
                 nlohmann::json{});
    } else {
        LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_INIT_PHASE,
                 "[init_state] Starting state machine dispatcher",
                 nlohmann::json{});
        MarketData::StateMachineDispatcher::get_instance().start();
    }

    if (!is_called_from_trading_client) {
        std::thread(trim_recorder_memory_loop).detach();
        std::thread(second_bar_writer_loop, coverage.polygon_second_bars)
            .detach();
    }

    if (!coverage.ohlcv_datasets.empty()) {
        LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_INIT_PHASE,
                 "[init_state] Starting OHLCV dataset init (background)",
                 nlohmann::json{});
        std::thread([this, datasets = coverage.ohlcv_datasets]() {
            this->init_ohlcv_datasets(datasets);
        }).detach();
    }

    LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_INIT_PHASE,
             "[init_state] Starting stream init (background)",
             nlohmann::json{});
    std::thread([this, coverage]() {
        MarketData::MarketDataStreams& streams =
            MarketData::MarketDataStreams::get_instance();
        for (const auto& config :
             to_streams_configs(coverage.exchange_bar_connections))
            streams.add_ohlcv_config(config);
        for (const auto& config :
             to_streams_configs(coverage.trade_connections))
            streams.add_trade_config(config);
        this->init_trade_bar_datasets(coverage.trade_bar_datasets);
        for (const auto& config :
             to_streams_configs(coverage.mini_ticker_connections))
            streams.add_ticker_config(config);
        for (const auto& book : coverage.orderbooks)
            Core::Engine::getInstance().init_orderbook(
                book.exchange_name, book.market_type, book.symbol);
        for (const auto& config :
             to_streams_configs(coverage.book_ticker_connections))
            streams.add_book_ticker_config(config);
        for (const auto& config :
             to_streams_configs(coverage.lob_connections))
            streams.add_lob_config(config);
        if (coverage.polymarket)
            streams.add_polymarket_config(polymarket_ws_url());

        LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_INIT_PHASE,
                 "[init_state] all configs added, starting streams",
                 nlohmann::json{});
        streams.set_is_ran_from_live_infra(this->is_called_from_trading_client);
        streams.set_log_callbacks(this->error_log_callback,
                                  this->info_log_callback);
        streams.start_streams();

        if (!coverage.rest_orderbook_snapshots.empty()) {
            LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_INIT_PHASE,
                     "[init_state] streams started, starting order books",
                     nlohmann::json{});
            std::thread([this, books = coverage.rest_orderbook_snapshots]() {
                this->init_order_books(books);
            }).detach();
        }
    }).detach();
}

void MarketDataStreamManager::apply_change(const CoverageChange& change) {
    auto& streams = MarketDataStreams::get_instance();
    streams.retire_streams(
        WsStreamConstants::OHLCV_STREAM_TYPE,
        to_streams_configs(change.retired_exchange_bar_connections));
    streams.retire_streams(WsStreamConstants::TRADE_STREAM_TYPE,
                           to_streams_configs(change.retired_trade_connections));
    streams.retire_streams(
        WsStreamConstants::BOOK_TICKER_STREAM_TYPE,
        to_streams_configs(change.retired_book_ticker_connections));
    streams.retire_streams(WsStreamConstants::LOB_STREAM_TYPE,
                           to_streams_configs(change.retired_lob_connections));
    streams.retire_streams(
        "ticker", to_streams_configs(change.retired_mini_ticker_connections));

    // Datasets and books exist before the connections that feed them.
    if (!change.added_ohlcv_datasets.empty())
        init_ohlcv_datasets(change.added_ohlcv_datasets);
    init_trade_bar_datasets(change.added_trade_bar_datasets);
    for (const auto& book : change.added_orderbooks)
        Core::Engine::getInstance().init_orderbook(
            book.exchange_name, book.market_type, book.symbol);

    streams.open_streams(
        WsStreamConstants::OHLCV_STREAM_TYPE,
        to_streams_configs(change.opened_exchange_bar_connections));
    streams.open_streams(WsStreamConstants::TRADE_STREAM_TYPE,
                         to_streams_configs(change.opened_trade_connections));
    streams.open_streams(
        WsStreamConstants::BOOK_TICKER_STREAM_TYPE,
        to_streams_configs(change.opened_book_ticker_connections));
    streams.open_streams(WsStreamConstants::LOB_STREAM_TYPE,
                         to_streams_configs(change.opened_lob_connections));
    streams.open_streams(
        "ticker", to_streams_configs(change.opened_mini_ticker_connections));

    if (!change.added_rest_orderbook_snapshots.empty()) {
        std::thread([this, books = change.added_rest_orderbook_snapshots]() {
            this->init_order_books(books);
        }).detach();
    }
}

void MarketDataStreamManager::apply_split_requests() {
    const std::filesystem::path directory =
        Config::getInstance().get_data_dir_root() +
        "/caches/data_recorder/split_adjustments";
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error)) return;
    for (const auto& entry :
         std::filesystem::directory_iterator(directory, error)) {
        if (entry.path().extension() != ".json") continue;
        try {
            std::ifstream file(entry.path());
            const auto request = nlohmann::json::parse(file);
            // Memory loaded after the files were rescaled is already right.
            if (request.at("created_at_ms").get<long long>() >
                process_start_ms_) {
                const auto exchange = request.at("exchange").get<std::string>();
                const auto market = request.at("market_type").get<std::string>();
                const auto symbol = request.at("symbol").get<std::string>();
                const auto cutoff = request.at("cutoff_ms").get<long long>();
                const auto ratio = request.at("ratio").get<double>();
                int rescaled = 0;
                for (const auto& dataset :
                     DatasetsManager::get_instance().get_ohlcv_datasets()) {
                    if (dataset->meta.exchange_name != exchange ||
                        dataset->meta.market_type != market ||
                        dataset->meta.symbol != symbol)
                        continue;
                    dataset->rescale_for_split(cutoff, ratio);
                    ++rescaled;
                }
                LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_INIT_PHASE,
                         "applied split to in-memory bars",
                         (nlohmann::json{{"symbol", symbol},
                                         {"ratio", ratio},
                                         {"cutoff_ms", cutoff},
                                         {"datasets", rescaled}}));
            }
        } catch (const std::exception& parse_error) {
            LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_INIT_PHASE,
                      "unreadable split request " + entry.path().string() +
                          ": " + parse_error.what(),
                      nlohmann::json{});
        }
        std::filesystem::remove(entry.path(), error);
    }
}

void MarketDataStreamManager::refresh_coverage() {
    if (is_called_from_trading_client) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_INIT_PHASE,
                  "refresh_coverage refused in the trading client; coverage "
                  "changes there take effect at restart",
                  nlohmann::json{});
        return;
    }
    if (!is_initialized.load()) return;
    std::lock_guard<std::mutex> refresh_lock(refresh_mutex_);
    apply_split_requests();
    ResolvedCoverage current;
    {
        std::lock_guard<std::mutex> lock(coverage_mutex_);
        current = coverage_;
    }
    ResolvedCoverage next;
    try {
        const auto config =
            MarketDataCoverage::load(Config::getInstance().get_coverage_path());
        next = resolve(config, current);
        validate_state_machine_dependencies(next);
        require_polygon_key_if_streamed(next);
    } catch (const std::exception& error) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_INIT_PHASE,
                  std::string("coverage reload rejected, keeping current: ") +
                      error.what(),
                  nlohmann::json{});
        return;
    }

    // The polymarket stream is opened once at startup.
    if (next.polymarket != current.polymarket) {
        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_INIT_PHASE,
                  "polymarket coverage change takes effect at restart",
                  nlohmann::json{});
        next.polymarket = current.polymarket;
    }
    const CoverageChange change = MarketDataCoverage::diff(current, next);
    if (change.empty()) return;

    LOG_INFO(
        Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_INIT_PHASE,
        "applying coverage change",
        (nlohmann::json{
            {"added_ohlcv_datasets", change.added_ohlcv_datasets.size()},
            {"removed_ohlcv_datasets_until_restart",
             change.removed_ohlcv_datasets.size()},
            {"added_trade_bar_datasets", change.added_trade_bar_datasets.size()},
            {"removed_trade_bar_datasets_until_restart",
             change.removed_trade_bar_datasets.size()},
            {"opened_connections",
             change.opened_exchange_bar_connections.size() +
                 change.opened_trade_connections.size() +
                 change.opened_book_ticker_connections.size() +
                 change.opened_lob_connections.size() +
                 change.opened_mini_ticker_connections.size()},
            {"retired_connections",
             change.retired_exchange_bar_connections.size() +
                 change.retired_trade_connections.size() +
                 change.retired_book_ticker_connections.size() +
                 change.retired_lob_connections.size() +
                 change.retired_mini_ticker_connections.size()},
            {"recording_keys", next.recording_keys.size()}}));
    // Recording narrows first, so a symbol leaving coverage stops being
    // recorded even though a connection it shares stays open.
    DataRecorder::getInstance().set_recording_keys(next.recording_keys);
    apply_change(change);
    std::lock_guard<std::mutex> lock(coverage_mutex_);
    coverage_ = std::move(next);
}

void MarketDataStreamManager::start_stream() {
    if (is_initialized) {
        return;
    }
    // Test-mode kill switch: e2e tests inject market data via the engine
    // control plane and don't need public WS subscriptions. Set by
    // infra_e2e_testing's make_engine_process by default.
    const char* disable = std::getenv("DISABLE_PUBLIC_MARKET_DATA_STREAMS");
    if (disable != nullptr && std::string(disable) == "1") {
        LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_INIT_PHASE,
                 "[MDSM] DISABLE_PUBLIC_MARKET_DATA_STREAMS=1 — skipping "
                 "public WS subscriptions + OHLCV dataset init",
                 nlohmann::json{});
        is_initialized = true;
        return;
    }
    this->init_state();
    is_initialized = true;
}

}  // namespace MarketData
