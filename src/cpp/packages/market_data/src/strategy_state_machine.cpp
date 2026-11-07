#include "market_data/strategy_state_machine.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>

#include "common_types/log_event_types.h"
#include "disk_logging/log.h"

namespace MarketData {

// Extra slack added below the warm-up horizon when clamping a stale anchor,
// so the backfilled span comfortably exceeds the SM's emit-gate window.
static constexpr long long WARMUP_MARGIN_SEC = 3600;

void StrategyStateMachine::init_dependencies() {
    std::call_once(init_deps_flag_, [this]() {
        auto deps = get_market_data_dependencies();
        if (deps.empty()) {
            is_fully_synced_.store(true);
            deps_initialized_.store(true, std::memory_order_release);
            return;
        }

        for (auto& dep : deps) {
            std::string key = dep.get_key();
            dep_keys_order_.push_back(key);
            dependencies_.emplace(key, std::move(dep));
        }
        deps_initialized_.store(true, std::memory_order_release);
    });
}

void StrategyStateMachine::prepare_historical() {
    initialize_symbol_states();
    set_fully_synced();
    init_dependencies();
    has_loaded_state_ = deserialize_state();
    warmup_from_bars();
}

void StrategyStateMachine::prepare_live() {
    initialize_symbol_states();
    init_dependencies();
    for (auto& [k, dep] : dependencies_) {
        if (dep.is_optional) dep.mark_synced();
        // Bar (OHLCV) deps don't go through the tick syncer — they're
        // warmup-from-parquet, no REST backfill exists. Pre-marking them as
        // synced lets the live gate proceed once the warmup hook has filled
        // the SM's internal state. Subclasses owning bar deps override
        // warmup_from_bars() to load history from the dataset store.
        if (dep.event_type == MarketEvent::OHLCV) dep.mark_synced();
    }
    live_mode_ = true;
    use_react_lock_ = true;
    has_loaded_state_ = deserialize_state();
    // deserialize_state() applies the warm-up floor when a state.bin exists.
    // Call it again here so the floor is set even with NO state.bin (cold
    // start) — cursors seeded at 0 are below the floor and get clamped to
    // now-required-buffer. The syncer re-applies it after its own reload.
    if (apply_bounded_warmup_floor()) has_loaded_state_ = false;
    warmup_from_bars();
}

void StrategyStateMachine::react(const MarketEvent& event, bool is_historical) {
    init_dependencies();
    // react_impl is a WRITER (mutates state, single-writer seqlock). It must
    // be serialized — a shared_lock here let the syncer thread and the live
    // dispatcher run react_impl concurrently, corrupting the seqlock parity so
    // emit_state_for_symbol saw "writer active" forever. Exclusive lock =
    // single writer at a time; lock-free readers stay protected by the seqlock.
    std::optional<std::unique_lock<std::shared_mutex>> lock;
    if (use_react_lock_) lock.emplace(state_mutex_);

    // Find matching dependency
    SMMarketDataDependency* dep = nullptr;
    if (!dependencies_.empty()) {
        auto it = dependencies_.find(get_event_dependency_key(event));
        if (it != dependencies_.end()) {
            dep = &it->second;
        }
    }

    long long event_ts = get_event_timestamp_ms(event);

    // Historical replay path
    if (is_historical) {
        if (dep && dep->check_synced()) return;  // live sync dedup
        if (dep && event_ts < dep->last_synced_ts_ms)
            return;  // already processed
        react_impl(event);
        if (dep) dep->advance_synced_ts(event_ts);

        if (dep && !dep->check_synced() && dep->first_live_event_ts_ms > 0 &&
            dep->last_synced_ts_ms >= dep->first_live_event_ts_ms) {
            dep->mark_synced();
            flush_dep_buffer(dep);
            if (all_dependencies_synced()) {
                flush_all_dep_buffers_and_go_live();
            }
        }
        return;
    }

    // Live, fully synced
    if (is_fully_synced_.load(std::memory_order_acquire)) {
        react_impl(event);
        if (dep) dep->advance_synced_ts(event_ts);
        return;
    }

    // Live, not synced
    if (dep) {
        dep->set_first_live_event(event_ts);

        // Auto-sync non-trade deps (no historical backfill needed)
        if (!dep->check_synced() && dep->event_type != MarketEvent::TRADE) {
            dep->mark_synced();
            react_impl(event);
            dep->advance_synced_ts(event_ts);
            if (all_dependencies_synced()) {
                flush_all_dep_buffers_and_go_live();
            }
            return;
        }

        // Synced dep — process immediately, skip buffer
        if (dep->check_synced()) {
            react_impl(event);
            dep->advance_synced_ts(event_ts);
            return;
        }

        // Unsynced dep — buffer
        buffer_push_dep(dep, event);
    }
}

std::optional<std::vector<float>> StrategyStateMachine::emit_state() const {
    if (!is_fully_synced_.load(std::memory_order_acquire)) {
        return std::nullopt;
    }
    std::shared_lock lock(state_mutex_);
    return emit_state_impl();
}

void StrategyStateMachine::set_fully_synced() {
    is_fully_synced_.store(true, std::memory_order_release);
}

bool StrategyStateMachine::is_fully_synced() const {
    return is_fully_synced_.load(std::memory_order_acquire);
}

std::pair<int, int> StrategyStateMachine::get_sync_progress() const {
    if (!deps_initialized_.load(std::memory_order_acquire)) return {0, 0};
    int synced = 0;
    int total = 0;
    for (const auto& [key, dep] : dependencies_) {
        ++total;
        if (dep.check_synced()) ++synced;
    }
    return {synced, total};
}

long long StrategyStateMachine::get_min_synced_ts() const {
    if (!deps_initialized_.load(std::memory_order_acquire)) return 0;
    long long min_ts = std::numeric_limits<long long>::max();
    for (const auto& [key, dep] : dependencies_) {
        if (dep.last_synced_ts_ms > 0 && dep.last_synced_ts_ms < min_ts)
            min_ts = dep.last_synced_ts_ms;
    }
    return min_ts == std::numeric_limits<long long>::max() ? 0 : min_ts;
}

bool StrategyStateMachine::all_dependencies_synced() const {
    for (const auto& [key, dep] : dependencies_) {
        if (dep.is_optional) continue;  // never blocks go-live
        if (dep.first_live_event_ts_ms == 0) return false;  // not yet seen live
        if (!dep.check_synced()) return false;
    }
    return true;
}

// --- Per-dep buffer operations ---

void StrategyStateMachine::buffer_push_dep(SMMarketDataDependency* dep,
                                           const MarketEvent& event) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    if (dep->buffer.size() < SMMarketDataDependency::MAX_BUFFER_SIZE) {
        dep->buffer.push_back(event);
    } else {
        LOG_ERROR(
            Core::TRADING_LOG_EVENT_TYPE_SM_DEPENDENCY_BUFFER_OVERFLOW,
            "SM dep buffer overflow — resetting " + dep->get_key(),
            (nlohmann::json{{"dep_key", dep->get_key()},
                            {"max_buffer_size",
                             SMMarketDataDependency::MAX_BUFFER_SIZE}}));
        dep->reset_sync();
    }
}

void StrategyStateMachine::flush_dep_buffer(SMMarketDataDependency* dep) {
    std::vector<MarketEvent> buf;
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buf.swap(dep->buffer);
    }
    for (const auto& event : buf) {
        react_impl(event);
        dep->advance_synced_ts(get_event_timestamp_ms(event));
    }
}

void StrategyStateMachine::flush_all_dep_buffers_and_go_live() {
    for (int i = 0; i < 50; ++i) {
        bool any_flushed = false;
        for (auto& [key, dep] : dependencies_) {
            std::vector<MarketEvent> buf;
            {
                std::lock_guard<std::mutex> lock(buffer_mutex_);
                buf.swap(dep.buffer);
            }
            if (!buf.empty()) {
                any_flushed = true;
                for (const auto& event : buf) {
                    react_impl(event);
                }
            }
        }
        if (!any_flushed) break;
    }
    bool was_synced = is_fully_synced_.exchange(true, std::memory_order_acq_rel);
    LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_SM_WENT_LIVE,
             "SM: all deps synced, state machine is now live",
             (nlohmann::json{{"sm_name", get_name()},
                             {"from_state", was_synced ? "live" : "syncing"},
                             {"to_state", "live"},
                             {"is_real_transition", !was_synced},
                             {"trigger", "flush_all_dep_buffers"}}));
}

// --- Stream disconnect ---

void StrategyStateMachine::stream_disconnected(const std::string& symbol,
                                               const std::string& exchange_name,
                                               const std::string& market_type,
                                               MarketEvent::Type event_type) {
    if (!deps_initialized_.load(std::memory_order_acquire)) return;
    std::string dep_key = SMMarketDataDependency::make_key(
        symbol, exchange_name, market_type, event_type);

    auto it = dependencies_.find(dep_key);
    if (it == dependencies_.end()) return;

    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        it->second.reset_sync();
    }

    bool was_synced = is_fully_synced_.exchange(false, std::memory_order_acq_rel);

    LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_SM_STREAM_DISCONNECTED_RESET,
             "SM stream_disconnected: reset " + dep_key,
             (nlohmann::json{{"sm_name", get_name()},
                             {"dep_key", dep_key},
                             {"symbol", symbol},
                             {"exchange_name", exchange_name},
                             {"market_type", market_type},
                             {"from_state", was_synced ? "live" : "syncing"},
                             {"to_state", "reset"},
                             {"was_previously_live", was_synced}}));
}

// --- Round-robin dep selection ---

SMMarketDataDependency* StrategyStateMachine::get_next_unsynced_dependency() {
    if (dep_keys_order_.empty()) return nullptr;
    size_t n = dep_keys_order_.size();
    for (size_t i = 0; i < n; ++i) {
        size_t idx = (next_dep_index_ + i) % n;
        auto& dep = dependencies_.at(dep_keys_order_[idx]);
        if (!dep.check_synced() && dep.first_live_event_ts_ms > 0) {
            next_dep_index_ = (idx + 1) % n;
            return &dep;
        }
    }
    return nullptr;
}

// --- Persistence ---

std::string StrategyStateMachine::get_persistence_dir() const {
    const char* data_dir = std::getenv("DATA_DIR_ROOT");
    if (!data_dir) return "";
    std::string dir =
        std::string(data_dir) + "/caches/state_machines/" + get_name() + "/";
    std::filesystem::create_directories(dir);
    return dir;
}

void StrategyStateMachine::serialize_state() const {
    if (!should_persist()) return;
    std::string dir = get_persistence_dir();
    if (dir.empty()) return;

    std::ostringstream buf(std::ios::binary);
    {
        std::unique_lock lock(state_mutex_);
        serialize_state_impl(buf);
        serialize_deps(buf);
    }

    std::string tmp_path = dir + "state.bin.tmp";
    std::string final_path = dir + "state.bin";
    std::ofstream f(tmp_path, std::ios::binary);
    if (!f.is_open()) return;
    f << buf.str();
    f.close();

    std::filesystem::rename(tmp_path, final_path);
    write_debug_deps_json();
}

void StrategyStateMachine::serialize_state_to_stream(std::ostream& out) const {
    std::unique_lock lock(state_mutex_);
    serialize_state_impl(out);
    serialize_deps(out);
}

bool StrategyStateMachine::deserialize_state() {
    std::string dir = get_persistence_dir();
    if (dir.empty()) return false;

    std::unique_lock lock(state_mutex_);
    std::ifstream f(dir + "state.bin", std::ios::binary);
    if (!f.is_open()) return false;

    if (!deserialize_state_impl(f)) return false;
    deserialize_deps(f);
    if (!f.good()) return false;

    // Re-assert the bounded warm-up floor on every load (live mode only).
    // deserialize_state() runs twice — prepare_live() and the syncer's reload
    // (sm_syncer.cpp) — and the second load would otherwise resurrect a
    // below-floor on-disk cursor, so re-applying here keeps it clamped. If it
    // clamps, report not-loaded so callers re-sync from the floor.
    if (apply_bounded_warmup_floor()) return false;
    return f.good();
}

// Bounded warm-up floor. For an SM whose state is fully bounded by
// required_warmup_seconds(), there is no "no starting point": any non-optional
// TRADE cursor that is missing (0) OR older than the window is worthless —
// nothing before now-required-buffer survives in any feature window. Reset the
// (now stale/empty) accumulators and clamp every below-floor TRADE cursor to
// now-required-buffer, so the syncer backfills just the bounded window from a
// clean state. Works with no state.bin at all (cursors seed at 0). Live mode
// only — prepare_historical()/training replay run with live_mode_==false.
// Returns true iff it clamped anything.
bool StrategyStateMachine::apply_bounded_warmup_floor() {
    if (!live_mode_ || !supports_bounded_warmup() ||
        required_warmup_seconds() <= 0)
        return false;
    const long long now_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    const long long floor_ts =
        now_ms - (required_warmup_seconds() + WARMUP_MARGIN_SEC) * 1000;
    const bool below = std::any_of(
        dependencies_.begin(), dependencies_.end(), [&](const auto& kv) {
            const SMMarketDataDependency& d = kv.second;
            return !d.is_optional && d.event_type == MarketEvent::TRADE &&
                   d.last_synced_ts_ms < floor_ts;
        });
    if (!below) return false;
    initialize_symbol_states();  // clean accumulators (cross-symbol consistency)
    for (auto& [k, d] : dependencies_) {
        (void)k;
        if (!d.is_optional && d.event_type == MarketEvent::TRADE &&
            d.last_synced_ts_ms < floor_ts)
            d.last_synced_ts_ms = floor_ts;
    }
    LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_SM_SYNCER_PROGRESS,
             "warmup floor: syncing from now-" +
                 std::to_string(required_warmup_seconds()) + "s-buffer for " +
                 get_name(),
             (nlohmann::json{{"sm_name", get_name()},
                             {"warmup_seconds", required_warmup_seconds()},
                             {"floor_ts_ms", floor_ts}}));
    return true;
}

void StrategyStateMachine::serialize_deps(std::ostream& out) const {
    auto num_deps = static_cast<uint32_t>(dependencies_.size());
    out.write(reinterpret_cast<const char*>(&num_deps), sizeof(num_deps));

    for (const auto& [key, dep] : dependencies_) {
        auto key_len = static_cast<uint32_t>(key.size());
        out.write(reinterpret_cast<const char*>(&key_len), sizeof(key_len));
        out.write(key.data(), key_len);

        out.write(reinterpret_cast<const char*>(&dep.first_live_event_ts_ms),
                  sizeof(dep.first_live_event_ts_ms));
        out.write(reinterpret_cast<const char*>(&dep.last_synced_ts_ms),
                  sizeof(dep.last_synced_ts_ms));

        auto tid_len = static_cast<uint32_t>(dep.last_synced_trade_id.size());
        out.write(reinterpret_cast<const char*>(&tid_len), sizeof(tid_len));
        out.write(dep.last_synced_trade_id.data(), tid_len);

        auto is_synced_val = static_cast<uint8_t>(dep.check_synced() ? 1 : 0);
        out.write(reinterpret_cast<const char*>(&is_synced_val),
                  sizeof(is_synced_val));

        auto event_type = static_cast<uint8_t>(dep.event_type);
        out.write(reinterpret_cast<const char*>(&event_type),
                  sizeof(event_type));
    }
}

void StrategyStateMachine::deserialize_deps(std::istream& in) {
    uint32_t num_deps = 0;
    in.read(reinterpret_cast<char*>(&num_deps), sizeof(num_deps));
    if (!in.good()) return;

    for (uint32_t i = 0; i < num_deps; ++i) {
        uint32_t key_len = 0;
        in.read(reinterpret_cast<char*>(&key_len), sizeof(key_len));
        std::string key(key_len, '\0');
        in.read(key.data(), key_len);

        long long first_live = 0, last_synced = 0;
        in.read(reinterpret_cast<char*>(&first_live), sizeof(first_live));
        in.read(reinterpret_cast<char*>(&last_synced), sizeof(last_synced));

        uint32_t tid_len = 0;
        in.read(reinterpret_cast<char*>(&tid_len), sizeof(tid_len));
        std::string trade_id(tid_len, '\0');
        in.read(trade_id.data(), tid_len);

        uint8_t is_synced = 0;
        in.read(reinterpret_cast<char*>(&is_synced), sizeof(is_synced));

        uint8_t event_type = 0;
        in.read(reinterpret_cast<char*>(&event_type), sizeof(event_type));

        auto it = dependencies_.find(key);
        if (it != dependencies_.end()) {
            it->second.last_synced_ts_ms = last_synced;
            it->second.last_synced_trade_id = trade_id;
            // Always start unsynced on load — syncer bridges the gap
            it->second.is_synced.store(false);
        }
    }
}

void StrategyStateMachine::write_debug_deps_json() const {
    std::string dir = get_persistence_dir();
    if (dir.empty()) return;
    std::string path = dir + "deps.json";
    nlohmann::json j = nlohmann::json::array();
    for (const auto& [key, dep] : dependencies_) {
        j.push_back(dep.to_json());
    }
    std::ofstream f(path);
    f << j.dump(2);
}

}  // namespace MarketData
