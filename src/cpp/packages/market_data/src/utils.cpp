#include "market_data/utils.h"

#include <date/tz.h>

#include <chrono>
#include <sstream>
#include <unordered_set>

#include "env_utils.h"
#include "market_data/bar_close_policy.h"
#include "market_data/bar_types_constant.h"
#include "market_data/config.h"
#include "market_data/ohlcv_bar_meta.h"
#include "market_data/stream_info.h"
#include "market_data/trade_streams.h"
#include "ws_stream.h"

static std::string get_stream_base_url(const std::string& exchange_name,
                                       const std::string& market_type,
                                       const std::string& stream_type) {
    if (exchange_name == "binance") {
        if (market_type == "spot") {
            return EnvUtils::env_or("BINANCE_PUBLIC_WS_BASE_URL",
                                    "wss://stream.binance.com:9443/ws");
        } else if (market_type == "swap") {
            // Binance restructured futures WS endpoints into three routed
            // paths. The unrouted base URL still serves /public-class
            // streams (bookTicker, aggTrade, depth) but /market-class
            // streams (kline, miniTicker, ticker) silently push zero
            // messages without the explicit /market prefix.
            // Reference: https://developers.binance.com/docs/derivatives/usds-margined-futures/websocket-market-streams
            const bool is_market_routed =
                stream_type == WsStreamConstants::OHLCV_STREAM_TYPE ||
                stream_type == "ticker";
            if (is_market_routed) {
                return EnvUtils::env_or(
                    "BINANCE_MARKET_WS_FUTURES_BASE_URL",
                    "wss://fstream.binance.com/market/ws");
            }
            return EnvUtils::env_or("BINANCE_PUBLIC_WS_FUTURES_BASE_URL",
                                    "wss://fstream.binance.com/ws");
        }
    }

    if (exchange_name == "bitget") {
        if (market_type == "spot" || market_type == "swap") {
            return EnvUtils::env_or("BITGET_PUBLIC_WS_BASE_URL",
                                    "wss://ws.bitget.com/v2/ws/public");
        }
    }

    if (exchange_name == "bybit") {
        if (market_type == "spot") {
            return EnvUtils::env_or("BYBIT_PUBLIC_WS_SPOT_BASE_URL",
                                    "wss://stream.bybit.com/v5/public/spot");
        } else if (market_type == "swap") {
            return EnvUtils::env_or("BYBIT_PUBLIC_WS_LINEAR_BASE_URL",
                                    "wss://stream.bybit.com/v5/public/linear");
        }
    }

    if (exchange_name == "polygon") {
        if (market_type == "stock") {
            return EnvUtils::env_or("POLYGON_PUBLIC_WS_BASE_URL",
                                    "wss://socket.polygon.io/stocks");
        }
    }

    if (exchange_name == "gateio") {
        if (market_type == "spot") {
            return EnvUtils::env_or("GATEIO_PUBLIC_WS_BASE_URL",
                                    "wss://api.gateio.ws/ws/v4/");
        }
    }

    if (exchange_name == "aster") {
        if (market_type == "swap") {
            return EnvUtils::env_or("ASTER_PUBLIC_WS_BASE_URL",
                                    "wss://fstream.asterdex.com/ws");
        }
    }

    if (exchange_name == "hyperliquid") {
        if (market_type == "swap") {
            return EnvUtils::env_or("HYPERLIQUID_PUBLIC_WS_BASE_URL",
                                    "wss://api.hyperliquid.xyz/ws");
        }
    }

    return "";
}

std::string MarketData::get_stream_url(
    const std::string& exchange_name, const std::string& market_type,
    const std::string& stream_type, std::optional<std::string> interval,
    const std::vector<StreamInfo>& stream_infos) {
    std::string base_url =
        get_stream_base_url(exchange_name, market_type, stream_type);
    if (base_url.empty()) {
        return "";
    }
    return base_url;
}

std::vector<MarketData::StreamInfo> MarketData::get_stream_infos(
    const std::string& exchange_name, const std::string& market_type,
    const std::string& stream_type, const std::vector<StreamInfo>& streams) {
    std::vector<StreamInfo> stream_infos;
    for (const auto& stream : streams) {
        if (stream.exchange_name == exchange_name &&
            stream.market_type == market_type) {
            stream_infos.push_back(stream);
        }
    }
    return stream_infos;
}

std::string MarketData::get_raw_trade_ticks_base_dir(
    const std::string& symbol, const std::string& exchange_name,
    const std::string& market_type) {
    return Config::getInstance().get_data_dir_root() + "/raw/trades/" +
           exchange_name + "/" + market_type + "/" + symbol + "/parquets";
}

std::string MarketData::get_derived_bars_base_dir(
    const std::string& symbol, const std::string& exchange_name,
    const std::string& market_type, const BarClosePolicy& bar_close_policy,
    bool omit_data_dir_root) {
    return (omit_data_dir_root ? ""
                               : Config::getInstance().get_data_dir_root()) +
           "/derived/bars/" + exchange_name + "/" + market_type + "/" +
           bar_close_policy.get_bar_type_and_close_threshold() + "/" + symbol;
}

std::string MarketData::get_funding_rate_base_dir(
    const std::string& symbol, const std::string& exchange_name,
    const std::string& market_type, bool omit_data_dir_root) {
    return (omit_data_dir_root ? ""
                               : Config::getInstance().get_data_dir_root()) +
           "/raw/funding/" + exchange_name + "/" + market_type + "/" + symbol;
}

std::vector<std::string> MarketData::get_unique_symbols_from_bar_dir(
    const std::string& market_data_library_root,
    const std::string& exchange_name, const std::string& market_type) {
    std::unordered_set<std::string> unique_symbols;
    std::vector<MarketData::BarClosePolicy> bar_close_policies =
        MarketData::DEFAULT_LIVE_STREAMED_BARS;

    for (const auto& bar_close_policy : bar_close_policies) {
        std::string bar_base_directory_path =
            MarketData::get_bar_base_directory_path(exchange_name, market_type,
                                                    bar_close_policy);

        if (!std::filesystem::exists(market_data_library_root +
                                     bar_base_directory_path)) {
            continue;
        }

        for (const auto& file : std::filesystem::directory_iterator(
                 market_data_library_root + bar_base_directory_path)) {
            if (file.path().filename().string() == ".DS_Store") {
                continue;
            }
            unique_symbols.insert(file.path().filename().string());
        }
    }

    std::vector<std::string> symbols(unique_symbols.begin(),
                                     unique_symbols.end());
    std::sort(symbols.begin(), symbols.end());
    return symbols;
}

std::string MarketData::convert_interval_ms_to_str(int interval_ms) {
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

std::string MarketData::convert_interval_ms_to_bitget_str_format(
    int interval_ms) {
    switch (interval_ms) {
        case 60 * 1000:  // 1 minute
            return "1m";
        case 5 * 60 * 1000:  // 5 minutes
            return "5m";
        case 15 * 60 * 1000:  // 15 minutes
            return "15m";
        case 30 * 60 * 1000:  // 30 minutes
            return "30m";
        case 60 * 60 * 1000:  // 1 hour
            return "1H";
        case 4 * 60 * 60 * 1000:  // 4 hours
            return "4H";
        case 6 * 60 * 60 * 1000:  // 6 hours
            return "6H";
        case 12 * 60 * 60 * 1000:  // 12 hours
            return "12H";
        case 24 * 60 * 60 * 1000:  // 1 day
            return "1D";
        case 3 * 24 * 60 * 60 * 1000:  // 3 days
            return "3D";
        case 7 * 24 * 60 * 60 * 1000:  // 1 week
            return "1W";
        default:
            throw std::invalid_argument("Invalid interval_ms provided");
    }
}

long long MarketData::convert_interval_str_to_ms(const std::string& interval) {
    if (interval == "1m") {
        return 60 * 1000;
    }
    if (interval == "3m") {
        return 3 * 60 * 1000;
    }
    if (interval == "5m") {
        return 5 * 60 * 1000;
    }
    if (interval == "15m") {
        return 15 * 60 * 1000;
    }
    if (interval == "30m") {
        return 30 * 60 * 1000;
    }
    if (interval == "1h") {
        return 60 * 60 * 1000;
    }
    if (interval == "2h") {
        return 2 * 60 * 60 * 1000;
    }
    if (interval == "4h") {
        return 4 * 60 * 60 * 1000;
    }
    if (interval == "6h") {
        return 6 * 60 * 60 * 1000;
    }

    if (interval == "1d") {
        return 24 * 60 * 60 * 1000;
    }
    throw std::invalid_argument("Invalid interval provided");
}

int MarketData::convert_ms_to_num_of_minutes(long long interval_ms) {
    return interval_ms / (60000);
}

long long MarketData::get_latest_fully_completed_kline_open_ms(
    long long interval_ms) {
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
    return now_ms - (now_ms % interval_ms) -
           (now_ms % interval_ms == 0 ? interval_ms : 0) - interval_ms;
}

bool MarketData::symbol_ends_in_usdc(const std::string& symbol) {
    return symbol.length() >= 4 && symbol.substr(symbol.length() - 4) == "USDC";
}

bool MarketData::is_time_ms_within_real_trading_hours(long long time_ms) {
    date::sys_time<std::chrono::milliseconds> tp{
        std::chrono::milliseconds{time_ms}};

    const auto* ny_tz = date::locate_zone("America/New_York");
    date::zoned_time ny_time{ny_tz, tp};

    auto local_tm = ny_time.get_local_time();
    auto local_days = date::floor<date::days>(local_tm);
    auto time_of_day = date::make_time(local_tm - local_days);
    auto ymd = date::year_month_day{local_days};
    auto wd = date::weekday{local_days};

    if (wd == date::Sunday || wd == date::Saturday) {
        return false;
    }

    int hour = time_of_day.hours().count();
    int minute = time_of_day.minutes().count();

    if (hour < 9 || (hour == 9 && minute < 30)) {
        return false;
    }
    if (hour >= 16) {
        return false;
    }

    return true;
}

// 04:00-20:00 ET on weekdays — the window Polygon publishes AM aggregates in.
// Holidays are deliberately not modelled: outside this window we only hold the
// staleness clock, and a holiday simply costs one harmless reconnect cycle.
bool MarketData::is_time_ms_within_extended_trading_hours(long long time_ms) {
    date::sys_time<std::chrono::milliseconds> tp{
        std::chrono::milliseconds{time_ms}};

    const auto* ny_tz = date::locate_zone("America/New_York");
    date::zoned_time ny_time{ny_tz, tp};

    auto local_tm = ny_time.get_local_time();
    auto local_days = date::floor<date::days>(local_tm);
    auto time_of_day = date::make_time(local_tm - local_days);
    auto wd = date::weekday{local_days};

    if (wd == date::Sunday || wd == date::Saturday) {
        return false;
    }

    int hour = time_of_day.hours().count();
    return hour >= 4 && hour < 20;
}

bool MarketData::is_market_open_bar(long long time_ms) {
    date::sys_time<std::chrono::milliseconds> tp{
        std::chrono::milliseconds{time_ms}};
    const auto* ny_tz = date::locate_zone("America/New_York");
    date::zoned_time ny_time{ny_tz, tp};

    auto local_tm = ny_time.get_local_time();
    auto local_days = date::floor<date::days>(local_tm);
    auto time_of_day = date::make_time(local_tm - local_days);
    auto wd = date::weekday{local_days};

    if (wd == date::Saturday || wd == date::Sunday) {
        return false;
    }

    int hour = time_of_day.hours().count();
    int minute = time_of_day.minutes().count();

    return hour == 9 && minute == 30;
}

std::optional<long long> MarketData::get_market_open_time_ms(
    long long current_time_ms) {
    date::sys_time<std::chrono::milliseconds> tp{
        std::chrono::milliseconds{current_time_ms}};
    const auto* ny_tz = date::locate_zone("America/New_York");
    date::zoned_time ny_time{ny_tz, tp};

    auto local_tm = ny_time.get_local_time();
    auto local_days = date::floor<date::days>(local_tm);
    auto wd = date::weekday{local_days};

    // Skip weekends
    if (wd == date::Saturday) {
        return std::nullopt;
    }
    if (wd == date::Sunday) {
        return std::nullopt;
    }

    // Construct the 09:30:00 local time for this day
    date::local_time<std::chrono::milliseconds> open_local =
        local_days + std::chrono::hours{9} + std::chrono::minutes{30};

    // Convert back to UTC (sys_time) then to ms
    date::sys_time<std::chrono::milliseconds> open_sys =
        ny_time.get_time_zone()->to_sys(open_local);

    return open_sys.time_since_epoch().count();
}

std::optional<long long> MarketData::get_market_close_time_ms(
    long long current_time_ms) {
    date::sys_time<std::chrono::milliseconds> tp{
        std::chrono::milliseconds{current_time_ms}};
    const auto* ny_tz = date::locate_zone("America/New_York");
    date::zoned_time ny_time{ny_tz, tp};

    auto local_tm = ny_time.get_local_time();
    auto local_days = date::floor<date::days>(local_tm);
    auto wd = date::weekday{local_days};

    if (wd == date::Saturday) {
        return std::nullopt;
    }
    if (wd == date::Sunday) {
        return std::nullopt;
    }

    date::local_time<std::chrono::milliseconds> close_local =
        local_days + std::chrono::hours{16};

    date::sys_time<std::chrono::milliseconds> close_sys =
        ny_time.get_time_zone()->to_sys(close_local);

    return close_sys.time_since_epoch().count();
}

std::optional<long long> MarketData::get_time_ms_until_market_close(
    long long current_time_ms) {
    auto market_close_time_ms = get_market_close_time_ms(current_time_ms);
    if (!market_close_time_ms.has_value()) {
        return std::nullopt;
    }
    if (current_time_ms > market_close_time_ms.value()) {
        return std::nullopt;
    }
    return market_close_time_ms.value() - current_time_ms;
}

std::chrono::system_clock::time_point MarketData::get_dataset_read_start_time(
    const MarketData::OhlcvBarMeta& meta,
    std::chrono::system_clock::time_point default_start) {
    // Per-bar-type max history to load at init. Driven by the longest
    // rolling-window any strategy reads off the dataset (today: panic-
    // clarity-4h's 7-day OFI; 30-day cap leaves margin for windows up
    // to ~3 weeks). If a future strategy needs more, widen here.
    //
    // Sparse bar types (exchange klines, 15m, 1m) are unbounded — the
    // total row count for `default_start`-to-now is small enough that
    // scan_partitions doesn't risk OOM, and some callers (backtest,
    // historical feature gen) genuinely need the full range.
    std::optional<int> max_days;
    const bool is_binance_swap_1s =
        meta.exchange_name == "binance" && meta.market_type == "swap" &&
        meta.bar_close_policy.is_time_bar &&
        meta.bar_close_policy.time_ms == 1000;
    const bool is_binance_spot_100ms =
        meta.exchange_name == "binance" && meta.market_type == "spot" &&
        meta.bar_close_policy.is_time_bar &&
        meta.bar_close_policy.time_ms == 100;
    const bool is_gateio_spot_5s =
        meta.exchange_name == "gateio" && meta.market_type == "spot" &&
        meta.bar_close_policy.is_time_bar &&
        meta.bar_close_policy.time_ms == 5000;
    if (is_binance_swap_1s) {
        max_days = 30;  // 30d × 86_400 ≈ 2.6M rows / symbol
    } else if (is_binance_spot_100ms) {
        max_days = 3;   // 3d × 864_000 ≈ 2.6M rows / symbol
    } else if (is_gateio_spot_5s) {
        max_days = 60;  // 60d × 17_280 ≈ 1.0M rows / symbol
    }
    if (!max_days.has_value()) {
        return default_start;
    }
    const auto now = std::chrono::system_clock::now();
    const auto clamp = now - std::chrono::hours(24 * max_days.value());
    return default_start < clamp ? clamp : default_start;
}

// Upper bound on klines held in memory per dataset, for venues whose universe
// is too wide to keep full history resident. Polygon alone is ~12.5k symbols
// across two bar types; at the 700k default that is hundreds of GB, and
// fetch_from_proxy accumulates every page before its first write. 0 means
// "no venue cap" — the existing per-dataset cap applies unchanged.
long long MarketData::get_max_klines_to_keep(const OhlcvBarMeta& meta) {
    if (meta.exchange_name != "polygon") return 0LL;
    if (meta.bar_close_policy.time_ms == 60000) return 5000LL;   // ~5 sessions
    if (meta.bar_close_policy.time_ms == 86400000) return 1000LL;  // ~4 years
    return 0LL;
}

long long MarketData::get_min_klines_to_keep_after_init(
    const OhlcvBarMeta& meta) {
    // 750K bars on binance/swap/1s = ~8.7 days, sized to cover a 7-day
    // rolling window with ~1.7-day margin so the window is reliably
    // populated immediately after init (no transient bit-identical values
    // across 3d/7d RollingOFI buffers during the first hours of warmup).
    const bool is_binance_swap_1s =
        meta.exchange_name == "binance" && meta.market_type == "swap" &&
        meta.bar_close_policy.is_time_bar &&
        meta.bar_close_policy.time_ms == 1000;
    if (is_binance_swap_1s) return 750000LL;
    // 15m: 100 daily rows plus cohorts rebuilt on start, with margin;
    // spot features look back 30 days.
    const bool is_binance_15m = meta.exchange_name == "binance" &&
                                meta.bar_close_policy.is_time_bar &&
                                meta.bar_close_policy.time_ms == 900000;
    if (is_binance_15m && meta.market_type == "swap") return 11000LL;
    if (is_binance_15m && meta.market_type == "spot") return 4000LL;
    // Daily: 200 consecutive bars, read at 400.
    if (meta.exchange_name == "binance" && meta.market_type == "swap" &&
        meta.bar_close_policy.is_time_bar &&
        meta.bar_close_policy.time_ms == 86400000) {
        return 400LL;
    }
    return 0LL;
}
