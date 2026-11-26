#include "market_data/market_data_streams.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <fstream>
#include <future>
#include <memory>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "binance_utils.h"
#include "common_types/log_event_types.h"
#include "market_data/config.h"
#include "market_data/data_recorder.h"
#include "market_data/state_machine_dispatcher.h"
#include "market_data/utils.h"
#include "market_making/engine.h"
#include "market_making/rate_limited_emitter.h"
#include "ccxt_proxy.h"
#include "ccxt_utils.h"
#include "datetime_utils.h"
#include "disk_logging/log.h"
#include "ip_rotator.h"
#include "market_data/bar.h"
#include "market_data/dataset_manager.h"
#include "market_data/depth_history.h"
#include "market_data/lighter_order_book.h"
#include "market_data/forming_klines.h"
#include "market_data/normalized_trade.h"
#include "market_data/polygon_second_bars.h"
#include "market_data/utils.h"
#include "market_making/dtypes/exchange_info.h"
#include "market_making/engine.h"
#include "market_making/utils.h"
#include "threadpool.h"
#include "lighter_markets.h"
#include "ws_stream.h"

// Rate-limited emitter for per-message parse errors. These live on hot
// consumer threads — a bad-message burst at venue wire speed would be a
// 1kHz event rate. One emit per 10s per (exchange, market, error_class)
// keeps the DB and log file readable while preserving the suppressed-count
// signal agents need to detect storms. Engine path is the lock-free
// trading_logs queue; a no-op when engine is not initialized (e.g. inside
// market_data_recorder binary).
namespace {
Core::RateLimitedEmitter g_parse_error_emitter;

void emit_parse_error(const std::string& exchange, const std::string& market,
                       const std::string& error_class,
                       const std::string& what) {
    g_parse_error_emitter.emit_if_allowed(
        error_class + "|" + exchange + "|" + market, 10000,
        [&](uint64_t suppressed) {
            nlohmann::json md = {
                {"exchange_name", exchange},
                {"market_type", market},
                {"error_class", error_class},
                {"what", what},
                {"events_suppressed_since_last_emit", suppressed},
            };
            Core::Engine::getInstance().create_system_log(
                "", Core::LOG_LEVEL_ERROR,
                "Market data parse error: " + error_class,
                Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_STREAM_PARSE_ERROR,
                md, -1, exchange, market);
        });
}

void emit_lighter_out_of_sync(const std::string &symbol,
                              const std::string &exchange,
                              const std::string &market,
                              const std::string &reason) {
    g_parse_error_emitter.emit_if_allowed(
        "orderbook_out_of_sync|" + exchange + "|" + market + "|" + symbol,
        30000, [&](uint64_t suppressed) {
            nlohmann::json md = {
                {"detection_reason", reason},
                {"events_suppressed_since_last_emit", suppressed},
            };
            Core::Engine::getInstance().create_system_log(
                symbol, Core::LOG_LEVEL_WARNING,
                "Lighter order book out of sync (" + reason +
                    "); resubscribing for a snapshot",
                Core::TRADING_LOG_EVENT_TYPE_ORDERBOOK_OUT_OF_SYNC, md, -1,
                exchange, market);
        });
}

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void append_number(std::string &out, double value) {
    char buf[32];
    const auto res = std::to_chars(buf, buf + sizeof(buf), value);
    out.append(buf, res.ptr);
}

void append_number(std::string &out, long long value) {
    char buf[24];
    const auto res = std::to_chars(buf, buf + sizeof(buf), value);
    out.append(buf, res.ptr);
}

// Binance depthUpdate level shape: [["price","qty"],...].
void append_pair_levels(std::string &out,
                        const std::vector<MarketData::DepthLevel> &levels) {
    out += '[';
    for (size_t i = 0; i < levels.size(); ++i) {
        if (i) out += ',';
        out += "[\"";
        append_number(out, levels[i].price);
        out += "\",\"";
        append_number(out, levels[i].quantity);
        out += "\"]";
    }
    out += ']';
}

// Full-refresh level shape: [{"price":p,"quantity":q},...].
void append_object_levels(std::string &out,
                          const std::vector<MarketData::DepthLevel> &levels) {
    out += '[';
    for (size_t i = 0; i < levels.size(); ++i) {
        if (i) out += ',';
        out += "{\"price\":";
        append_number(out, levels[i].price);
        out += ",\"quantity\":";
        append_number(out, levels[i].quantity);
        out += '}';
    }
    out += ']';
}

constexpr long long LIGHTER_RESUBSCRIBE_MIN_INTERVAL_MS = 2000;
constexpr long long LIGHTER_SNAPSHOT_WAIT_MS = 5000;
constexpr int LIGHTER_MAX_CROSSED_UPDATES = 20;
constexpr long long LIGHTER_PRICE_UPDATE_INTERVAL_MS = 1000;
}  // namespace

namespace MarketData {
struct LighterBookStream {
    LighterOrderBook book;
    std::string symbol;
    std::string depth_key;
    long long last_resubscribe_ms = 0;
    int crossed_streak = 0;
};
}  // namespace MarketData

MarketData::MarketDataStreams::MarketDataStreams() noexcept
    : bar_aggregator(BarAggregator::getInstance()),
      bba_notify_queue_(65536) {
    book_states_ = std::unique_ptr<BbaSlot[]>(new BbaSlot[MAX_BBA_SLOTS]());
    bba_slot_metadata_ =
        std::unique_ptr<BbaSlotMeta[]>(new BbaSlotMeta[MAX_BBA_SLOTS]());
}

MarketData::MarketDataStreams::~MarketDataStreams() noexcept {
    // Destroy streams
}

void MarketData::MarketDataStreams::add_ohlcv_config(
    const StreamsConfig &new_config) {
    this->ohlcv_configs.push_back(new_config);
}

void MarketData::MarketDataStreams::add_lob_config(
    const StreamsConfig &new_config) {
    this->lob_configs.push_back(new_config);
}

void MarketData::MarketDataStreams::add_trade_config(
    const StreamsConfig &new_config) {
    this->trade_configs.push_back(new_config);
}

void MarketData::MarketDataStreams::add_book_ticker_config(
    const StreamsConfig &new_config) {
    this->book_ticker_configs.push_back(new_config);
}

void MarketData::MarketDataStreams::add_ticker_config(
    const StreamsConfig &new_config) {
    this->ticker_configs.push_back(new_config);
}

void MarketData::MarketDataStreams::add_polymarket_config(
    const std::string &ws_url) {
    this->polymarket_ws_url_ = ws_url;
}

std::shared_ptr<const MarketData::BbaSymbolSlots>
MarketData::MarketDataStreams::assign_bba_slots(
    const std::vector<StreamInfo> &stream_infos) {
    auto slots = std::make_shared<BbaSymbolSlots>();
    std::lock_guard<std::mutex> lock(bba_registry_mutex_);
    for (const auto &info : stream_infos) {
        const std::string key =
            info.exchange_name + ":" + info.market_type + ":" + info.symbol;
        auto it = bba_slot_ids_.find(key);
        if (it == bba_slot_ids_.end()) {
            if (next_bba_slot_id_ >= MAX_BBA_SLOTS) {
                throw std::runtime_error("BBA slots exhausted at " + key);
            }
            const uint32_t slot_id = next_bba_slot_id_++;
            bba_slot_metadata_[slot_id] = {info.symbol, info.exchange_name,
                                           info.market_type};
            it = bba_slot_ids_.emplace(key, slot_id).first;
        }
        // Lighter's ticker carries the bare coin ("s":"BTC") in the field the
        // handler scans, so its slot is keyed by the coin.
        if (info.exchange_name == "lighter" && info.symbol.size() > 4 &&
            info.symbol.compare(info.symbol.size() - 4, 4, "USDC") == 0) {
            (*slots)[info.symbol.substr(0, info.symbol.size() - 4)] =
                it->second;
        } else {
            (*slots)[info.symbol] = it->second;
        }
    }
    return slots;
}

static inline double sv_to_double(std::string_view sv) {
    double val = 0;
    std::from_chars(sv.data(), sv.data() + sv.size(), val);
    return val;
}

static long long get_staleness_threshold_ms(const std::string &stream_type) {
    if (stream_type == WsStreamConstants::BOOK_TICKER_STREAM_TYPE ||
        stream_type == WsStreamConstants::LOB_STREAM_TYPE) {
        return 30000;
    }
    if (stream_type == WsStreamConstants::TRADE_STREAM_TYPE) {
        return 60000;
    }
    return 300000;
}

void MarketData::MarketDataStreams::send_polymarket_message(
    const std::string &msg) {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    for (auto &stream_meta : streams) {
        if (stream_meta.stream_type ==
            WsStreamConstants::POLYMARKET_STREAM_TYPE) {
            if (!stream_meta.stream->is_disconnected()) {
                stream_meta.stream->send_message(msg);
            }
            return;
        }
    }
}

std::vector<MarketData::StreamStatus>
MarketData::MarketDataStreams::get_stream_statuses() const {
    std::lock_guard<std::mutex> lock(stream_statuses_mutex_);
    return stream_statuses_;
}

void MarketData::MarketDataStreams::send_polymarket_book_message(
    const std::string &msg) {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    for (auto &stream_meta : streams) {
        if (stream_meta.stream_type ==
            WsStreamConstants::POLYMARKET_BOOK_STREAM_TYPE) {
            if (!stream_meta.stream->is_disconnected()) {
                stream_meta.stream->send_message(msg);
            }
            return;
        }
    }
}

void MarketData::MarketDataStreams::resubscribe_lighter_order_book(
    const std::string &symbol, long long market_id) {
    const std::string channel = "order_book/" + std::to_string(market_id);
    std::lock_guard<std::mutex> lock(streams_mutex_);
    for (auto &stream_meta : streams) {
        if (stream_meta.stream_type != WsStreamConstants::LOB_STREAM_TYPE)
            continue;
        const bool carries_symbol = std::any_of(
            stream_meta.stream_infos.begin(), stream_meta.stream_infos.end(),
            [&](const StreamInfo &info) {
                return info.exchange_name == "lighter" && info.symbol == symbol;
            });
        if (!carries_symbol) continue;
        if (stream_meta.stream->is_disconnected()) return;
        stream_meta.stream->send_message(
            WsStream::lighter_subscribe_message("unsubscribe", channel));
        stream_meta.stream->send_message(
            WsStream::lighter_subscribe_message("subscribe", channel));
        return;
    }
}

void MarketData::MarketDataStreams::consume_lighter_lob_message(
    const MarketDataMsg &msg, simdjson::dom::parser &parser,
    std::unordered_map<long long, LighterBookStream> &books) {
    auto parsed = LighterBookMessage::parse(msg.msg, parser);
    if (!parsed) return;
    const long long received_ms = now_ms();

    auto &stream = books[parsed->market_id];
    if (stream.symbol.empty()) {
        const auto market = LighterDirect::LighterMarkets::instance().by_market_id(
            parsed->market_id);
        if (!market) return;
        stream.symbol = market->symbol;
        stream.depth_key = Core::Utils::get_symbol_exchange_market_key(
            stream.symbol, msg.exchange_name, msg.market_type);
    }

    auto request_snapshot = [&]() {
        if (received_ms - stream.last_resubscribe_ms <
            LIGHTER_RESUBSCRIBE_MIN_INTERVAL_MS)
            return;
        stream.last_resubscribe_ms = received_ms;
        resubscribe_lighter_order_book(stream.symbol, parsed->market_id);
    };

    const auto result = stream.book.apply(*parsed);
    using Result = LighterOrderBook::ApplyResult;
    if (result == Result::NOT_READY) {
        if (received_ms - stream.last_resubscribe_ms > LIGHTER_SNAPSHOT_WAIT_MS)
            request_snapshot();
        return;
    }
    if (result == Result::GAP) {
        DepthHistory::instance().mark_invalid(stream.depth_key, received_ms);
        emit_lighter_out_of_sync(stream.symbol, msg.exchange_name,
                                 msg.market_type, "seq_gap");
        request_snapshot();
        return;
    }
    if (result == Result::CROSSED) {
        if (++stream.crossed_streak >= LIGHTER_MAX_CROSSED_UPDATES) {
            stream.book.reset();
            stream.crossed_streak = 0;
            DepthHistory::instance().mark_invalid(stream.depth_key,
                                                  received_ms);
            emit_lighter_out_of_sync(stream.symbol, msg.exchange_name,
                                     msg.market_type, "crossed_book");
            request_snapshot();
            return;
        }
    } else {
        stream.crossed_streak = 0;
    }

    DepthHistory::instance().publish(
        stream.depth_key, stream.book.top(DepthHistory::MAX_LEVELS, received_ms,
                                          parsed->exchange_ms));

    auto &recorder = DataRecorder::getInstance();
    if (!recorder.is_enabled()) return;
    std::string line;
    if (result == Result::SNAPSHOT_APPLIED ||
        (result == Result::CROSSED &&
         parsed->kind == LighterBookMessage::Kind::SNAPSHOT)) {
        line.reserve(64 + 40 * (parsed->bids.size() + parsed->asks.size()));
        line += "{\"type\":\"full_refresh\",\"s\":\"" + stream.symbol +
                "\",\"E\":";
        append_number(line, parsed->exchange_ms);
        line += ",\"u\":";
        append_number(line, parsed->nonce);
        line += ",\"book\":{\"bids\":";
        append_object_levels(line, parsed->bids);
        line += ",\"asks\":";
        append_object_levels(line, parsed->asks);
        line += "}}";
        recorder.record_with_symbol("lob_snapshot", msg.exchange_name,
                                    msg.market_type, stream.symbol, line);
        return;
    }
    line.reserve(128 + 32 * (parsed->bids.size() + parsed->asks.size()));
    line += "{\"e\":\"depthUpdate\",\"E\":";
    append_number(line, parsed->exchange_ms);
    line += ",\"T\":";
    append_number(line, parsed->exchange_ms);
    line += ",\"s\":\"" + stream.symbol + "\",\"U\":";
    append_number(line, parsed->begin_nonce + 1);
    line += ",\"u\":";
    append_number(line, parsed->nonce);
    line += ",\"pu\":";
    append_number(line, parsed->begin_nonce);
    line += ",\"b\":";
    append_pair_levels(line, parsed->bids);
    line += ",\"a\":";
    append_pair_levels(line, parsed->asks);
    line += '}';
    recorder.record_with_symbol("lob_l2_update", msg.exchange_name,
                                msg.market_type, stream.symbol, line);
}

// {"channel":"trade:1","type":"update/trade","trades":[{"trade_id","price",
//   "size","timestamp","is_maker_ask",...}],"liquidation_trades":[...]}.
// The subscribed/trade snapshot replays history and is skipped.
void MarketData::MarketDataStreams::consume_lighter_trade_message(
    const MarketDataMsg &msg, simdjson::dom::element doc,
    std::unordered_map<long long, std::string> &symbols_by_market_id,
    std::unordered_map<long long, long long> &last_price_emit_ms) {
    std::string_view type, channel;
    if (doc["type"].get(type) != simdjson::SUCCESS || type != "update/trade" ||
        doc["channel"].get(channel) != simdjson::SUCCESS)
        return;
    const auto colon = channel.find(':');
    if (colon == std::string_view::npos) return;
    long long market_id = -1;
    const auto id = channel.substr(colon + 1);
    if (std::from_chars(id.data(), id.data() + id.size(), market_id).ec !=
        std::errc{})
        return;

    auto symbol_it = symbols_by_market_id.find(market_id);
    if (symbol_it == symbols_by_market_id.end()) {
        const auto market =
            LighterDirect::LighterMarkets::instance().by_market_id(market_id);
        if (!market) return;
        symbol_it = symbols_by_market_id.emplace(market_id, market->symbol).first;
    }
    const std::string &symbol = symbol_it->second;

    simdjson::dom::array trades;
    if (doc["trades"].get(trades) != simdjson::SUCCESS) return;
    const long long parsed_ms = now_ms();
    auto &recorder = DataRecorder::getInstance();
    double last_price = 0;
    long long last_ts = 0;

    for (auto item : trades) {
        std::string_view price_sv, size_sv;
        int64_t ts = 0, trade_id = 0;
        bool is_maker_ask = false;
        if (item["price"].get(price_sv) != simdjson::SUCCESS ||
            item["size"].get(size_sv) != simdjson::SUCCESS ||
            item["timestamp"].get(ts) != simdjson::SUCCESS ||
            item["is_maker_ask"].get(is_maker_ask) != simdjson::SUCCESS)
            continue;
        if (item["trade_id"].get(trade_id) != simdjson::SUCCESS) trade_id = 0;

        NormalizedParsedTrade trade;
        trade.symbol = symbol;
        trade.exchange_name = msg.exchange_name;
        trade.market_type = msg.market_type;
        trade.price = sv_to_double(price_sv);
        trade.quantity = sv_to_double(size_sv);
        trade.timestamp = ts;
        trade.parsed_timestamp = parsed_ms;
        trade.latency_ms = parsed_ms - ts;
        // A resting ask was hit: the buyer took liquidity.
        trade.is_buyer_maker = !is_maker_ask;
        trade.trade_id = std::to_string(trade_id);
        if (trade.price <= 0.0 || trade.quantity <= 0.0) continue;

        if (recorder.is_enabled()) {
            std::string line;
            line.reserve(128);
            line += "{\"t\":";
            append_number(line, static_cast<long long>(trade_id));
            line += ",\"p\":\"";
            line.append(price_sv);
            line += "\",\"q\":\"";
            line.append(size_sv);
            line += "\",\"T\":";
            append_number(line, static_cast<long long>(ts));
            line += trade.is_buyer_maker ? ",\"m\":true" : ",\"m\":false";
            line += ",\"s\":\"" + symbol + "\"}";
            recorder.record_with_symbol(WsStreamConstants::TRADE_STREAM_TYPE,
                                        msg.exchange_name, msg.market_type,
                                        symbol, line);
        }

        BarAggregator::getInstance().add_trade(trade, true);

        MarketEvent event;
        event.type = MarketEvent::TRADE;
        event.trade = {trade.symbol,         msg.exchange_name, msg.market_type,
                       trade.price,          trade.quantity,
                       trade.is_buyer_maker, trade.timestamp};
        StateMachineDispatcher::get_instance().enqueue(MarketEvent(event));
        if (is_ran_from_live_infra) live_event_callback_(std::move(event));

        last_price = trade.price;
        last_ts = ts;
        ++trade_msgs_count;
        trade_total_latency_to_parse_ms += trade.latency_ms;
    }

    // Lighter has no miniTicker; last trade price marks paper positions,
    // throttled to the miniTicker's 1/s cadence.
    if (!is_ran_from_live_infra || last_price <= 0) return;
    auto &last_emit = last_price_emit_ms[market_id];
    if (parsed_ms - last_emit < LIGHTER_PRICE_UPDATE_INTERVAL_MS) return;
    last_emit = parsed_ms;
    Core::MarketEvent market_event;
    market_event.type = Core::MarketEventType::PRICE_UPDATE;
    market_event.symbol = symbol;
    market_event.price = last_price;
    market_event.event_time = last_ts;
    market_event.exchange_name = msg.exchange_name;
    market_event.market_type = msg.market_type;
    Core::Engine::getInstance().process_market_event(market_event);
}

void MarketData::MarketDataStreams::handle_ohlcv_message(
    const std::string &msg, const std::string &exchange_name,
    const std::string &market_type) {
    DataRecorder::getInstance().record(
        WsStreamConstants::OHLCV_STREAM_TYPE, exchange_name, market_type, msg);
    MarketDataMsg ohlcv_msg;
    ohlcv_msg.exchange_name = exchange_name;
    ohlcv_msg.market_type = market_type;
    ohlcv_msg.msg = msg;
    ohlcv_queue.enqueue(ohlcv_msg);
}

void MarketData::MarketDataStreams::handle_lob_message(
    const std::string &msg, const std::string &exchange_name,
    const std::string &market_type) {
    MarketDataMsg lob_msg;
    lob_msg.exchange_name = exchange_name;
    lob_msg.market_type = market_type;
    lob_msg.msg = msg;
    lob_queue.enqueue(lob_msg);
}

void MarketData::MarketDataStreams::handle_trade_message(
    const std::string &msg, const std::string &exchange_name,
    const std::string &market_type) {
    MarketDataMsg trade_msg;
    trade_msg.exchange_name = exchange_name;
    trade_msg.market_type = market_type;
    trade_msg.msg = msg;
    trade_queue.enqueue(trade_msg);
}

void MarketData::MarketDataStreams::handle_ticker_message(
    const std::string &msg, const std::string &exchange_name,
    const std::string &market_type) {
    MarketDataMsg ticker_msg;
    ticker_msg.exchange_name = exchange_name;
    ticker_msg.market_type = market_type;
    ticker_msg.msg = msg;
    ticker_queue.enqueue(ticker_msg);
}

void MarketData::MarketDataStreams::enqueue_full_refresh(
    const std::string &exchange_name, const std::string &market_type,
    const std::string &symbol, const Core::OrderBookLevels &levels) {
    nlohmann::json snapshot;
    snapshot["type"] = "full_refresh";
    snapshot["s"] = symbol;
    snapshot["book"] = levels.to_json();
    DataRecorder::getInstance().record("lob_snapshot", exchange_name,
                                       market_type, snapshot.dump());

    MarketDataMsg msg;
    msg.exchange_name = exchange_name;
    msg.market_type = market_type;
    msg.symbol = symbol;
    msg.is_full_refresh = true;
    msg.full_refresh_orderbook = levels;
    lob_queue.enqueue(msg);
}

void MarketData::MarketDataStreams::enqueue_levels_only_refresh(
    const std::string &exchange_name, const std::string &market_type,
    const std::string &symbol, const Core::OrderBookLevels &levels) {
    MarketDataMsg msg;
    msg.exchange_name = exchange_name;
    msg.market_type = market_type;
    msg.symbol = symbol;
    msg.is_levels_only_refresh = true;
    msg.full_refresh_orderbook = levels;
    lob_queue.enqueue(msg);
}

void MarketData::MarketDataStreams::handle_book_ticker_message(
    const std::string &msg, const BbaSymbolSlots &slots) {
    const char *data = msg.data();
    size_t len = msg.size();

    if (len == 0 || len > BBA_PAYLOAD_SIZE) return;

    // Skip subscription confirmations
    if (memmem(data, len, "\"result\"", 8) != nullptr) return;

    // Extract symbol via raw byte scan. Binance/Aster carry "s":"SYMBOL";
    // Hyperliquid bbo carries "coin":"COIN" (route only actual bbo data
    // messages — subscriptionResponse acks also contain a coin field).
    const char *s_key =
        static_cast<const char *>(memmem(data, len, "\"s\":\"", 5));
    const char *sym_start = nullptr;
    if (s_key) {
        sym_start = s_key + 5;
    } else {
        static const char kBboChannel[] = "\"channel\":\"bbo\"";
        if (memmem(data, len, kBboChannel, sizeof(kBboChannel) - 1) == nullptr)
            return;
        const char *c_key =
            static_cast<const char *>(memmem(data, len, "\"coin\":\"", 8));
        if (!c_key) return;
        sym_start = c_key + 8;
    }
    const char *sym_end = static_cast<const char *>(
        memchr(sym_start, '"', data + len - sym_start));
    if (!sym_end) return;
    std::string_view symbol_sv(sym_start, sym_end - sym_start);

    // Route to slot. Binance/Aster carry the canonical symbol in "s"; HL bbo
    // carries the bare coin, while slots are registered under the canonical
    // (BTCUSDC) symbol — append the USDC quote for the lookup.
    auto it = s_key ? slots.find(symbol_sv)
                    : slots.find(CCXTUtils::hyperliquid_coin_to_symbol(
                          std::string(symbol_sv)));
    if (it == slots.end()) return;
    uint32_t slot_id = it->second;

    // SeqLock write
    auto &slot = book_states_[slot_id];
    uint64_t cur = slot.seq.load(std::memory_order_relaxed);
    slot.seq.store(cur + 1, std::memory_order_release);
    std::memcpy(slot.payload, data, len);
    slot.length = len;
    slot.seq.store(cur + 2, std::memory_order_release);

    // Notify consumer
    bba_notify_queue_.enqueue(slot_id);
}

void MarketData::MarketDataStreams::handle_polymarket_message(
    const std::string &msg) {
    MarketDataMsg pm_msg;
    pm_msg.msg = msg;
    polymarket_queue.enqueue(pm_msg);
}

void MarketData::MarketDataStreams::parse_binance_ohlcv_message(
    const MarketDataMsg &ohlcv_msg, simdjson::dom::parser &parser) {
    try {
        if (keep_forming_klines.load(std::memory_order_relaxed)) {
            FormingKlineStore::get_instance().put(
                ohlcv_msg.exchange_name, ohlcv_msg.market_type, ohlcv_msg.msg,
                DatetimeUtils::get_current_time_ms());
        }
        if (!BinaUtils::isKlineClosedFastSearch(ohlcv_msg.msg)) {
            return;
        }

        simdjson::padded_string padded(ohlcv_msg.msg);
        auto doc = parser.parse(padded);

        auto data_result = doc["data"];
        bool has_data_wrapper = (data_result.error() == simdjson::SUCCESS);

        std::string interval;
        std::string symbol;
        MarketData::OhlcvDatasetTick ohlcv_tick;

        if (has_data_wrapper) {
            ohlcv_tick = MarketData::OhlcvDatasetTick::from_simdjson(
                data_result.value());
            interval = std::string(doc["data"]["k"]["i"].get_string().value());
            symbol = std::string(doc["data"]["k"]["s"].get_string().value());
        } else {
            ohlcv_tick =
                MarketData::OhlcvDatasetTick::from_simdjson(doc.value());
            interval = std::string(doc["k"]["i"].get_string().value());
            symbol = std::string(doc["k"]["s"].get_string().value());
        }

        BarFlushInfo bar_flush_info;
        bar_flush_info.tick = ohlcv_tick;

        BarClosePolicy bar_close_policy;
        bar_close_policy.is_dollar_bar = false;
        bar_close_policy.is_tick_count_bar = false;
        bar_close_policy.is_time_bar = true;
        bar_close_policy.is_exchange_bar = true;
        bar_close_policy.time_ms =
            MarketData::convert_interval_str_to_ms(interval);

        bar_flush_info.bar_identifier = MarketData::get_derived_bars_base_dir(
            symbol, ohlcv_msg.exchange_name, ohlcv_msg.market_type,
            bar_close_policy, true);
        MarketData::DatasetsManager::get_instance().add_ohlcv_tick(
            bar_flush_info);
    } catch (const std::exception &e) {
        emit_parse_error(ohlcv_msg.exchange_name, ohlcv_msg.market_type,
                         "binance_ohlcv_parse", e.what());
    }
}

std::optional<Core::OrderBookUpdate>
MarketData::MarketDataStreams::parse_gateio_lob_message(
    const MarketDataMsg &lob_msg, simdjson::dom::parser &parser,
    std::unordered_map<std::string, Core::OrderBook> &orderbook_map) {
    try {
        simdjson::padded_string padded(lob_msg.msg);
        auto doc = parser.parse(padded);

        if (!doc["event"].is_string() ||
            std::string(doc["event"].get_string().value()) != "update") {
            return std::nullopt;
        }

        if (!doc["channel"].is_string() ||
            std::string(doc["channel"].get_string().value()) !=
                "spot.order_book_update") {
            return std::nullopt;
        }

        if (!doc["result"].is_object()) {
            return std::nullopt;
        }

        auto result = doc["result"];

        std::string symbol = CCXTUtils::convert_gateio_symbol(
            std::string(result["s"].get_string().value()));
        long long timestamp = result["t"].get_int64().value();
        long long sequence_start = result["U"].get_int64().value();
        long long sequence_end = result["u"].get_int64().value();

        Core::OrderBookUpdate update;
        update.symbol_full = Core::Utils::get_symbol_exchange_market_key(
            symbol, lob_msg.exchange_name, lob_msg.market_type);
        update.timestamp = timestamp;
        update.sequence_start = sequence_start;
        update.sequence_end = sequence_end;

        if (result["a"].is_array()) {
            for (auto ask : result["a"].get_array()) {
                auto ask_arr = ask.get_array();
                double price =
                    std::stod(std::string(ask_arr.at(0).get_string().value()));
                double quantity =
                    std::stod(std::string(ask_arr.at(1).get_string().value()));
                update.asks.emplace_back(price, quantity);
            }
        }

        if (result["b"].is_array()) {
            for (auto bid : result["b"].get_array()) {
                auto bid_arr = bid.get_array();
                double price =
                    std::stod(std::string(bid_arr.at(0).get_string().value()));
                double quantity =
                    std::stod(std::string(bid_arr.at(1).get_string().value()));
                update.bids.emplace_back(price, quantity);
            }
        }

        return update;
    } catch (const std::exception &e) {
        emit_parse_error("gateio", lob_msg.market_type, "gateio_lob_parse",
                         e.what());
        return std::nullopt;
    }
}

std::optional<Core::OrderBookUpdate>
MarketData::MarketDataStreams::parse_binance_lob_message(
    const MarketDataMsg &lob_msg, simdjson::dom::parser &parser) {
    try {
        simdjson::padded_string padded(lob_msg.msg);
        auto doc = parser.parse(padded);

        // Skip subscription confirmation responses
        if (doc.at_key("result").error() == simdjson::SUCCESS) {
            return std::nullopt;
        }

        // Handle potential {data: {...}} wrapper (combined stream format)
        auto data_result = doc["data"];
        bool has_data_wrapper = (data_result.error() == simdjson::SUCCESS);
        auto root = has_data_wrapper ? data_result.value() : doc.value();

        std::string_view event_type;
        if (root["e"].get(event_type) != simdjson::SUCCESS ||
            event_type != "depthUpdate") {
            return std::nullopt;
        }

        std::string symbol(root["s"].get_string().value());
        long long timestamp = root["E"].get_int64().value();
        long long sequence_start = root["U"].get_int64().value();
        long long sequence_end = root["u"].get_int64().value();

        Core::OrderBookUpdate update;
        update.symbol_full = Core::Utils::get_symbol_exchange_market_key(
            symbol, lob_msg.exchange_name, lob_msg.market_type);
        update.timestamp = timestamp;
        update.sequence_start = sequence_start;
        update.sequence_end = sequence_end;
        int64_t prev_end = 0;
        if (root["pu"].get_int64().get(prev_end) == simdjson::SUCCESS) update.prev_sequence_end = prev_end;

        if (root["a"].is_array()) {
            for (auto ask : root["a"].get_array()) {
                auto ask_arr = ask.get_array();
                double price =
                    std::stod(std::string(ask_arr.at(0).get_string().value()));
                double quantity =
                    std::stod(std::string(ask_arr.at(1).get_string().value()));
                update.asks.emplace_back(price, quantity);
            }
        }

        if (root["b"].is_array()) {
            for (auto bid : root["b"].get_array()) {
                auto bid_arr = bid.get_array();
                double price =
                    std::stod(std::string(bid_arr.at(0).get_string().value()));
                double quantity =
                    std::stod(std::string(bid_arr.at(1).get_string().value()));
                update.bids.emplace_back(price, quantity);
            }
        }

        return update;
    } catch (const std::exception &e) {
        emit_parse_error("binance", lob_msg.market_type, "binance_lob_parse",
                         e.what());
        return std::nullopt;
    }
}

void MarketData::MarketDataStreams::parse_gateio_ohlcv_message(
    const MarketDataMsg &ohlcv_msg, simdjson::dom::parser &parser) {
    try {
        simdjson::padded_string padded(ohlcv_msg.msg);
        auto doc = parser.parse(padded);

        if (!doc["event"].is_string() ||
            std::string(doc["event"].get_string().value()) != "update") {
            return;
        }

        if (!doc["channel"].is_string() ||
            std::string(doc["channel"].get_string().value()) !=
                "spot.candlesticks") {
            return;
        }

        if (!doc["result"].is_object()) {
            return;
        }

        auto result = doc["result"];

        bool is_closed = result["w"].get_bool().value();
        if (!is_closed) {
            return;
        }

        std::string name_field = std::string(result["n"].get_string().value());

        size_t first_underscore = name_field.find('_');
        if (first_underscore == std::string::npos) {
            return;
        }

        std::string interval = name_field.substr(0, first_underscore);
        std::string symbol = name_field.substr(first_underscore + 1);
        symbol.erase(std::remove(symbol.begin(), symbol.end(), '_'),
                     symbol.end());

        // Gate.io timestamps are in seconds
        long long kline_open_time_seconds =
            std::stoll(std::string(result["t"].get_string().value()));
        long long kline_open_time_ms = kline_open_time_seconds * 1000;

        long long interval_ms =
            MarketData::convert_interval_str_to_ms(interval);
        long long kline_close_time_ms = kline_open_time_ms + interval_ms;

        OhlcvDatasetTick ohlcv_tick;
        ohlcv_tick.kline_open_time = kline_open_time_ms;
        ohlcv_tick.kline_close_time = kline_close_time_ms;
        ohlcv_tick.open_price =
            std::stod(std::string(result["o"].get_string().value()));
        ohlcv_tick.high_price =
            std::stod(std::string(result["h"].get_string().value()));
        ohlcv_tick.low_price =
            std::stod(std::string(result["l"].get_string().value()));
        ohlcv_tick.close_price =
            std::stod(std::string(result["c"].get_string().value()));
        ohlcv_tick.volume =
            std::stod(std::string(result["v"].get_string().value()));
        ohlcv_tick.quote_asset_volume =
            std::stod(std::string(result["a"].get_string().value()));
        ohlcv_tick.is_closed = true;

        ohlcv_tick.number_of_trades = 0;
        ohlcv_tick.taker_buy_base_volume = 0;
        ohlcv_tick.taker_buy_quote_volume = 0;

        BarFlushInfo bar_flush_info;
        bar_flush_info.tick = ohlcv_tick;

        BarClosePolicy bar_close_policy;
        bar_close_policy.is_dollar_bar = false;
        bar_close_policy.is_tick_count_bar = false;
        bar_close_policy.is_time_bar = true;
        bar_close_policy.is_exchange_bar = true;
        bar_close_policy.time_ms = interval_ms;

        bar_flush_info.bar_identifier = MarketData::get_derived_bars_base_dir(
            symbol, ohlcv_msg.exchange_name, ohlcv_msg.market_type,
            bar_close_policy, true);

        MarketData::DatasetsManager::get_instance().add_ohlcv_tick(
            bar_flush_info);
    } catch (const std::exception &e) {
        emit_parse_error("gateio", ohlcv_msg.market_type,
                         "gateio_ohlcv_parse", e.what());
    }
}

void MarketData::MarketDataStreams::parse_bybit_ohlcv_message(
    const MarketDataMsg &ohlcv_msg, simdjson::dom::parser &parser) {
    try {
        simdjson::padded_string padded(ohlcv_msg.msg);
        auto doc = parser.parse(padded);

        // Check if this is a subscription confirmation message and discard it
        if (doc["op"].is_string() &&
            std::string(doc["op"].get_string().value()) == "subscribe") {
            return;
        }

        // Check if topic field exists
        if (!doc["topic"].is_string()) {
            return;
        }

        std::string topic = std::string(doc["topic"].get_string().value());

        size_t first_dot = topic.find('.');
        size_t second_dot = topic.find('.', first_dot + 1);

        if (first_dot == std::string::npos || second_dot == std::string::npos) {
            emit_parse_error("bybit", ohlcv_msg.market_type,
                             "bybit_topic_format_invalid", topic);
            return;
        }

        std::string interval_str =
            topic.substr(first_dot + 1, second_dot - first_dot - 1);
        std::string symbol = topic.substr(second_dot + 1);

        // Convert interval to standard format
        std::string interval;
        int interval_minutes = std::stoi(interval_str);
        if (interval_minutes < 60) {
            interval = interval_str + "m";
        } else if (interval_minutes == 60) {
            interval = "1h";
        } else if (interval_minutes == 120) {
            interval = "2h";
        } else if (interval_minutes == 240) {
            interval = "4h";
        } else if (interval_minutes == 360) {
            interval = "6h";
        } else if (interval_minutes == 720) {
            interval = "12h";
        } else if (interval_minutes == 1440) {
            interval = "1d";
        } else {
            emit_parse_error("bybit", ohlcv_msg.market_type,
                             "bybit_unsupported_interval",
                             interval_str + " minutes");
            return;
        }

        if (!doc["data"].is_array() || doc["data"].get_array().size() == 0) {
            return;
        }

        auto data_array = doc["data"].get_array();
        auto data_element = data_array.at(0);

        if (!data_element["confirm"].get_bool().value()) {
            return;
        }

        OhlcvDatasetTick ohlcv_tick;
        ohlcv_tick.kline_open_time = data_element["start"].get_uint64().value();
        ohlcv_tick.kline_close_time = data_element["end"].get_uint64().value();

        if (ohlcv_tick.kline_close_time % 2 != 0) {
            ohlcv_tick.kline_close_time += 1;
        }

        ohlcv_tick.open_price =
            std::stod(std::string(data_element["open"].get_string().value()));
        ohlcv_tick.close_price =
            std::stod(std::string(data_element["close"].get_string().value()));
        ohlcv_tick.high_price =
            std::stod(std::string(data_element["high"].get_string().value()));
        ohlcv_tick.low_price =
            std::stod(std::string(data_element["low"].get_string().value()));
        ohlcv_tick.volume =
            std::stod(std::string(data_element["volume"].get_string().value()));
        ohlcv_tick.quote_asset_volume = std::stod(
            std::string(data_element["turnover"].get_string().value()));
        ohlcv_tick.is_closed = true;

        ohlcv_tick.number_of_trades = 0;
        ohlcv_tick.taker_buy_base_volume = 0;
        ohlcv_tick.taker_buy_quote_volume = 0;

        BarFlushInfo bar_flush_info;
        bar_flush_info.tick = ohlcv_tick;

        BarClosePolicy bar_close_policy;
        bar_close_policy.is_dollar_bar = false;
        bar_close_policy.is_tick_count_bar = false;
        bar_close_policy.is_time_bar = true;
        bar_close_policy.is_exchange_bar = true;
        bar_close_policy.time_ms =
            MarketData::convert_interval_str_to_ms(interval);

        bar_flush_info.bar_identifier = MarketData::get_derived_bars_base_dir(
            symbol, ohlcv_msg.exchange_name, ohlcv_msg.market_type,
            bar_close_policy, true);

        MarketData::DatasetsManager::get_instance().add_ohlcv_tick(
            bar_flush_info);
    } catch (const std::exception &e) {
        emit_parse_error("bybit", ohlcv_msg.market_type,
                         "bybit_ohlcv_parse", e.what());
    }
}

void MarketData::MarketDataStreams::parse_bitget_ohlcv_message(
    const MarketDataMsg &ohlcv_msg, simdjson::dom::parser &parser) {
    try {
        simdjson::padded_string padded(ohlcv_msg.msg);
        auto doc = parser.parse(padded);

        if (doc["action"].is_string() &&
            std::string(doc["action"].get_string().value()) == "subscribe") {
            return;
        }

        if (!doc["action"].is_string() ||
            std::string(doc["action"].get_string().value()) != "snapshot") {
            return;
        }

        if (!doc["arg"].is_object() || !doc["arg"]["channel"].is_string()) {
            return;
        }

        std::string channel =
            std::string(doc["arg"]["channel"].get_string().value());
        std::string symbol =
            std::string(doc["arg"]["instId"].get_string().value());

        if (channel.substr(0, 6) != "candle") {
            return;
        }
        std::string interval = channel.substr(6);

        if (interval == "1m")
            interval = "1m";
        else if (interval == "5m")
            interval = "5m";
        else if (interval == "15m")
            interval = "15m";
        else if (interval == "30m")
            interval = "30m";
        else if (interval == "1H")
            interval = "1h";
        else if (interval == "4H")
            interval = "4h";
        else if (interval == "6H")
            interval = "6h";
        else if (interval == "12H")
            interval = "12h";
        else if (interval == "1D")
            interval = "1d";
        else if (interval == "3D")
            interval = "3d";
        else if (interval == "1W")
            interval = "1w";
        else {
            emit_parse_error("bitget", ohlcv_msg.market_type,
                             "bitget_unsupported_interval", interval);
            return;
        }

        if (!doc["data"].is_array() || doc["data"].get_array().size() == 0) {
            return;
        }

        auto data_array = doc["data"].get_array();

        for (auto data_element : data_array) {
            if (!data_element.is_array() ||
                data_element.get_array().size() < 8) {
                continue;
            }

            auto candle_data = data_element.get_array();

            OhlcvDatasetTick ohlcv_tick;
            ohlcv_tick.kline_open_time = std::stoull(
                std::string(candle_data.at(0).get_string().value()));
            ohlcv_tick.open_price =
                std::stod(std::string(candle_data.at(1).get_string().value()));
            ohlcv_tick.high_price =
                std::stod(std::string(candle_data.at(2).get_string().value()));
            ohlcv_tick.low_price =
                std::stod(std::string(candle_data.at(3).get_string().value()));
            ohlcv_tick.close_price =
                std::stod(std::string(candle_data.at(4).get_string().value()));
            ohlcv_tick.volume =
                std::stod(std::string(candle_data.at(5).get_string().value()));
            ohlcv_tick.quote_asset_volume =
                std::stod(std::string(candle_data.at(6).get_string().value()));

            long long interval_ms =
                MarketData::convert_interval_str_to_ms(interval);
            ohlcv_tick.kline_close_time =
                ohlcv_tick.kline_open_time + interval_ms;

            ohlcv_tick.is_closed = true;

            ohlcv_tick.number_of_trades = 0;
            ohlcv_tick.taker_buy_base_volume = 0;
            ohlcv_tick.taker_buy_quote_volume = 0;

            BarFlushInfo bar_flush_info;
            bar_flush_info.tick = ohlcv_tick;

            BarClosePolicy bar_close_policy;
            bar_close_policy.is_dollar_bar = false;
            bar_close_policy.is_tick_count_bar = false;
            bar_close_policy.is_time_bar = true;
            bar_close_policy.is_exchange_bar = true;
            bar_close_policy.time_ms = interval_ms;

            bar_flush_info.bar_identifier =
                MarketData::get_derived_bars_base_dir(
                    symbol, ohlcv_msg.exchange_name, ohlcv_msg.market_type,
                    bar_close_policy, true);

            MarketData::DatasetsManager::get_instance().add_ohlcv_tick(
                bar_flush_info);
        }
    } catch (const std::exception &e) {
        emit_parse_error("bitget", ohlcv_msg.market_type,
                         "bitget_ohlcv_parse", e.what());
    }
}

void MarketData::MarketDataStreams::parse_polygon_ohlcv_message(
    const MarketDataMsg &ohlcv_msg, simdjson::dom::parser &parser) {
    try {
        simdjson::padded_string padded(ohlcv_msg.msg);
        auto doc = parser.parse(padded);

        if (!doc.is_array()) {
            return;
        }

        auto bars_array = doc.get_array();
        std::vector<std::pair<std::string, SecondBar>> second_bars;

        for (auto bar : bars_array) {
            if (!bar["ev"].is_string()) {
                continue;
            }

            std::string event_type =
                std::string(bar["ev"].get_string().value());
            if (event_type == "A") {
                SecondBar second;
                second.open_time_ms = bar["s"].get_int64().value();
                second.open = bar["o"].get_double().value();
                second.high = bar["h"].get_double().value();
                second.low = bar["l"].get_double().value();
                second.close = bar["c"].get_double().value();
                second.volume =
                    static_cast<double>(bar["v"].get_int64().value());
                second.vwap = bar["vw"].get_double().value();
                const long long avg_trade_size = bar["z"].get_int64().value();
                second.trades =
                    avg_trade_size > 0
                        ? static_cast<std::int64_t>(second.volume /
                                                    avg_trade_size)
                        : 0;
                second_bars.emplace_back(
                    std::string(bar["sym"].get_string().value()), second);
                continue;
            }
            if (event_type == "status") {
                LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_INIT_PHASE,
                         "polygon status",
                         (nlohmann::json{
                             {"message", std::string(simdjson::minify(bar))}}));
                continue;
            }
            if (event_type != "AM") {
                continue;
            }

            std::string symbol = std::string(bar["sym"].get_string().value());

            OhlcvDatasetTick ohlcv_tick;
            ohlcv_tick.kline_open_time = bar["s"].get_int64().value();
            ohlcv_tick.kline_close_time = bar["e"].get_int64().value();
            ohlcv_tick.open_price = bar["o"].get_double().value();
            ohlcv_tick.high_price = bar["h"].get_double().value();
            ohlcv_tick.low_price = bar["l"].get_double().value();
            ohlcv_tick.close_price = bar["c"].get_double().value();
            ohlcv_tick.volume =
                static_cast<double>(bar["v"].get_int64().value());

            double vwap = bar["vw"].get_double().value();
            ohlcv_tick.quote_asset_volume = ohlcv_tick.volume * vwap;

            long long avg_trade_size = bar["z"].get_int64().value();
            ohlcv_tick.number_of_trades =
                avg_trade_size > 0
                    ? static_cast<long>(ohlcv_tick.volume / avg_trade_size)
                    : 0;
            ohlcv_tick.taker_buy_base_volume = 0;
            ohlcv_tick.taker_buy_quote_volume = 0;
            ohlcv_tick.is_closed = true;

            long long interval_ms =
                ohlcv_tick.kline_close_time - ohlcv_tick.kline_open_time;

            BarFlushInfo bar_flush_info;
            bar_flush_info.tick = ohlcv_tick;

            BarClosePolicy bar_close_policy;
            bar_close_policy.is_dollar_bar = false;
            bar_close_policy.is_tick_count_bar = false;
            bar_close_policy.is_time_bar = true;
            bar_close_policy.is_exchange_bar = true;
            bar_close_policy.time_ms = interval_ms;

            bar_flush_info.bar_identifier =
                MarketData::get_derived_bars_base_dir(
                    symbol, ohlcv_msg.exchange_name, ohlcv_msg.market_type,
                    bar_close_policy, true);

            MarketData::DatasetsManager::get_instance().add_ohlcv_tick(
                bar_flush_info);
        }
        if (!second_bars.empty())
            PolygonSecondBarStore::get_instance().add(std::move(second_bars));
    } catch (const std::exception &e) {
        emit_parse_error("polygon", ohlcv_msg.market_type,
                         "polygon_ohlcv_parse", e.what());
    }
}

void MarketData::MarketDataStreams::lob_msg_consumer_thread() {
    thread_local simdjson::dom::parser parser;
    std::unordered_map<std::string, Core::OrderBook> gateio_lob_map;
    std::unordered_map<long long, LighterBookStream> lighter_books;
    while (true) {
        MarketDataMsg msg;
        if (lob_queue.wait_dequeue_timed(msg,
                                         std::chrono::milliseconds(100))) {
            MarketEvent event;
            event.type = MarketEvent::L2_UPDATE;
            bool has_event = false;

            if (msg.is_full_refresh) {
                L2UpdateEvent l2;
                l2.sub_type = L2UpdateEvent::FULL_REFRESH;
                l2.exchange_name = msg.exchange_name;
                l2.market_type = msg.market_type;
                l2.symbol = msg.symbol;
                l2.full_refresh_orderbook = msg.full_refresh_orderbook;
                event.l2_update = std::move(l2);
                has_event = true;
            } else if (msg.is_levels_only_refresh) {
                L2UpdateEvent l2;
                l2.sub_type = L2UpdateEvent::LEVELS_ONLY;
                l2.exchange_name = msg.exchange_name;
                l2.market_type = msg.market_type;
                l2.symbol = msg.symbol;
                l2.full_refresh_orderbook = msg.full_refresh_orderbook;
                event.l2_update = std::move(l2);
                has_event = true;
            } else if (msg.exchange_name == "binance") {
                auto update = parse_binance_lob_message(msg, parser);
                if (update) {
                    L2UpdateEvent l2;
                    l2.sub_type = L2UpdateEvent::INCREMENTAL;
                    l2.update = std::move(*update);
                    event.l2_update = std::move(l2);
                    has_event = true;
                }
            } else if (msg.exchange_name == "gateio") {
                auto update =
                    parse_gateio_lob_message(msg, parser, gateio_lob_map);
                if (update) {
                    L2UpdateEvent l2;
                    l2.sub_type = L2UpdateEvent::INCREMENTAL;
                    l2.update = std::move(*update);
                    event.l2_update = std::move(l2);
                    has_event = true;
                }
            } else if (msg.exchange_name == "lighter") {
                // Lighter depth feeds paper fills only (DepthHistory); the
                // engine order book and state machines never see it.
                consume_lighter_lob_message(msg, parser, lighter_books);
            } else if (msg.exchange_name == "bybit") {
            } else if (msg.exchange_name == "bitget") {
            } else if (msg.exchange_name == "polygon") {
            }

            if (has_event) {
                // Record on consumer thread (symbol already known)
                if (!msg.msg.empty()) {
                    std::string symbol =
                        (event.l2_update->sub_type ==
                         L2UpdateEvent::INCREMENTAL)
                            ? event.l2_update->update.symbol_full
                            : event.l2_update->symbol;
                    DataRecorder::getInstance().record_with_symbol(
                        "lob_l2_update", msg.exchange_name, msg.market_type,
                        symbol, msg.msg);
                }

                StateMachineDispatcher::get_instance().enqueue(
                    MarketEvent(event));

                if (is_ran_from_live_infra) {
                    live_event_callback_(std::move(event));
                }
            }
        }
    }
}

void MarketData::MarketDataStreams::ohlcv_msg_consumer_thread() {
    thread_local simdjson::dom::parser parser;
    while (true) {
        MarketDataMsg msg;
        if (ohlcv_queue.wait_dequeue_timed(msg,
                                           std::chrono::milliseconds(100))) {
            if (msg.exchange_name == "binance") {
                parse_binance_ohlcv_message(msg, parser);
            } else if (msg.exchange_name == "gateio") {
                parse_gateio_ohlcv_message(msg, parser);
            } else if (msg.exchange_name == "bybit") {
                parse_bybit_ohlcv_message(msg, parser);
            } else if (msg.exchange_name == "bitget") {
                parse_bitget_ohlcv_message(msg, parser);
            } else if (msg.exchange_name == "polygon") {
                parse_polygon_ohlcv_message(msg, parser);
            }
        }
    }
}

void MarketData::MarketDataStreams::trade_msg_consumer_thread() {
    thread_local simdjson::dom::parser parser;
    std::unordered_map<long long, std::string> lighter_symbols;
    std::unordered_map<long long, long long> lighter_price_emit_ms;

    while (true) {
        MarketDataMsg trade_msg;
        if (trade_queue.wait_dequeue_timed(
                trade_msg, std::chrono::milliseconds(100))) {
            auto start_time = std::chrono::steady_clock::now();
            try {
                simdjson::padded_string padded(trade_msg.msg);
                auto doc = parser.parse(padded);

                simdjson::dom::element trade_data;
                if (trade_msg.exchange_name == "gateio") {
                    if (!doc["event"].is_string() ||
                        std::string(doc["event"].get_string().value()) !=
                            "update") {
                        continue;
                    }
                    trade_data = doc["result"];
                } else if (trade_msg.exchange_name == "binance") {
                    auto result_field = doc["result"];
                    if (result_field.error() == simdjson::error_code::SUCCESS) {
                        continue;
                    }
                    trade_data = doc.value();
                } else if (trade_msg.exchange_name == "lighter") {
                    consume_lighter_trade_message(trade_msg, doc.value(),
                                                  lighter_symbols,
                                                  lighter_price_emit_ms);
                    continue;
                } else {
                    trade_data = doc["data"];
                }

                const auto &trade =
                    MarketData::NormalizedParsedTrade::from_simdjson(
                        trade_data, trade_msg.exchange_name,
                        trade_msg.market_type);

                // Binance USDS-M Futures @trade emits sentinel events with
                // {"X":"NA","p":"0","q":"0"} that look structurally like
                // real trades. Drop them at the boundary so OHLC math and
                // downstream consumers never see them.
                if (trade.price <= 0.0 || trade.quantity <= 0.0) {
                    continue;
                }

                DataRecorder::getInstance().record_with_symbol(
                    WsStreamConstants::TRADE_STREAM_TYPE,
                    trade_msg.exchange_name, trade_msg.market_type,
                    trade.symbol, trade_msg.msg);

                BarAggregator::getInstance().add_trade(trade, true);

                MarketEvent event;
                event.type = MarketEvent::TRADE;
                event.trade = {trade.symbol, trade_msg.exchange_name,
                               trade_msg.market_type, trade.price,
                               trade.quantity, trade.is_buyer_maker,
                               trade.timestamp,
                               std::strtoll(trade.trade_id.c_str(), nullptr, 10),
                               trade.trade_time_ms};

                StateMachineDispatcher::get_instance().enqueue(
                    MarketEvent(event));

                if (is_ran_from_live_infra) {
                    live_event_callback_(std::move(event));
                }

                ++trade_msgs_count;
                trade_total_latency_to_parse_ms += trade.latency_ms;
                auto end_time = std::chrono::steady_clock::now();
                trade_total_latency_to_process_micros +=
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        end_time - start_time)
                        .count();

            } catch (const std::exception &e) {
                emit_parse_error(trade_msg.exchange_name,
                                 trade_msg.market_type, "trade_parse",
                                 e.what());
            }
        }
    }
}

void MarketData::MarketDataStreams::book_ticker_msg_consumer_thread() {
    thread_local simdjson::dom::parser parser;
    std::vector<uint64_t> last_processed_seq(MAX_BBA_SLOTS, 0);
    uint32_t slot_id;

    long long processed = 0;
    long long conflated = 0;
    long long torn = 0;
    auto last_log = std::chrono::steady_clock::now();

    while (!should_stop_reconnecting.load()) {
        if (!bba_notify_queue_.wait_dequeue_timed(
                slot_id, std::chrono::milliseconds(100))) {
            continue;
        }

        // Bounded SeqLock read
        auto &slot = book_states_[slot_id];
        char local_buf[BBA_PAYLOAD_SIZE];
        size_t local_len = 0;
        uint64_t seq1, seq2;
        bool success = false;

        for (int retry = 0; retry < 10; ++retry) {
            seq1 = slot.seq.load(std::memory_order_acquire);
            if (seq1 & 1) {
                std::this_thread::yield();
                continue;
            }
            local_len = slot.length;
            std::memcpy(local_buf, slot.payload, local_len);
            seq2 = slot.seq.load(std::memory_order_acquire);
            if (seq1 == seq2) {
                success = true;
                break;
            }
            std::this_thread::yield();
        }
        if (!success) { ++torn; continue; }

        // Conflation: skip if already processed this exact update
        if (seq1 == last_processed_seq[slot_id]) { ++conflated; continue; }
        last_processed_seq[slot_id] = seq1;

        // Parse & dispatch
        const auto &meta = bba_slot_metadata_[slot_id];
        try {
            simdjson::padded_string padded(local_buf, local_len);
            simdjson::dom::element doc = parser.parse(padded);

            double bid_price = 0, bid_qty = 0, ask_price = 0, ask_qty = 0;
            int64_t event_ts_ms = 0;
            std::string normalized_record;

            if (meta.exchange_name == "lighter") {
                // {"channel":"ticker:1","nonce":..,"ticker":{"s":"BTC",
                //   "a":{"price","size"},"b":{...}},"timestamp":ms}
                simdjson::dom::element ticker;
                std::string_view bid_sv, bid_qty_sv, ask_sv, ask_qty_sv;
                if (doc["ticker"].get(ticker) != simdjson::SUCCESS ||
                    ticker["b"]["price"].get(bid_sv) != simdjson::SUCCESS ||
                    ticker["b"]["size"].get(bid_qty_sv) != simdjson::SUCCESS ||
                    ticker["a"]["price"].get(ask_sv) != simdjson::SUCCESS ||
                    ticker["a"]["size"].get(ask_qty_sv) != simdjson::SUCCESS)
                    continue;
                bid_price = sv_to_double(bid_sv);
                bid_qty = sv_to_double(bid_qty_sv);
                ask_price = sv_to_double(ask_sv);
                ask_qty = sv_to_double(ask_qty_sv);
                if (doc["timestamp"].get(event_ts_ms) != simdjson::SUCCESS)
                    event_ts_ms = now_ms();
                if (DataRecorder::getInstance().is_enabled()) {
                    int64_t nonce = 0;
                    if (doc["nonce"].get(nonce) != simdjson::SUCCESS) nonce = 0;
                    const auto ts = static_cast<long long>(event_ts_ms);
                    auto &line = normalized_record;
                    line.reserve(192);
                    line += "{\"u\":";
                    append_number(line, static_cast<long long>(nonce));
                    line += ",\"s\":\"" + meta.symbol + "\",\"b\":\"";
                    line.append(bid_sv);
                    line += "\",\"B\":\"";
                    line.append(bid_qty_sv);
                    line += "\",\"a\":\"";
                    line.append(ask_sv);
                    line += "\",\"A\":\"";
                    line.append(ask_qty_sv);
                    line += "\",\"T\":";
                    append_number(line, ts);
                    line += ",\"E\":";
                    append_number(line, ts);
                    line += '}';
                }
            } else if (meta.exchange_name == "hyperliquid") {
                // HL bbo: {"channel":"bbo","data":{"coin":..,"time":..,
                //   "bbo":[{px,sz,n}(bid),{px,sz,n}(ask)]}} — levels may be null.
                simdjson::dom::element data;
                simdjson::dom::array bbo_arr;
                if (doc["data"].get(data) != simdjson::SUCCESS ||
                    data["bbo"].get(bbo_arr) != simdjson::SUCCESS)
                    continue;
                auto level = [](const simdjson::dom::array &arr, size_t idx,
                                double &px, double &sz) {
                    simdjson::dom::element lvl;
                    if (arr.at(idx).get(lvl) != simdjson::SUCCESS ||
                        lvl.is_null())
                        return;
                    std::string_view p, s;
                    if (lvl["px"].get(p) == simdjson::SUCCESS)
                        px = sv_to_double(p);
                    if (lvl["sz"].get(s) == simdjson::SUCCESS)
                        sz = sv_to_double(s);
                };
                level(bbo_arr, 0, bid_price, bid_qty);
                level(bbo_arr, 1, ask_price, ask_qty);
                if (data["time"].get(event_ts_ms) != simdjson::SUCCESS) {
                    event_ts_ms =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
                }
            } else {
                std::string_view bid_sv, bid_qty_sv, ask_sv, ask_qty_sv;
                if (doc["b"].get(bid_sv) != simdjson::SUCCESS ||
                    doc["B"].get(bid_qty_sv) != simdjson::SUCCESS ||
                    doc["a"].get(ask_sv) != simdjson::SUCCESS ||
                    doc["A"].get(ask_qty_sv) != simdjson::SUCCESS)
                    continue;

                bid_price = sv_to_double(bid_sv);
                bid_qty = sv_to_double(bid_qty_sv);
                ask_price = sv_to_double(ask_sv);
                ask_qty = sv_to_double(ask_qty_sv);

                if (doc["E"].get(event_ts_ms) != simdjson::SUCCESS) {
                    event_ts_ms =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
                }
            }

            DataRecorder::getInstance().record_with_symbol(
                WsStreamConstants::BOOK_TICKER_STREAM_TYPE,
                meta.exchange_name, meta.market_type, meta.symbol,
                normalized_record.empty() ? std::string(local_buf, local_len)
                                          : normalized_record);

            MarketEvent event;
            event.type = MarketEvent::BBA;
            event.bba = {meta.symbol, meta.exchange_name, meta.market_type,
                         bid_price, bid_qty, ask_price, ask_qty, event_ts_ms};

            StateMachineDispatcher::get_instance().enqueue(
                MarketEvent(event));

            if (live_event_callback_) {
                live_event_callback_(std::move(event));
            }
            ++processed;
        } catch (const std::exception &e) {
            if (error_log_callback) {
                error_log_callback("bba consumer error: " +
                                   std::string(e.what()));
            }
        }

        // Periodic diagnostics every 10 seconds
        auto now = std::chrono::steady_clock::now();
        if (now - last_log > std::chrono::seconds(10)) {
            auto elapsed_s =
                std::chrono::duration_cast<std::chrono::seconds>(now - last_log)
                    .count();
            double rate =
                elapsed_s > 0 ? static_cast<double>(processed) / elapsed_s
                              : 0;
            std::cout << "[BBA] processed=" << processed
                      << " conflated=" << conflated << " torn=" << torn
                      << " rate=" << rate << "/s" << std::endl;
            processed = 0;
            conflated = 0;
            torn = 0;
            last_log = now;
        }
    }
}

void MarketData::MarketDataStreams::ticker_msg_consumer_thread() {
    while (!should_stop_reconnecting.load()) {
        MarketDataMsg ticker_msg;
        if (!ticker_queue.wait_dequeue_timed(
                ticker_msg, std::chrono::milliseconds(100))) {
            continue;
        }
        if (!is_ran_from_live_infra) continue;
        try {
            const auto &json_data = nlohmann::json::parse(ticker_msg.msg);
            if (!json_data.is_array()) continue;
            for (const auto &ticker : json_data) {
                if (!ticker.contains("s") || !ticker.contains("c") ||
                    !ticker.contains("E")) {
                    continue;
                }
                Core::MarketEvent market_event;
                market_event.type = Core::MarketEventType::PRICE_UPDATE;
                market_event.symbol = ticker["s"].get<std::string>();
                market_event.price =
                    std::stod(ticker["c"].get<std::string>());
                market_event.event_time = ticker["E"].get<long long>();
                market_event.exchange_name = ticker_msg.exchange_name;
                market_event.market_type = ticker_msg.market_type;
                Core::Engine::getInstance().process_market_event(market_event);
            }
        } catch (const std::exception &e) {
            emit_parse_error(ticker_msg.exchange_name, ticker_msg.market_type,
                             "ticker_parse", e.what());
        }
    }
}

void MarketData::MarketDataStreams::polymarket_msg_consumer_thread() {
    while (!should_stop_reconnecting.load()) {
        MarketDataMsg pm_msg;
        if (polymarket_queue.wait_dequeue_timed(
                pm_msg, std::chrono::milliseconds(100))) {
            DataRecorder::getInstance().record(
                WsStreamConstants::POLYMARKET_STREAM_TYPE, "polymarket",
                "market", pm_msg.msg);
            try {
                auto json_data = nlohmann::json::parse(pm_msg.msg);
                if (!json_data.contains("event_type")) continue;

                const auto &event_type =
                    json_data["event_type"].get<std::string>();

                if (!live_event_callback_) continue;

                if (event_type == "tick_size_change") {
                    const auto &condition_id =
                        json_data["market"].get<std::string>();
                    double new_tick_size = std::stod(
                        json_data["new_tick_size"].get<std::string>());

                    MarketEvent event;
                    event.type = MarketEvent::POLYMARKET_TICK_SIZE_CHANGE;
                    event.polymarket_tick_size_change =
                        PolymarketTickSizeChangeEvent{condition_id,
                                                      new_tick_size};
                    live_event_callback_(std::move(event));
                } else if (event_type == "best_bid_ask") {
                    const auto &asset_id =
                        json_data["asset_id"].get<std::string>();
                    double best_bid = std::stod(
                        json_data["best_bid"].get<std::string>());
                    double best_ask = std::stod(
                        json_data["best_ask"].get<std::string>());
                    long long ts = std::stoll(
                        json_data["timestamp"].get<std::string>());

                    MarketEvent live_event;
                    live_event.type = MarketEvent::POLYMARKET_BBA;
                    live_event.polymarket_bba =
                        PolymarketBBAEvent{asset_id, best_bid, best_ask, ts};
                    live_event_callback_(std::move(live_event));
                } else if (event_type == "book") {
                    const auto &asset_id =
                        json_data["asset_id"].get<std::string>();
                    long long ts = std::stoll(
                        json_data["timestamp"].get<std::string>());

                    Core::OrderBookLevels levels;
                    levels.timestamp_ms = ts;
                    for (const auto &entry : json_data["bids"]) {
                        double price =
                            std::stod(entry["price"].get<std::string>());
                        double size =
                            std::stod(entry["size"].get<std::string>());
                        if (size > 0)
                            levels.bids.emplace_back(price, size);
                    }
                    for (const auto &entry : json_data["asks"]) {
                        double price =
                            std::stod(entry["price"].get<std::string>());
                        double size =
                            std::stod(entry["size"].get<std::string>());
                        if (size > 0)
                            levels.asks.emplace_back(price, size);
                    }

                    MarketEvent live_event;
                    live_event.type = MarketEvent::POLYMARKET_L2_SNAPSHOT;
                    live_event.polymarket_l2_snapshot =
                        PolymarketL2SnapshotEvent{asset_id,
                                                   std::move(levels), ts};
                    live_event_callback_(std::move(live_event));
                } else if (event_type == "price_change") {
                    long long ts = std::stoll(
                        json_data["timestamp"].get<std::string>());

                    for (const auto &change : json_data["price_changes"]) {
                        const auto &asset_id =
                            change["asset_id"].get<std::string>();
                        double price =
                            std::stod(change["price"].get<std::string>());
                        double size =
                            std::stod(change["size"].get<std::string>());
                        const auto &side =
                            change["side"].get<std::string>();
                        bool is_bid = (side == "BUY");
                        double best_bid =
                            std::stod(change["best_bid"].get<std::string>());
                        double best_ask =
                            std::stod(change["best_ask"].get<std::string>());

                        MarketEvent live_event;
                        live_event.type = MarketEvent::POLYMARKET_L2_DELTA;
                        live_event.polymarket_l2_delta =
                            PolymarketL2DeltaEvent{asset_id, price, size,
                                                    is_bid, best_bid,
                                                    best_ask, ts};
                        live_event_callback_(std::move(live_event));
                    }
                } else if (event_type == "last_trade_price") {
                    const auto &condition_id =
                        json_data["market"].get<std::string>();
                    const auto &asset_id =
                        json_data["asset_id"].get<std::string>();
                    double price = std::stod(
                        json_data["price"].get<std::string>());
                    double size = std::stod(
                        json_data["size"].get<std::string>());
                    const auto &side =
                        json_data["side"].get<std::string>();
                    bool is_buy = (side == "BUY");
                    long long ts = std::stoll(
                        json_data["timestamp"].get<std::string>());

                    MarketEvent live_event;
                    live_event.type = MarketEvent::POLYMARKET_LAST_TRADE;
                    live_event.polymarket_last_trade =
                        PolymarketLastTradeEvent{condition_id, asset_id,
                                                  price, size, is_buy, ts};

                    StateMachineDispatcher::get_instance().enqueue(
                        MarketEvent(live_event));

                    live_event_callback_(std::move(live_event));
                } else if (event_type == "market_resolved") {
                    const auto &condition_id =
                        json_data["market"].get<std::string>();
                    long long ts = std::stoll(
                        json_data["timestamp"].get<std::string>());

                    MarketEvent live_event;
                    live_event.type =
                        MarketEvent::POLYMARKET_MARKET_RESOLVED;
                    live_event.polymarket_market_resolved =
                        PolymarketMarketResolvedEvent{condition_id, ts};

                    StateMachineDispatcher::get_instance().enqueue(
                        std::move(live_event));
                }
            } catch (const std::exception &e) {
                if (error_log_callback) {
                    error_log_callback("polymarket consumer error: " +
                                       std::string(e.what()));
                }
            }
        }
    }
}

void MarketData::MarketDataStreams::check_completed_bars() {
    size_t completed_bars_count = 0;
    auto last_completed_bars_log_time = std::chrono::steady_clock::now();
    constexpr auto completed_bars_log_interval = std::chrono::minutes(1);

    while (true) {
        const auto &completed_bars =
            BarAggregator::getInstance().emit_closed_bars_live();

        completed_bars_count += completed_bars.size();

        auto now = std::chrono::steady_clock::now();
        if (now - last_completed_bars_log_time >= completed_bars_log_interval) {
            LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_STREAM_THROUGHPUT,
                     "Completed bars in last 1 min: " +
                         std::to_string(completed_bars_count),
                     nlohmann::json{});

            completed_bars_count = 0;
            last_completed_bars_log_time = now;
        }
        auto sorted_bars = completed_bars;
        std::sort(sorted_bars.begin(), sorted_bars.end(),
                  [](const BarFlushInfo &a, const BarFlushInfo &b) {
                      return a.tick.kline_close_time < b.tick.kline_close_time;
                  });
        for (const auto &bar : sorted_bars) {
            long long emit_latency_ms = DatetimeUtils::get_current_time_ms() -
                                        bar.tick.kline_close_time;
            bar_emit_min_latency_ms =
                std::min(bar_emit_min_latency_ms, emit_latency_ms);
            bar_emit_max_latency_ms =
                std::max(bar_emit_max_latency_ms, emit_latency_ms);
            MarketData::DatasetsManager::get_instance().add_ohlcv_tick(bar);
        }

        if (this->last_spawned_live_sync_thread_time +
                std::chrono::seconds(10) <
            std::chrono::steady_clock::now()) {
            this->last_spawned_live_sync_thread_time =
                std::chrono::steady_clock::now();
            std::thread([this]() {
                this->bar_aggregator.sync_live_streamed_bars();
            }).detach();
        }

        if (this->last_trade_throughput_log_time + std::chrono::seconds(120) <
            std::chrono::steady_clock::now()) {
            auto now = std::chrono::steady_clock::now();
            auto duration_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - this->last_trade_throughput_log_time)
                    .count();
            double messages_per_second =
                duration_ms > 0
                    ? (static_cast<double>(trade_msgs_count) * 1000.0) /
                          duration_ms
                    : 0.0;
            double avg_latency_ms =
                trade_msgs_count > 0
                    ? (static_cast<double>(trade_total_latency_to_parse_ms) /
                       trade_msgs_count)
                    : 0.0;
            double avg_latency_to_process_micros =
                trade_msgs_count > 0
                    ? (static_cast<double>(
                           trade_total_latency_to_process_micros) /
                       trade_msgs_count)
                    : 0.0;

            this->last_trade_throughput_log_time = now;
            LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_STREAM_THROUGHPUT,
                     "--------------------------------\nTrade Throughput: " +
                         std::to_string(static_cast<int>(messages_per_second)) +
                         " msgs/sec | " + "Avg latency from event to parsed: " +
                         std::to_string(avg_latency_ms) + " ms | " +
                         "Avg latency to process trade msg: " +
                         std::to_string(avg_latency_to_process_micros) +
                         " μs\n--------------------------------",
                     nlohmann::json{});

            if (bar_emit_max_latency_ms > 0) {
                LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_STREAM_THROUGHPUT,
                         "Bar emit latency | min=" +
                             std::to_string(bar_emit_min_latency_ms) +
                             "ms max=" +
                             std::to_string(bar_emit_max_latency_ms) + "ms",
                         nlohmann::json{});
            }

            trade_msgs_count = 0;
            trade_total_latency_to_parse_ms = 0;
            trade_total_latency_to_process_micros = 0;
            bar_emit_min_latency_ms = std::numeric_limits<long long>::max();
            bar_emit_max_latency_ms = 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void MarketData::MarketDataStreams::open_stream_locked(
    const std::string &stream_type, const StreamsConfig &config) {
    if (config.streams.empty()) return;
    const std::string exchange_name = config.streams[0].exchange_name;
    const std::string market_type = config.streams[0].market_type;
    auto retired = std::make_shared<std::atomic<bool>>(false);

    std::string stream_url;
    std::string ip;
    std::string connected_label = "stream";
    std::function<void(const std::string &)> handler;
    if (stream_type == "ticker") {
        // Binance !miniTicker@arr is path-based, not subscription-message
        // based; the "ticker" tag routes it to /market on binance futures.
        const std::string base_url = MarketData::get_stream_url(
            exchange_name, market_type, "ticker", std::nullopt,
            config.streams);
        if (base_url.empty()) return;
        stream_url = base_url + "/!miniTicker@arr";
        connected_label = "ticker stream";
        handler = [this, exchange_name, market_type,
                   retired](const std::string &msg) {
            if (retired->load(std::memory_order_relaxed)) return;
            handle_ticker_message(msg, exchange_name, market_type);
        };
    } else {
        stream_url = MarketData::get_stream_url(exchange_name, market_type,
                                                stream_type,
                                                config.streams[0].interval,
                                                config.streams);
        ip = IPRotator::getInstance().getNextIP();
        if (stream_type == WsStreamConstants::BOOK_TICKER_STREAM_TYPE) {
            connected_label = "BBA stream";
            handler = [this, slots = assign_bba_slots(config.streams),
                       retired](const std::string &msg) {
                if (retired->load(std::memory_order_relaxed)) return;
                handle_book_ticker_message(msg, *slots);
            };
        } else if (stream_type == WsStreamConstants::OHLCV_STREAM_TYPE) {
            handler = [this, exchange_name, market_type,
                       retired](const std::string &msg) {
                if (retired->load(std::memory_order_relaxed)) return;
                handle_ohlcv_message(msg, exchange_name, market_type);
            };
        } else if (stream_type == WsStreamConstants::LOB_STREAM_TYPE) {
            handler = [this, exchange_name, market_type,
                       retired](const std::string &msg) {
                if (retired->load(std::memory_order_relaxed)) return;
                handle_lob_message(msg, exchange_name, market_type);
            };
        } else if (stream_type == WsStreamConstants::TRADE_STREAM_TYPE) {
            handler = [this, exchange_name, market_type,
                       retired](const std::string &msg) {
                if (retired->load(std::memory_order_relaxed)) return;
                handle_trade_message(msg, exchange_name, market_type);
            };
        } else {
            throw std::invalid_argument("unknown stream type " + stream_type);
        }
    }

    auto stream_ptr = std::make_unique<WsStream>(
        stream_url, handler, ip, info_log_callback, error_log_callback);
    stream_ptr->set_stream_infos(exchange_name, market_type, stream_type,
                                 config.streams);
    stream_ptr->connect();
    streams.push_back(StreamMeta{config.streams, std::move(stream_ptr),
                                 stream_type, retired});
    LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_STREAM_CONNECTED,
             "Connected to " + connected_label + ": " + stream_url,
             nlohmann::json{});
}

void MarketData::MarketDataStreams::open_streams(
    const std::string &stream_type, const std::vector<StreamsConfig> &configs) {
    for (const auto &config : configs) {
        {
            std::lock_guard<std::mutex> lock(streams_mutex_);
            open_stream_locked(stream_type, config);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

static bool same_stream_infos(const std::vector<MarketData::StreamInfo> &a,
                              const std::vector<MarketData::StreamInfo> &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].exchange_name != b[i].exchange_name ||
            a[i].market_type != b[i].market_type ||
            a[i].symbol != b[i].symbol || a[i].interval != b[i].interval)
            return false;
    }
    return true;
}

void MarketData::MarketDataStreams::retire_streams(
    const std::string &stream_type, const std::vector<StreamsConfig> &configs) {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    for (const auto &config : configs) {
        auto it = std::find_if(
            streams.begin(), streams.end(), [&](const StreamMeta &meta) {
                return meta.stream_type == stream_type &&
                       same_stream_infos(meta.stream_infos, config.streams);
            });
        if (it == streams.end()) {
            LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_STREAM_STOPPING,
                      "retire_streams: no open " + stream_type +
                          " connection matches",
                      nlohmann::json{});
            continue;
        }
        it->retired->store(true);
        it->stream->disconnect();
        LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_STREAM_STOPPING,
                 "Retired " + stream_type + " stream: " + it->stream->wsurl_,
                 nlohmann::json{});
        retired_streams.push_back(std::move(*it));
        streams.erase(it);
    }
}

void MarketData::MarketDataStreams::start_streams() {
    open_streams(WsStreamConstants::OHLCV_STREAM_TYPE, ohlcv_configs);
    open_streams(WsStreamConstants::LOB_STREAM_TYPE, lob_configs);
    LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_INIT_PHASE,
             "start_streams: starting trade stream group with " +
                 std::to_string(trade_configs.size()) + " configs",
             nlohmann::json{});
    open_streams(WsStreamConstants::TRADE_STREAM_TYPE, trade_configs);
    open_streams("ticker", ticker_configs);
    open_streams(WsStreamConstants::BOOK_TICKER_STREAM_TYPE,
                 book_ticker_configs);

    if (!polymarket_ws_url_.empty()) {
        auto pm_stream = std::make_unique<WsStream>(
            polymarket_ws_url_,
            [this](const std::string &msg) {
                handle_polymarket_message(msg);
            },
            "", info_log_callback, error_log_callback);
        pm_stream->connect();
        {
            std::lock_guard<std::mutex> lock(streams_mutex_);
            streams.push_back(StreamMeta{
                {}, std::move(pm_stream),
                WsStreamConstants::POLYMARKET_STREAM_TYPE});
        }
        LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_STREAM_CONNECTED,
                 "Connected to polymarket stream: " + polymarket_ws_url_,
                 nlohmann::json{});
    }

    auto now_tp = std::chrono::system_clock::now();
    auto now_t = std::chrono::system_clock::to_time_t(now_tp);
    std::tm tm{};
    gmtime_r(&now_t, &tm);
    char ts_buf[64];
    std::snprintf(ts_buf, sizeof(ts_buf), "%04d-%02d-%02d_%02d-%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min);
    std::string health_db_path =
        MarketData::Config::getInstance().get_data_dir_root() +
        "/caches/data_recorder/stream_health_" + ts_buf + ".db";
    health_monitor_ = std::make_unique<StreamHealthMonitor>(
        health_db_path, std::chrono::steady_clock::now());

    sm_syncer_ = std::make_unique<StateMachineSyncer>();

    std::thread([this]() { health_check_loop(); }).detach();
    std::thread([this]() { bars_utils_thread(); }).detach();
    std::thread([this]() { ohlcv_msg_consumer_thread(); }).detach();
    std::thread([this]() { lob_msg_consumer_thread(); }).detach();
    std::thread([this]() { trade_msg_consumer_thread(); }).detach();
    std::thread([this]() { book_ticker_msg_consumer_thread(); }).detach();
    std::thread([this]() { ticker_msg_consumer_thread(); }).detach();
    std::thread([this]() { polymarket_msg_consumer_thread(); }).detach();
    std::thread([this]() { check_completed_bars(); }).detach();
    std::thread([this]() { run_refresh_orderbooks_check(); }).detach();
    std::thread([this]() {
        // Wait for trade streams to start delivering events
        std::this_thread::sleep_for(std::chrono::seconds(10));
        auto last_persist = std::chrono::steady_clock::now();
        while (!this->should_stop_reconnecting) {
            if (sm_syncer_ &&
                MarketData::Config::getInstance()
                    .get_is_state_machines_enabled()) {
                sm_syncer_->sync_state_machines();
                auto now = std::chrono::steady_clock::now();
                if (now - last_persist >= std::chrono::seconds(30)) {
                    sm_syncer_->persist_state();
                    last_persist = now;
                }
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }).detach();
}

void MarketData::MarketDataStreams::set_is_ran_from_live_infra(
    bool is_ran_from_live_infra) {
    this->is_ran_from_live_infra = is_ran_from_live_infra;
}

void MarketData::MarketDataStreams::set_keep_forming_klines(bool keep) {
    keep_forming_klines.store(keep, std::memory_order_relaxed);
}

void MarketData::MarketDataStreams::set_live_event_callback(
    std::function<void(MarketData::MarketEvent &&)> callback) {
    this->live_event_callback_ = std::move(callback);
}

void MarketData::MarketDataStreams::stop_streams() {
    LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_STREAM_STOPPING,
             "Stopping streams", nlohmann::json{});
    std::lock_guard<std::mutex> lock(streams_mutex_);
    for (const auto &stream : this->streams) {
        stream.stream->disconnect();
    }
    this->should_stop_reconnecting = true;
}

MarketData::MarketDataStreams &MarketData::MarketDataStreams::get_instance() {
    static MarketDataStreams instance;
    return instance;
}

void MarketData::MarketDataStreams::set_log_callbacks(
    const LogCallback &error_log_callback,
    const LogCallback &info_log_callback) {
    this->error_log_callback = error_log_callback;
    this->info_log_callback = info_log_callback;
}

void MarketData::MarketDataStreams::health_check_loop() {
    while (!this->should_stop_reconnecting) {
        try {
            std::unique_lock<std::mutex> streams_lock(streams_mutex_);
            long long streams_alive = 0;
            long long now_ms = DatetimeUtils::get_current_time_ms();
            auto now_steady = std::chrono::steady_clock::now();
            std::vector<StreamStatus> pass_statuses;
            pass_statuses.reserve(this->streams.size());

            for (int i = 0; i < static_cast<int>(this->streams.size()); i++) {
                const auto &stream = this->streams[i];
                bool is_disconnected = stream.stream->is_disconnected();

                std::string exchange_name;
                std::string market_type;
                if (!stream.stream_infos.empty()) {
                    exchange_name = stream.stream_infos[0].exchange_name;
                    market_type = stream.stream_infos[0].market_type;
                }

                std::string stream_key =
                    StreamHealthMonitor::build_stream_key(
                        stream.stream_infos, stream.stream_type, i);

                if (is_disconnected) {
                    if (!exchange_name.empty()) {
                        auto msg = "Reconnecting to stream: " +
                                   exchange_name + ":" + market_type +
                                   " - " + stream.stream->wsurl_;
                        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_STREAM_RECONNECTING,
                                  msg, nlohmann::json{});
                        this->error_log_callback(msg);
                    }
                    if (stream.stream_type ==
                        WsStreamConstants::TRADE_STREAM_TYPE) {
                        for (const auto &stream_info : stream.stream_infos) {
                            BarAggregator::getInstance()
                                .live_trade_stream_disconnected_reset(
                                    stream_info.exchange_name,
                                    stream_info.market_type,
                                    stream_info.symbol);
                            for (auto &sm : StateMachineDispatcher::
                                     get_instance()
                                         .get_machines()) {
                                sm->stream_disconnected(
                                    stream_info.symbol,
                                    stream_info.exchange_name,
                                    stream_info.market_type,
                                    MarketEvent::TRADE);
                            }
                        }
                    }
                    if (stream.stream_type ==
                        WsStreamConstants::BOOK_TICKER_STREAM_TYPE) {
                        for (const auto &stream_info : stream.stream_infos) {
                            for (auto &sm : StateMachineDispatcher::
                                     get_instance()
                                         .get_machines()) {
                                sm->stream_disconnected(
                                    stream_info.symbol,
                                    stream_info.exchange_name,
                                    stream_info.market_type,
                                    MarketEvent::BBA);
                            }
                        }
                    }
                    if (stream.stream_type ==
                        WsStreamConstants::LOB_STREAM_TYPE) {
                        for (const auto &stream_info : stream.stream_infos) {
                            for (auto &sm : StateMachineDispatcher::
                                     get_instance()
                                         .get_machines()) {
                                sm->stream_disconnected(
                                    stream_info.symbol,
                                    stream_info.exchange_name,
                                    stream_info.market_type,
                                    MarketEvent::L2_UPDATE);
                            }
                        }
                    }

                    if (health_monitor_) {
                        health_monitor_->record_disconnect(stream_key, now_ms);
                    }

                    auto stream_ptr = stream.stream.get();
                    std::thread([this, stream_ptr, stream_key]() {
                        try {
                            stream_ptr->connect();
                        } catch (const std::exception &e) {
                            auto msg = "Reconnect failed for " + stream_key +
                                       ": " + e.what();
                            LOG_ERROR(
                                Core::
                                    TRADING_LOG_EVENT_TYPE_MARKET_DATA_STREAM_RECONNECTING,
                                msg, nlohmann::json{});
                            this->error_log_callback(msg);
                        } catch (...) {
                            auto msg = "Reconnect failed for " + stream_key +
                                       ": unknown exception";
                            LOG_ERROR(
                                Core::
                                    TRADING_LOG_EVENT_TYPE_MARKET_DATA_STREAM_RECONNECTING,
                                msg, nlohmann::json{});
                            this->error_log_callback(msg);
                        }
                    }).detach();
                } else {
                    ++streams_alive;
                    stream.stream->send_ping();
                }

                auto last_msg_age_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        now_steady - stream.stream->get_last_msg_received())
                        .count();

                if (health_monitor_) {
                    long long total_msg_count =
                        stream.stream->get_msg_count();
                    auto uptime_s =
                        std::chrono::duration_cast<std::chrono::seconds>(
                            now_steady - health_monitor_->get_process_start())
                            .count();
                    double avg_msgs_per_sec =
                        uptime_s > 0
                            ? static_cast<double>(total_msg_count) / uptime_s
                            : 0.0;

                    health_monitor_->upsert_stream_status(
                        stream_key, stream.stream_type, exchange_name,
                        market_type, stream.stream->wsurl_,
                        stream.stream_infos, !is_disconnected,
                        last_msg_age_ms, total_msg_count, avg_msgs_per_sec,
                        now_ms);
                }

                // US equities are silent outside 04:00-20:00 ET, so a quiet
                // Polygon stream is healthy then. Hold its clock rather than
                // exempting it, so the grace window restarts at the open.
                const bool venue_is_silent_by_schedule =
                    exchange_name == "polygon" &&
                    !MarketData::is_time_ms_within_extended_trading_hours(
                        now_ms);
                if (venue_is_silent_by_schedule) {
                    stream.stream->hold_staleness_clock();
                }

                bool is_stale = false;
                if (!stream.stream_infos.empty() && !is_disconnected &&
                    !venue_is_silent_by_schedule) {
                    long long threshold =
                        get_staleness_threshold_ms(stream.stream_type);
                    if (last_msg_age_ms > threshold) {
                        is_stale = true;
                        std::string symbols_str;
                        for (const auto &si : stream.stream_infos) {
                            if (!symbols_str.empty()) symbols_str += ",";
                            symbols_str += si.symbol;
                        }
                        auto msg =
                            "Stream " + exchange_name + ":" + market_type +
                            ":" + stream.stream_type + " [" + symbols_str +
                            "] stale for " +
                            std::to_string(last_msg_age_ms / 1000) +
                            "s (threshold: " +
                            std::to_string(threshold / 1000) +
                            "s) url=" + stream.stream->wsurl_ +
                            " — forcing disconnect";
                        LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_STREAM_DISCONNECTED,
                                  msg, nlohmann::json{});
                        this->error_log_callback(msg);
                        stream.stream->disconnect();
                        stream.stream->is_disconnected_.store(true);
                        if (health_monitor_) {
                            health_monitor_->record_force_disconnect(
                                stream_key, now_ms, last_msg_age_ms);
                        }
                    }
                }

                StreamStatus status;
                status.key = stream_key;
                status.stream_type = stream.stream_type;
                status.exchange_name = exchange_name;
                status.market_type = market_type;
                status.symbols.reserve(stream.stream_infos.size());
                for (const auto &si : stream.stream_infos) {
                    status.symbols.push_back(si.symbol);
                }
                status.host =
                    StreamStatusUtils::url_host(stream.stream->wsurl_);
                status.is_connected = !is_disconnected && !is_stale;
                status.is_held = venue_is_silent_by_schedule;
                status.is_stale = is_stale;
                status.last_msg_age_ms = last_msg_age_ms;
                status.msg_count = stream.stream->get_msg_count();
                status.sampled_at_ms = now_ms;
                if (health_monitor_) {
                    const auto counts =
                        health_monitor_->get_event_counts(stream_key);
                    status.disconnects = counts.disconnects;
                    status.reconnects = counts.reconnects;
                    status.forced_disconnects = counts.forced_disconnects;
                }
                pass_statuses.push_back(std::move(status));
            }
            {
                std::lock_guard<std::mutex> lock(stream_statuses_mutex_);
                stream_statuses_ = std::move(pass_statuses);
            }
            if (now_steady - this->last_logged_streams_status_time >
                std::chrono::seconds(60)) {
                auto msg = "Streams alive: " + std::to_string(streams_alive) +
                           "/" + std::to_string(this->streams.size());
                LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_STREAM_THROUGHPUT,
                         msg, nlohmann::json{});
                this->info_log_callback(msg);
                this->last_logged_streams_status_time = now_steady;

                if (health_monitor_) {
                    health_monitor_->append_health_log(
                        now_ms, static_cast<int>(streams_alive),
                        static_cast<int>(this->streams.size()));
                }
            }
            streams_lock.unlock();
            std::this_thread::sleep_for(std::chrono::seconds(5));
            BarAggregator::getInstance().reread_klines_from_disk();
        } catch (...) {
            LOG_ERROR(Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_STREAM_PARSE_ERROR,
                      "Error in health check loop", nlohmann::json{});
        }
    }
}

void MarketData::MarketDataStreams::run_refresh_orderbooks_check() {
    std::unordered_map<std::string, std::chrono::steady_clock::time_point>
        last_fetch_times;
    std::unordered_map<std::string, int> failure_counts;
    auto last_log_time = std::chrono::steady_clock::time_point{};

    while (!this->should_stop_reconnecting) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        auto order_books_out_of_sync =
            Core::Engine::getInstance().get_order_books_out_of_sync();

        auto now = std::chrono::steady_clock::now();

        if (now - last_log_time > std::chrono::seconds(60)) {
            LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_ORDERBOOK_OUT_OF_SYNC,
                     "Order books out of sync: " +
                         std::to_string(order_books_out_of_sync.size()),
                     nlohmann::json{});
            last_log_time = now;
        }

        std::vector<std::string> keys_to_fetch;
        for (const auto &symbol_exchange_market_key : order_books_out_of_sync) {
            if (keys_to_fetch.size() >= 10) {
                break;
            }

            int failures = failure_counts[symbol_exchange_market_key];
            if (failures >= 10) {
                continue;
            }

            long long cooldown_seconds = failures >= 3 ? 300 : 30;

            auto it = last_fetch_times.find(symbol_exchange_market_key);
            if (it != last_fetch_times.end()) {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                   now - it->second)
                                   .count();
                if (elapsed < cooldown_seconds) {
                    continue;
                }
            }

            keys_to_fetch.push_back(symbol_exchange_market_key);
            last_fetch_times[symbol_exchange_market_key] = now;
        }

        constexpr size_t BATCH_SIZE = 2;
        for (size_t i = 0; i < keys_to_fetch.size(); i += BATCH_SIZE) {
            size_t batch_end = std::min(i + BATCH_SIZE, keys_to_fetch.size());
            std::vector<std::future<void>> futures;

            for (size_t j = i; j < batch_end; ++j) {
                const auto &key = keys_to_fetch[j];
                futures.push_back(std::async(
                    std::launch::async,
                    [this, key, &failure_counts]() {
                        const auto full_key_parts =
                            Core::Utils::get_full_key_parts(key);

                        const auto &orderbook =
                            CcxtProxy::instance().get_full_orderbook(
                                full_key_parts.exchange_name.value(),
                                full_key_parts.market_type.value(),
                                full_key_parts.symbol.value());

                        if (orderbook.has_value()) {
                            this->enqueue_full_refresh(
                                full_key_parts.exchange_name.value(),
                                full_key_parts.market_type.value(),
                                full_key_parts.symbol.value(),
                                orderbook.value());
                            failure_counts[key] = 0;
                        } else {
                            failure_counts[key]++;
                        }
                    }));
            }

            for (auto &future : futures) {
                future.wait();
            }
        }
    }
}

void MarketData::MarketDataStreams::backup_fetch_ohlcv_klines(
    const std::shared_ptr<OhlcvDataset> &dataset) {
    const auto latest_kline_open_time_ms =
        dataset->get_latest_bar_open_time_ms();

    if (!latest_kline_open_time_ms.has_value()) {
        return;
    }

    long long latest_kline_open_time_ms_value =
        latest_kline_open_time_ms.value();

    long long latest_fully_completed_kline_open_ms =
        MarketData::get_latest_fully_completed_kline_open_ms(
            dataset->meta.bar_close_policy.time_ms);

    if (latest_kline_open_time_ms_value ==
        latest_fully_completed_kline_open_ms) {
        return;
    }

    if (this->is_ran_from_live_infra) {
        ::Core::Engine::getInstance().create_system_log(
            dataset->meta.symbol, Core::LOG_LEVEL_INFO, "Backup fetching bars",
            Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_BAR_SYNC_PROGRESS,
            nlohmann::json::object(), -1);
    }

    // Real-time gap-fill — only a few bars needed at most. limit=100 drops
    // Binance's REST weight per call from 5 (at default limit=1000) to 1,
    // critical when the periodic check fans out across every active symbol
    // at every kline boundary. Startup backfill (ohlcv_dataset.cpp:407)
    // keeps the default 1000 because it pulls many pages at once.
    std::vector<MarketData::OhlcvDatasetTick> ticks =
        CcxtProxy::instance().get_ohlcv_dataset_ticks(
            dataset->meta.exchange_name, dataset->meta.market_type,
            dataset->meta.symbol,
            convert_interval_ms_to_str(dataset->meta.bar_close_policy.time_ms),
            latest_kline_open_time_ms_value -
                dataset->meta.bar_close_policy.time_ms - 60000,
            /*limit=*/100);

    for (const auto &tick : ticks) {
        // is_called_from_init_fetch=false: this is a realtime gap-fill, not the
        // initial backfill. Claiming otherwise bypasses add_new_kline's
        // pre-sync gate, so a dataset whose genesis->now backfill has not run
        // yet gets a handful of *current* bars written to disk. The next
        // update-mode run then resumes from that future-dated suffix and
        // orphans every bar before it -- the whole history, permanently. This
        // is how ~6k polygon tickers ended up holding ~24 rows each and nothing
        // else. Before init_fetch_done these ticks are dropped, which costs
        // nothing: the backfill fetches that same span anyway.
        dataset->add_new_kline(tick, false);
    }
}

void MarketData::MarketDataStreams::run_backup_fetch_check(
    const MarketData::Store &store,
    std::unordered_map<std::string,
                       std::vector<std::chrono::steady_clock::time_point>>
        &backup_fetch_timestamps,
    std::unordered_map<std::string, std::unordered_map<long long, int>>
        &kline_attempt_counts) {
    long long now_ms = DatetimeUtils::get_current_time_ms();
    std::chrono::steady_clock::time_point now_time =
        std::chrono::steady_clock::now();

    auto exponential_backoff_cutoff = now_time - std::chrono::seconds(60);

    std::vector<std::shared_ptr<OhlcvDataset>> datasets_needing_backup;
    for (const auto &dataset : store.ohlcv_datasets_vec) {
        if (dataset->meta.exchange_name == "polygon") {
            continue;
        }

        if (!Core::ExchangeInfo::getInstance().is_symbol_active(
                dataset->meta.symbol, dataset->meta.exchange_name,
                dataset->meta.market_type)) {
            continue;
        }

        if (!dataset->get_init_fetch_done() || !dataset->is_exchange_bar()) {
            continue;
        }

        const auto &latest_kline_open_time_ms =
            dataset->get_latest_bar_open_time_ms();

        if (!latest_kline_open_time_ms.has_value()) {
            continue;
        }

        long long latest_kline_open_time_ms_value =
            latest_kline_open_time_ms.value();

        long long latest_fully_completed_kline_open_ms =
            MarketData::get_latest_fully_completed_kline_open_ms(
                dataset->meta.bar_close_policy.time_ms);

        if (now_ms - (latest_fully_completed_kline_open_ms +
                      dataset->meta.bar_close_policy.time_ms) <
            2000) {
            continue;
        }

        if (latest_fully_completed_kline_open_ms >
            latest_kline_open_time_ms_value) {
            std::string symbol_key = dataset->meta.exchange_name + ":" +
                                     dataset->meta.market_type + ":" +
                                     dataset->meta.symbol;

            auto &attempt_map = kline_attempt_counts[symbol_key];
            if (attempt_map.count(latest_kline_open_time_ms_value) &&
                attempt_map[latest_kline_open_time_ms_value] >= 3) {
                continue;
            }

            auto &timestamps = backup_fetch_timestamps[symbol_key];
            timestamps.erase(
                std::remove_if(timestamps.begin(), timestamps.end(),
                               [&exponential_backoff_cutoff](const auto &ts) {
                                   return ts < exponential_backoff_cutoff;
                               }),
                timestamps.end());

            int recent_count = static_cast<int>(timestamps.size());
            int wait_seconds = std::min(45, 2 * (1 << recent_count));

            if (timestamps.empty() || now_time - timestamps.back() >
                                          std::chrono::seconds(wait_seconds)) {
                timestamps.push_back(now_time);
                attempt_map[latest_kline_open_time_ms_value]++;
                datasets_needing_backup.push_back(dataset);
            }
        }
    }

    if (datasets_needing_backup.empty()) {
        return;
    }

    // 50 → 15 → 5: at every 15m boundary up to ~600 symbols can need a
    // gap-fill simultaneously. At 15-way concurrency with ~300ms/call, the
    // burst peaks at ~3000 req/min — overshooting Binance's 2400/min IP
    // window once other binance traffic (depth, inventory, bar gap-fill for
    // the new 1s-bar strategy) is layered on top. 5-way spreads the same
    // 600-symbol fan-out over ~36s at ~1000 req/min, leaving ~1400 req/min
    // headroom for everything else. Net cost: kline gap-fill takes ~3x
    // longer to drain on bursts (~36s vs ~12s), which is fine since the
    // backup is real-time and bars only become stale by a kline interval.
    // Sized per venue: the 5-way figure above is a Binance IP-budget
    // calculation and does not transfer. Polygon's budget is ~3x larger and
    // its equity fan-out is ~20x wider, so holding it to 5 makes a 15m-boundary
    // gap-fill burst take minutes to drain.
    std::unordered_map<std::string,
                       std::vector<std::shared_ptr<MarketData::OhlcvDataset>>>
        by_exchange;
    for (const auto &dataset : datasets_needing_backup) {
        by_exchange[dataset->meta.exchange_name].push_back(dataset);
    }

    std::vector<std::unique_ptr<ThreadPool>> pools;
    std::vector<std::future<void>> futures;
    futures.reserve(datasets_needing_backup.size());

    for (auto &[exchange_name, exchange_datasets] : by_exchange) {
        auto pool = std::make_unique<ThreadPool>(
            exchange_name == "polygon" ? 32 : 5);
        for (const auto &dataset : exchange_datasets) {
            futures.push_back(pool->enqueue(
                [this, dataset]() { backup_fetch_ohlcv_klines(dataset); }));
        }
        pools.push_back(std::move(pool));
    }

    for (auto &future : futures) {
        future.wait();
    }
}

void MarketData::MarketDataStreams::bars_utils_thread() {
    Store store = MarketData::DatasetsManager::get_instance().copy_store();

    std::chrono::system_clock::time_point last_copied_store_time =
        std::chrono::system_clock::now();

    std::unordered_map<std::string,
                       std::vector<std::chrono::steady_clock::time_point>>
        backup_fetch_timestamps;
    std::unordered_map<std::string, std::unordered_map<long long, int>>
        kline_attempt_counts;

    while (!this->should_stop_reconnecting) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));

        long long now_ms = DatetimeUtils::get_current_time_ms();

        if (this->last_spawned_sync_to_disk_thread_time +
                std::chrono::seconds(10) <
            std::chrono::steady_clock::now()) {
            this->last_spawned_sync_to_disk_thread_time =
                std::chrono::steady_clock::now();
            std::thread([this]() {
                MarketData::DatasetsManager::get_instance()
                    .sync_ohlcv_datasets_to_disk();
            }).detach();
        }

        if (std::chrono::system_clock::now() - last_copied_store_time >
            std::chrono::minutes(1)) {
            store = MarketData::DatasetsManager::get_instance().copy_store();
        }

        run_backup_fetch_check(store, backup_fetch_timestamps,
                               kline_attempt_counts);
    }
}
