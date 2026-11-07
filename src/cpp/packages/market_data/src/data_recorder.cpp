#include "market_data/data_recorder.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

#include "datetime_utils.h"
#include "market_data/config.h"
#include "simdjson.h"

namespace MarketData {

DataRecorder& DataRecorder::getInstance() {
    static DataRecorder instance;
    return instance;
}

DataRecorder::DataRecorder() {
    const char* env_enabled = std::getenv("IS_RECORDING_DATA");
    if (!env_enabled || std::string(env_enabled) != "1") {
        enabled_ = false;
        return;
    }

    data_dir_root_ = Config::getInstance().get_data_dir_root();
    if (data_dir_root_.empty()) {
        std::cerr << "[DataRecorder] DATA_DIR_ROOT is empty, disabling recorder"
                  << std::endl;
        enabled_ = false;
        return;
    }

    enabled_ = true;
    writer_thread_ = std::thread(&DataRecorder::writer_thread_loop, this);

    std::cout << "[DataRecorder] Enabled" << std::endl;
}

DataRecorder::~DataRecorder() { shutdown(); }

void DataRecorder::set_recording_keys(
    const std::vector<ResolvedRecordingKey>& keys) {
    std::unordered_set<std::string> specific;
    std::unordered_set<std::string> wildcard;
    for (const auto& key : keys) {
        const std::string base_key =
            key.stream_type + "|" + key.exchange_name + "|" + key.market_type;
        if (key.symbol.has_value()) {
            specific.insert(base_key + "|" + *key.symbol);
        } else {
            wildcard.insert(base_key);
        }
    }
    std::unique_lock<std::shared_mutex> lock(keys_mutex_);
    recording_keys_ = std::move(specific);
    wildcard_keys_ = std::move(wildcard);
}

bool DataRecorder::is_recorded(const std::string& base_key,
                               const std::string& symbol) const {
    std::shared_lock<std::shared_mutex> lock(keys_mutex_);
    return wildcard_keys_.count(base_key) > 0 ||
           recording_keys_.count(base_key + "|" + symbol) > 0;
}

bool DataRecorder::is_enabled() const { return enabled_; }

void DataRecorder::record(const std::string& stream_type,
                          const std::string& exchange_name,
                          const std::string& market_type,
                          const std::string& raw_msg) {
    if (!enabled_) return;

    // Extract symbol from raw JSON using stream_type-aware strategy
    thread_local simdjson::dom::parser parser;
    std::string symbol;
    try {
        simdjson::dom::element doc = parser.parse(raw_msg);

        // Skip subscription confirmations
        if (doc.at_key("result").error() == simdjson::SUCCESS) {
            return;
        }

        std::string_view symbol_sv;
        bool found = false;

        if (stream_type == "book_ticker") {
            // Binance book_ticker: flat message, symbol at root
            found = (doc["s"].get(symbol_sv) == simdjson::SUCCESS);
        } else if (stream_type == "klines") {
            // Binance OHLCV: wrapped under data.k
            found = (doc["data"]["k"]["s"].get(symbol_sv) == simdjson::SUCCESS);
            // Fallback for flat messages
            if (!found) found = (doc["s"].get(symbol_sv) == simdjson::SUCCESS);
        } else if (stream_type == "polymarket") {
            // Polymarket: asset_id for most events, market for tick_size_change
            found = (doc["asset_id"].get(symbol_sv) == simdjson::SUCCESS);
            if (!found)
                found = (doc["market"].get(symbol_sv) == simdjson::SUCCESS);
        } else {
            // trade, lob: wrapped under data
            found = (doc["data"]["s"].get(symbol_sv) == simdjson::SUCCESS);
            // Fallback for flat messages (e.g. gateio or other formats)
            if (!found) found = (doc["s"].get(symbol_sv) == simdjson::SUCCESS);
        }

        if (!found) return;
        symbol = std::string(symbol_sv);
    } catch (...) {
        return;
    }

    std::string base_key =
        stream_type + "|" + exchange_name + "|" + market_type;

    if (!is_recorded(base_key, symbol)) return;

    RecordMsg record_msg;
    record_msg.stream_type = stream_type;
    record_msg.exchange_name = exchange_name;
    record_msg.market_type = market_type;
    record_msg.symbol = std::move(symbol);
    record_msg.raw_msg = raw_msg;
    record_msg.timestamp_ms = DatetimeUtils::get_current_time_ms();

    queue_.enqueue(std::move(record_msg));
}

void DataRecorder::record_with_symbol(const std::string& stream_type,
                                      const std::string& exchange_name,
                                      const std::string& market_type,
                                      const std::string& symbol,
                                      const std::string& raw_msg) {
    if (!enabled_) return;

    std::string base_key =
        stream_type + "|" + exchange_name + "|" + market_type;

    if (!is_recorded(base_key, symbol)) return;

    RecordMsg record_msg;
    record_msg.stream_type = stream_type;
    record_msg.exchange_name = exchange_name;
    record_msg.market_type = market_type;
    record_msg.symbol = symbol;
    record_msg.raw_msg = raw_msg;
    record_msg.timestamp_ms = DatetimeUtils::get_current_time_ms();

    queue_.enqueue(std::move(record_msg));
}

std::string DataRecorder::build_file_path(const std::string& stream_type,
                                          const std::string& exchange_name,
                                          const std::string& market_type,
                                          const std::string& symbol,
                                          long long timestamp_ms) const {
    Date date = DatetimeUtils::ts_to_date(timestamp_ms);
    int hour = static_cast<int>((timestamp_ms / 3600000) % 24);

    std::ostringstream path;
    path << data_dir_root_ << "/raw/recordings/" << stream_type << "/"
         << exchange_name << "/" << market_type << "/" << symbol << "/jsonl/year="
         << std::setfill('0') << std::setw(4) << date.year
         << "/month=" << std::setfill('0') << std::setw(2) << date.month
         << "/day=" << std::setfill('0') << std::setw(2) << date.day << "/"
         << std::setfill('0') << std::setw(4) << date.year << "-"
         << std::setfill('0') << std::setw(2) << date.month << "-"
         << std::setfill('0') << std::setw(2) << date.day << "T"
         << std::setfill('0') << std::setw(2) << hour << ".jsonl";

    return path.str();
}

void DataRecorder::writer_thread_loop() {
    constexpr size_t BULK_SIZE = 1024;
    std::vector<RecordMsg> bulk_buffer(BULK_SIZE);

    std::unordered_map<std::string, std::ofstream> open_handles;
    auto last_flush_time = std::chrono::steady_clock::now();
    long long handles_hour = -1;

    while (!should_stop_.load(std::memory_order_relaxed)) {
        size_t count =
            queue_.try_dequeue_bulk(bulk_buffer.data(), BULK_SIZE);

        if (count == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        for (size_t i = 0; i < count; ++i) {
            auto& msg = bulk_buffer[i];

            // Strategy event branch — dedup + separate path
            if (msg.stream_type == "__strategy__") {
                if (!msg.dedup_key.empty()) {
                    auto [_, inserted] =
                        strategy_dedup_set_.insert(msg.dedup_key);
                    if (!inserted) continue;
                }

                // Clear dedup set hourly to bound memory
                if (dedup_last_clear_ts_ == 0) {
                    dedup_last_clear_ts_ = msg.timestamp_ms;
                } else if (msg.timestamp_ms - dedup_last_clear_ts_ >= 3600000) {
                    strategy_dedup_set_.clear();
                    dedup_last_clear_ts_ = msg.timestamp_ms;
                }

                std::string file_path = build_strategy_file_path(
                    msg.exchange_name, msg.timestamp_ms);

                auto it = open_handles.find(file_path);
                if (it == open_handles.end()) {
                    std::filesystem::path p(file_path);
                    std::filesystem::create_directories(p.parent_path());

                    auto [inserted_it, _] = open_handles.emplace(
                        file_path, std::ofstream(file_path,
                                                 std::ios::app |
                                                     std::ios::binary));
                    if (!inserted_it->second.is_open()) {
                        std::cerr << "[DataRecorder] Failed to open: "
                                  << file_path << std::endl;
                        open_handles.erase(inserted_it);
                        continue;
                    }
                    it = inserted_it;
                }

                it->second << msg.raw_msg << "\n";
                continue;
            }

            std::string file_path = build_file_path(
                msg.stream_type, msg.exchange_name, msg.market_type,
                msg.symbol, msg.timestamp_ms);

            auto it = open_handles.find(file_path);
            if (it == open_handles.end()) {
                // Create directories if needed
                std::filesystem::path p(file_path);
                std::filesystem::create_directories(p.parent_path());

                auto [inserted_it, _] = open_handles.emplace(
                    file_path,
                    std::ofstream(file_path, std::ios::app | std::ios::binary));

                if (!inserted_it->second.is_open()) {
                    std::cerr << "[DataRecorder] Failed to open: " << file_path
                              << std::endl;
                    open_handles.erase(inserted_it);
                    continue;
                }
                it = inserted_it;
            }

            it->second << "{\"lt\":" << msg.timestamp_ms << ","
                       << std::string_view(msg.raw_msg).substr(1) << "\n";
        }

        // Flush all handles every 5 seconds
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now -
                                                             last_flush_time)
                .count() >= 5) {
            for (auto& [path, handle] : open_handles) {
                handle.flush();
            }
            last_flush_time = now;
            // Files are hourly: close them all two minutes into each hour so
            // past hours' handles don't accumulate (late writes reopen).
            const long long hour =
                (DatetimeUtils::get_current_time_ms() - 120000) / 3600000;
            if (hour != handles_hour) {
                open_handles.clear();
                handles_hour = hour;
            }
        }
    }

    // Drain remaining messages
    size_t count;
    while ((count = queue_.try_dequeue_bulk(bulk_buffer.data(), BULK_SIZE)) >
           0) {
        for (size_t i = 0; i < count; ++i) {
            auto& msg = bulk_buffer[i];

            if (msg.stream_type == "__strategy__") {
                std::string file_path = build_strategy_file_path(
                    msg.exchange_name, msg.timestamp_ms);
                auto it = open_handles.find(file_path);
                if (it != open_handles.end() && it->second.is_open()) {
                    it->second << msg.raw_msg << "\n";
                }
                continue;
            }

            std::string file_path = build_file_path(
                msg.stream_type, msg.exchange_name, msg.market_type,
                msg.symbol, msg.timestamp_ms);

            auto it = open_handles.find(file_path);
            if (it != open_handles.end() && it->second.is_open()) {
                it->second << "{\"lt\":" << msg.timestamp_ms << ","
                           << std::string_view(msg.raw_msg).substr(1) << "\n";
            }
        }
    }

    // Close all handles
    for (auto& [path, handle] : open_handles) {
        if (handle.is_open()) {
            handle.flush();
            handle.close();
        }
    }
}

void DataRecorder::record_strategy_event(const std::string& strategy_name,
                                         const std::string& dedup_key,
                                         const std::string& json_line) {
    if (!enabled_) return;

    RecordMsg msg;
    msg.stream_type = "__strategy__";
    msg.exchange_name = strategy_name;
    msg.raw_msg = json_line;
    msg.dedup_key = dedup_key;
    msg.timestamp_ms = DatetimeUtils::get_current_time_ms();

    queue_.enqueue(std::move(msg));
}

std::string DataRecorder::build_strategy_file_path(
    const std::string& strategy_name, long long timestamp_ms) const {
    Date date = DatetimeUtils::ts_to_date(timestamp_ms);
    int hour = static_cast<int>((timestamp_ms / 3600000) % 24);

    std::ostringstream path;
    path << data_dir_root_ << "/strategy_recordings/" << strategy_name
         << "/year=" << std::setfill('0') << std::setw(4) << date.year
         << "/month=" << std::setfill('0') << std::setw(2) << date.month
         << "/day=" << std::setfill('0') << std::setw(2) << date.day << "/"
         << std::setfill('0') << std::setw(4) << date.year << "-"
         << std::setfill('0') << std::setw(2) << date.month << "-"
         << std::setfill('0') << std::setw(2) << date.day << "T"
         << std::setfill('0') << std::setw(2) << hour << ".jsonl";
    return path.str();
}

void DataRecorder::shutdown() {
    if (!enabled_) return;
    if (should_stop_.exchange(true)) return;  // Already shutting down

    if (writer_thread_.joinable()) {
        writer_thread_.join();
    }
}

}  // namespace MarketData
