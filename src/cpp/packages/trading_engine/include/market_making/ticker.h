#pragma once

#include <array>
#include <chrono>
#include <deque>
#include <mutex>
#include <tuple>

#include "dtypes/order.h"
#include "market_making/dtypes/def_only_structs.h"
#include "market_making/dtypes/order.h"
#include "market_making/dtypes/strategy_decision.h"
#include "market_making/dtypes/trade.h"
#include "market_making/strategy.h"
#include "rolling_event_buffer.h"
#include "sqlite_datasets.h"

namespace Core {

constexpr long long POLYMARKET_ROLLBACK_TTL_MS = 2 * 60 * 60 * 1000;
constexpr long long CEX_MIN_DELAY_BEFORE_CANCEL_MS = 1000;

// forward declare
class Engine;

struct AtomicSignal {
    std::atomic<float> prediction{0.0f};
    std::atomic<long long> timestamp_ms{0};

    void store(float pred, long long ts) {
        prediction.store(pred, std::memory_order_relaxed);
        timestamp_ms.store(ts, std::memory_order_release);
    }

    std::pair<float, long long> load() const {
        long long ts = timestamp_ms.load(std::memory_order_acquire);
        float pred = prediction.load(std::memory_order_relaxed);
        return {pred, ts};
    }
};

struct FillEvent {
    int outcome_index;  // 0 for single-outcome (crypto/stocks), 0/1 for binary
                        // PM
    double quantity;
    double price;
    OrderSide side;
    long long timestamp_ms;
};

class TradeTicker {
   public:
    TradeTicker(std::vector<ExchangeAndMarket> exchange_and_markets,
                std::string symbol, std::string quote_asset,
                std::string base_asset, int strategy_id);
    virtual ~TradeTicker() = default;

    AtomicSignal atomic_signal_main;
    AtomicSignal atomic_signal_short_horizon;

    std::atomic<std::chrono::steady_clock::time_point> last_received_update{
        std::chrono::steady_clock::now() - std::chrono::seconds(6000)};

    std::atomic<long long> last_stop_loss_triggered_time_ms{0};
    std::atomic<bool> is_signal_actionable{false};
    std::atomic<int> order_count{0};
    std::atomic<int> active_trade_count{0};
    std::atomic<int> unacked_enter_order_count{0};
    std::atomic<int> unacked_exit_order_count{0};
    std::string symbol;
    std::string quote_asset;
    std::string base_asset;
    int strategy_id;
    std::vector<Order> orders;
    std::vector<Order> orders_removed_from_state;
    std::vector<ExchangeAndMarket> exchange_and_markets;
    std::unordered_map<std::string, ExchangeAndMarket> exchange_market_map;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point>
        failed_create_order_times;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point>
        create_order_times;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point>
        next_allowed_order_timestamp;
    std::unordered_set<long long> processed_signal_timestamps;
    std::vector<Trade> active_trades;
    std::vector<Trade> recently_completed_polymarket_trades;

    std::vector<std::chrono::steady_clock::time_point> trade_completion_times;
    std::deque<std::chrono::steady_clock::time_point> recent_exit_order_times;
    std::deque<std::chrono::steady_clock::time_point> recent_enter_order_times;

    std::optional<Signal> prev_signal = std::nullopt;
    std::optional<Signal> curr_signal = std::nullopt;
    std::vector<Signal> signals;
    std::unordered_map<std::string, Signal> signals_by_model;
    std::unordered_map<double, int> bid_order_count_by_price;
    std::unordered_map<double, int> ask_order_count_by_price;
    std::unordered_map<std::string, int> bid_order_count_by_clob_price;
    std::unordered_map<std::string, int> ask_order_count_by_clob_price;
    int active_bid_count = 0;
    int active_ask_count = 0;
    double active_bid_notional = 0.0;
    double active_ask_notional = 0.0;

    // --- STRATEGY MODE FLAGS ---
    std::atomic<bool> is_trading_on_prediction_market{false};
    std::atomic<bool> is_market_making_strategy{false};
    // When set, find_active_trade_for_fill returns std::nullopt unconditionally
    // so every enter-fill creates a new Trade. For strategies that treat each
    // ladder rung fill as an independent position with its own open_price,
    // TP and FIFO slot; otherwise all rungs of one signal aggregate into one
    // Trade.
    std::atomic<bool> is_individual_fill_individual_trade{false};
    AccountMode account_mode{AccountMode::SHARED};

    // --- PREDICTION MARKET SPECIFIC START ---

    // Market state
    std::atomic<bool> is_active{false};
    std::atomic<bool> is_closed{false};
    std::atomic<bool> is_accepting_orders{false};

    // Order book config
    std::atomic<bool> is_order_book_enabled{false};
    std::atomic<double> order_price_min_tick_size{0.0};
    std::atomic<double> order_min_size{0.0};

    // Timing
    std::atomic<long long> event_start_time_ms{0};
    std::atomic<long long> event_end_time_ms{0};
    std::atomic<long long> accepting_orders_timestamp_ms{0};
    std::atomic<long long> last_mint_attempt_ms{0};

    // Identity
    std::string slug;
    std::string series_slug;
    std::string condition_id;
    double strike_price{0.0};
    // Canonical underlying symbol (e.g. "XRPUSDT") carried from the polymarket
    // series meta via populate_polymarket_fields_from_meta() — live and backtest
    // both populate it from the same markets-DB underlying_symbol column. Read
    // this downstream; never re-derive the symbol from series_slug. Empty =
    // unmapped series (do not price).
    std::string underlying_symbol;
    PolymarketProductType product_type{PolymarketProductType::UNKNOWN};

    // Risk
    std::atomic<bool> is_neg_risk{false};

    // Outcomes
    std::atomic<int> num_outcomes{0};

    std::vector<std::string> outcome_names_by_index;  // ["Up", "Down"]
    std::vector<std::string> clob_ids_by_index;  // ["9886735...", "7215285..."]

    // Reverse lookup
    std::unordered_map<std::string, int> clob_id_to_outcome_index;

    // Per-outcome best bid/ask from polymarket stream
    static constexpr int MAX_POLYMARKET_OUTCOMES = 10;
    struct OutcomeBBA {
        std::atomic<double> best_bid{0.0};
        std::atomic<double> best_ask{0.0};
        std::atomic<long long> timestamp_ms{0};
        std::atomic<long long> local_receive_time_ms{0};
        std::atomic<uint32_t> seq{0};
    };
    std::array<OutcomeBBA, MAX_POLYMARKET_OUTCOMES> outcome_bba;

    struct OutcomeBBASnapshot {
        double best_bid;
        double best_ask;
        long long timestamp_ms;
        long long local_receive_time_ms;
    };

    void update_outcome_best_bid_ask(int outcome_index, double best_bid,
                                     double best_ask, long long timestamp_ms);
    std::optional<OutcomeBBASnapshot> read_outcome_bba(int outcome_index) const;

    // Per-outcome exchange-reported inventory (atomic, no mutex needed)
    std::array<std::atomic<double>, MAX_POLYMARKET_OUTCOMES>
        outcome_exchange_inventory;

    void update_outcome_exchange_inventory(int outcome_index, double inventory);
    double get_outcome_exchange_inventory(int outcome_index) const;

    // Per-outcome trade flow from polymarket last_trade_price stream
    struct OutcomeTradeFlow {
        std::atomic<double> buy_volume{0.0};
        std::atomic<double> sell_volume{0.0};
        std::atomic<int> buy_count{0};
        std::atomic<int> sell_count{0};
        std::atomic<double> last_trade_price{0.0};
        std::atomic<long long> last_trade_ts{0};
    };
    std::array<OutcomeTradeFlow, MAX_POLYMARKET_OUTCOMES> outcome_trade_flow;

    struct OutcomeTradeFlowSnapshot {
        double buy_volume;
        double sell_volume;
        int buy_count;
        int sell_count;
        double last_trade_price;
        long long last_trade_ts;
    };

    void update_outcome_trade_flow(int outcome_index, bool is_buy, double size,
                                   double price, long long ts);
    [[nodiscard]] std::optional<OutcomeTradeFlowSnapshot>
    read_outcome_trade_flow(int outcome_index) const;

    // Recent fill tracking (30s window)
    RollingEventBuffer<FillEvent> recent_fills{30000};
    void record_fill(const OrderFillInfo& fill_info);

    // --- PREDICTION MARKET SPECIFIC END ---

    void configure_from_strategy(const StrategyConfig& strategy);
    void populate_polymarket_fields(const ::TradeTicker& db_ticker);
    void populate_polymarket_fields_from_meta(const PolymarketEventMeta& meta);
    bool is_prediction_market_active() const;
    bool is_polymarket_event_ongoing(long long current_time_ms) const;

    double get_net_polymarket_position_on_binary_market(
        const std::string& clob_id) const;

    void confirm_polymarket_fills(Engine& engine,
                                  const std::string& taker_order_id);
    void revert_polymarket_fills(Engine& engine,
                                 const std::string& taker_order_id);

    void add_synthethic_order_for_event_creation(const Order& order);
    std::vector<Order> get_orders(bool require_lock = true);
    std::vector<Trade> get_trades(bool require_lock = true);

    // Zero-copy accessors for callers that ALREADY hold trading_mutex (e.g.
    // strategy hot-paths invoked from on_market_event_callback, which
    // acquires the mutex before iterating). Returns a reference to the
    // internal vector — no allocation, no mpfr deep-copy. The reference is
    // valid only while the caller's lock is held AND the called code path
    // does not mutate the vector. Use the copy-returning get_trades() /
    // get_orders() if you don't already hold the lock or want snapshot
    // semantics.
    //
    // Per-event backtest cost during bursts was dominated by the deep
    // mpfr-copy inside get_trades(); switching strategy hot-paths to these
    // refs eliminates that.
    const std::vector<Trade>& get_active_trades_ref() const noexcept {
        return active_trades;
    }
    const std::vector<Order>& get_active_orders_ref() const noexcept {
        return orders;
    }
    std::vector<Order>& get_active_orders_mut_ref() noexcept { return orders; }
    int get_num_active_trades();
    int get_num_active_orders();
    int get_num_active_orders_no_lock();
    int get_num_active_orders_and_trades();

    static std::string get_order_cancel_key(const Order& order);
    bool is_cancel_already_sent(const std::string& cancel_key);
    bool is_cancel_already_sent(const Order& order);

    std::optional<long long> get_ms_since_failed_create_order(
        const std::string& exchange_name, const std::string& market_type,
        const std::string& symbol, long long account_id);

    long long get_cancel_retries(const std::string& client_order_id);

    long long get_last_sent_order_timestamp_ms(const std::string& exchange_name,
                                               const std::string& market_type,
                                               bool require_lock = true);
    std::optional<long long> get_time_ms_since_last_order_sent(
        long long current_time_ms, const std::string& exchange_name,
        const std::string& market_type, bool require_lock = true);

    void ack_signal_ready_for_trading(const Signal& signal);
    void recover_trade_from_disk(Engine& engine, const Trade& trade);
    void recover_order_from_disk(Engine& engine, const Order& order);
    void enter_trade_order_filled(Engine& engine, OrderFillInfo& fill_info);
    void exit_trade_order_filled(Engine& engine, OrderFillInfo& fill_info);
    void mm_trade_order_filled(Engine& engine, OrderFillInfo& fill_info);
    mpfr::mpreal get_trades_hedge_sizing_coeff_deltas(const Trade& trade);
    mpfr::mpreal get_directional_trades_sizing_coeff_delta(const Trade& trade);
    bool has_hedge_order_for_trade(const Trade& trade);
    bool has_directional_trade_for_hedge_trade(const Trade& trade);

    std::optional<Signal> get_last_actionable_signal();
    std::optional<Signal> get_last_signal();

    void on_order_filled_ack(Engine& engine, OrderFillInfo& fill_info);
    void add_signal(const Signal& signal, Strategy& strategy);
    void add_signal(const Signal& signal, Strategy& strategy,
                    size_t max_signals);
    void add_signal_by_model(const std::string& model_name,
                             const Signal& signal, Strategy& strategy);
    std::optional<Signal> get_signal_by_model(
        const std::string& model_name) const;
    void create_order_log(Engine& engine, const Order& order,
                          const std::string& event_type);
    void mark_last_stop_loss_triggered_time(Engine& engine);
    void sync_persistent_state_to_disk();

    long long get_time_ms_since_last_stop_loss_triggered(Engine& engine);

    mpfr::mpreal get_bids_notional_size();
    mpfr::mpreal get_asks_notional_size();

    std::optional<mpfr::mpreal> get_total_inventory_notional();

    mpfr::mpreal get_sizing_coeff_in_enter_orders();
    mpfr::mpreal get_sizing_coeff_in_exit_orders();
    // Caller must hold trading_mutex (same contract as get_active_orders_ref).
    mpfr::mpreal get_trade_exit_qty_on_book(const Trade& trade);
    mpfr::mpreal get_trade_remaining_qty(const Trade& trade);
    mpfr::mpreal get_max_available_qty_after_inventory_state(const Trade& trade,
                                                             Engine& engine);

    std::optional<std::chrono::steady_clock::time_point>
    get_last_trade_closed_timepoint();

    mpfr::mpreal get_sizing_coeff_deltas();
    mpfr::mpreal get_enter_order_sizing_coeff_deltas();

    std::optional<Signal> get_prediction();
    std::vector<Signal> get_predictions();

    std::optional<Order> get_order_by_id(const unsigned long long id);
    std::optional<std::reference_wrapper<Core::Trade>> get_trade_by_trade_id(
        const unsigned long long id);

    std::optional<std::reference_wrapper<Core::Trade>>
    get_trade_by_enter_order_id(const unsigned long long id);

    long long get_time_elapsed_since_event_start_ms() const;
    long long get_time_left_in_event_ms() const;

    std::optional<std::reference_wrapper<Core::Trade>> get_trade_by_signal_id(
        const unsigned long long signal_id, const std::string& exchange_name,
        const std::string& market_type, Core::TradeTypeInternal type_internal,
        const std::string& symbol);

    std::optional<std::reference_wrapper<Core::Trade>>
    find_active_trade_for_fill(const OrderFillInfo& fill_info,
                               const Order& order);

    void inventory_mismatch_remove_trade(Engine& engine, const Trade& trade);
    void inventory_mismatch_update_trade_size(Engine& engine,
                                              const Trade& trade,
                                              mpfr::mpreal new_quantity);
    bool manually_remove_trade_by_id(unsigned long long trade_id);
    void move_trade_to_completed(Core::Trade& trade, long long close_time_ms);
    void finalize_completed_trade(Engine& engine, Core::Trade& completed_trade,
                                  long long strategy_id);
    void force_close_trade(Engine& engine, unsigned long long trade_id,
                           ForceCloseReason reason,
                           const nlohmann::json& context = nlohmann::json::object());
    void on_order_cancel_success(Engine& engine, const Order& order,
                                 bool creation_failed = false,
                                 bool require_lock = true);
    void on_cancel_rm_from_state(Engine& engine, const Order& order,
                                 bool creation_failed = false);

    void on_create_order(Engine& engine, Order& order);
    void on_cancel_order(Engine& engine, Order& order);

    void erase_order_from_state(Engine& engine,
                                const std::vector<Order>::iterator& order_it);

    // Emit FILL_WITHOUT_MATCHING_ORDER when an exchange-side fill arrives for
    // an order_id that lives in neither active `orders` nor the recent
    // `orders_removed_from_state`. Rate-limited 10s per (exchange, symbol).
    // `phase` is "enter" | "exit" — tracks which lifecycle path dropped.
    void emit_fill_without_matching_order(Engine& engine,
                                          const OrderFillInfo& fill_info,
                                          const char* phase);

    void ack_create_order(Engine& engine, const Order& order);

    void mark_order_to_be_created(Engine& engine, Order& order,
                                  StrategyDecision& strategy_decision,
                                  Strategy& strategy);
    void mark_order_to_be_cancelled(Engine& engine, Order& order,
                                    StrategyDecision& strategy_decision);

    bool hedge_trade_or_order_exists_for_symbol(const Trade& directional_trade,
                                                const std::string& symbol);

    int get_num_enter_orders_by_signal_id(const unsigned long long signal_id,
                                          const std::string& exchange_name,
                                          const std::string& market_type);

    bool trade_has_order_on_book(unsigned long long trade_id);
    bool bid_order_exists_on_book(double price);
    bool ask_order_exists_on_book(double price);
    bool bid_order_exists_on_book(const std::string& clob_price_key);
    bool ask_order_exists_on_book(const std::string& clob_price_key);
    static std::string get_clob_price_key(const std::string& clob_id,
                                          const std::string& price_str);

    void set_trade_kv_cache(
        const Trade& trade,
        const std::unordered_map<std::string, double>& kv_cache);

    int get_num_enter_orders(const std::string& exchange_name,
                             const std::string& market_type);
    int get_num_enter_orders();

    void record_exit_order_sent();
    void record_enter_order_sent();
    void prune_recent_order_times();
    size_t purge_stale_removed_orders(long long now_ms, long long grace_ms);
    int get_recent_exit_orders_count(std::chrono::seconds window) const;
    int get_recent_exit_orders_count_10s() const;
    int get_recent_enter_orders_count(std::chrono::seconds window) const;
    int get_recent_enter_orders_count_10s() const;

    std::optional<long long> get_exchange_and_market_type_account_id(
        const std::string& exchange_name, const std::string& market_type);
    std::tuple<Core::OrderConstructionResult, std::optional<Core::Order>>
    construct_order(Engine& engine, Strategy& strategy,
                    Core::CreateOrderParams& params);

    void recalculate_order_book_state();
    void set_order_exchange_id(unsigned long long order_id,
                               const std::string& exchange_order_id,
                               bool is_res_success);
    bool is_cancel_ready_to_be_sent(const Order& order) const;

    double round_polymarket_price(double price, bool round_up) const;
    double round_polymarket_quantity(double quantity) const;
    void round_and_stringify_order(Engine& engine, Order& order) const;

    virtual void on_create_order_impl(Engine& engine, Order& order) = 0;
    virtual void on_cancel_order_impl(Engine& engine, Order& order) = 0;
    virtual void on_create_orders_bulk(Engine& engine,
                                       std::vector<Order*>& orders);
    virtual void on_cancel_orders_bulk(Engine& engine,
                                       std::vector<Order*>& orders);
    std::mutex trading_mutex;

   protected:
    std::unordered_map<
        std::string,
        std::pair<std::chrono::steady_clock::time_point, Core::Order>>
        unacked_new_cancels;

   private:
    template <typename F>
    auto with_lock(bool require_lock, F&& func) {
        if (!require_lock) {
            return func();
        }
        std::lock_guard<std::mutex> lock(this->trading_mutex);
        return func();
    }
};
}  // namespace Core
