#pragma once

#include <string>

namespace Core {
enum OrderType {
    LIMIT,
    MARKET,
    LIMIT_MAKER,
    FILL_OR_KILL,
    IMMEDIATE_OR_CANCEL
};
enum OrderStatus { WORKING, FILLED, CANCELLED };
enum OrderSide { BUY, SELL };
enum OrderTypeInternal {
    DEFAULT = 0,
    PASSIVE_TAKE_PROFIT = 1,
    NONTRACKED_INVENTORY_CLOSE = 2,
    STARTUP_HEALTH_CHECK = 3,
    SYNTHETIC_CONTRACT_MINT = 4,
    SYNTHETIC_CONTRACT_SETTLEMENT = 5,
    DISCRETIONARY_ORDER = 6,
};

enum TradeType { LONG, SHORT };
enum MarketEventType {
    ORDER_BOOK_UPDATE,
    AGGR_TRADE,
    TRADE,
    SIZING_COEFF_UPDATE,
    PREDICTION_UPDATE,
    PRICE_UPDATE,
    BULK_PRICE_UPDATE,
    OHLCV_UPDATE,
    FUNDING_PAYMENT,
    CUSTOM,
    POLYMARKET_TRADE,
    POLYMARKET_MARKET_CREATED,
    POLYMARKET_MARKET_CLOSED,
    MODEL_UPDATE,
};
enum OrderCancelReason {
    EXPIRED,
    ALFA_SIGNAL_OVERRIDE,
};

enum MarketDataSubscriptionType {
    SUBSCRIBE,
    UNSUBSCRIBE,
};

enum MarketDataStreamType {
    TRADE_STREAM,
    ORDER_BOOK_STREAM,
};

enum PositionSide {
    LONG_SIDE,
    SHORT_SIDE,
};

enum OrderConstructionResult {
    VALID,
    STRATEGY_EXIT_RATE_LIMITED,
    TICKER_EXIT_RATE_LIMITED,
    STRATEGY_ENTER_RATE_LIMITED,
    TICKER_ENTER_RATE_LIMITED,
    FAILED_ORDER_COOLDOWN_EXIT,
    FAILED_ORDER_COOLDOWN_ENTER,
    INVALID_SIZING_COEFF_DELTA,
    MISSING_ORDERBOOK_PRICES,
    OVERLAPPING_BID_ORDER_EXISTS,
    OVERLAPPING_ASK_ORDER_EXISTS,
    MAX_NOTIONAL_RESTING_ON_BOOK_EXCEEDED,
    MAX_UNACKED_EXIT_ORDERS_EXCEEDED,
    MAX_UNACKED_ENTER_ORDERS_EXCEEDED,
    POST_ONLY_VALIDATION_FAILED_SELL,
    POST_ONLY_VALIDATION_FAILED_BUY,
    MAXIMUM_SLIPPAGE_EXCEEDED_BUY,
    MAXIMUM_SLIPPAGE_EXCEEDED_SELL,
    PRICE_WITHIN_THRESHOLD_EXCEEDED_BUY,
    PRICE_WITHIN_THRESHOLD_EXCEEDED_SELL,
    MISSING_INVENTORY_OR_BORROWABLE,
    RANDOMIZED_DELAY_BETWEEN_ORDERS_ACTIVE,
    MISSING_ACCOUNT_ID_TICKER,
    MISSING_ACCOUNT_ID_RISK_ENGINE,
    STRATEGY_IN_REDUCE_ONLY_MODE,
    ORDER_NOT_ALLOWED_TO_PASS_RISK_ENGINE,
    MIN_NOTIONAL_FILTER_FAILED,
    ENGINE_CONSTRUCT_ORDER_FAILED,
    STRATEGY_STOP_MAKING_MARKETS,
    STRATEGY_QUOTES_PULLED
};

inline const char *order_construction_result_to_str(OrderConstructionResult r) {
    switch (r) {
        case VALID: return "VALID";
        case STRATEGY_EXIT_RATE_LIMITED: return "STRATEGY_EXIT_RATE_LIMITED";
        case TICKER_EXIT_RATE_LIMITED: return "TICKER_EXIT_RATE_LIMITED";
        case STRATEGY_ENTER_RATE_LIMITED: return "STRATEGY_ENTER_RATE_LIMITED";
        case TICKER_ENTER_RATE_LIMITED: return "TICKER_ENTER_RATE_LIMITED";
        case FAILED_ORDER_COOLDOWN_EXIT: return "FAILED_ORDER_COOLDOWN_EXIT";
        case FAILED_ORDER_COOLDOWN_ENTER: return "FAILED_ORDER_COOLDOWN_ENTER";
        case INVALID_SIZING_COEFF_DELTA: return "INVALID_SIZING_COEFF_DELTA";
        case MISSING_ORDERBOOK_PRICES: return "MISSING_ORDERBOOK_PRICES";
        case OVERLAPPING_BID_ORDER_EXISTS: return "OVERLAPPING_BID_ORDER_EXISTS";
        case OVERLAPPING_ASK_ORDER_EXISTS: return "OVERLAPPING_ASK_ORDER_EXISTS";
        case MAX_NOTIONAL_RESTING_ON_BOOK_EXCEEDED:
            return "MAX_NOTIONAL_RESTING_ON_BOOK_EXCEEDED";
        case MAX_UNACKED_EXIT_ORDERS_EXCEEDED:
            return "MAX_UNACKED_EXIT_ORDERS_EXCEEDED";
        case MAX_UNACKED_ENTER_ORDERS_EXCEEDED:
            return "MAX_UNACKED_ENTER_ORDERS_EXCEEDED";
        case POST_ONLY_VALIDATION_FAILED_SELL:
            return "POST_ONLY_VALIDATION_FAILED_SELL";
        case POST_ONLY_VALIDATION_FAILED_BUY:
            return "POST_ONLY_VALIDATION_FAILED_BUY";
        case MAXIMUM_SLIPPAGE_EXCEEDED_BUY: return "MAXIMUM_SLIPPAGE_EXCEEDED_BUY";
        case MAXIMUM_SLIPPAGE_EXCEEDED_SELL:
            return "MAXIMUM_SLIPPAGE_EXCEEDED_SELL";
        case PRICE_WITHIN_THRESHOLD_EXCEEDED_BUY:
            return "PRICE_WITHIN_THRESHOLD_EXCEEDED_BUY";
        case PRICE_WITHIN_THRESHOLD_EXCEEDED_SELL:
            return "PRICE_WITHIN_THRESHOLD_EXCEEDED_SELL";
        case MISSING_INVENTORY_OR_BORROWABLE:
            return "MISSING_INVENTORY_OR_BORROWABLE";
        case RANDOMIZED_DELAY_BETWEEN_ORDERS_ACTIVE:
            return "RANDOMIZED_DELAY_BETWEEN_ORDERS_ACTIVE";
        case MISSING_ACCOUNT_ID_TICKER: return "MISSING_ACCOUNT_ID_TICKER";
        case MISSING_ACCOUNT_ID_RISK_ENGINE:
            return "MISSING_ACCOUNT_ID_RISK_ENGINE";
        case STRATEGY_IN_REDUCE_ONLY_MODE: return "STRATEGY_IN_REDUCE_ONLY_MODE";
        case ORDER_NOT_ALLOWED_TO_PASS_RISK_ENGINE:
            return "ORDER_NOT_ALLOWED_TO_PASS_RISK_ENGINE";
        case MIN_NOTIONAL_FILTER_FAILED: return "MIN_NOTIONAL_FILTER_FAILED";
        case ENGINE_CONSTRUCT_ORDER_FAILED:
            return "ENGINE_CONSTRUCT_ORDER_FAILED";
        case STRATEGY_STOP_MAKING_MARKETS: return "STRATEGY_STOP_MAKING_MARKETS";
        case STRATEGY_QUOTES_PULLED: return "STRATEGY_QUOTES_PULLED";
    }
    return "UNKNOWN";
}

enum TradeCreationType {
    ORGANIC = 0,
    MINT = 1,
    DISCRETIONARY = 2,
};

inline std::string trade_creation_type_to_str(TradeCreationType t) {
    switch (t) {
        case TradeCreationType::MINT:
            return "mint";
        case TradeCreationType::DISCRETIONARY:
            return "discretionary";
        default:
            return "organic";
    }
}

inline TradeCreationType trade_creation_type_from_str(const std::string &str) {
    if (str == "mint") return TradeCreationType::MINT;
    if (str == "discretionary") return TradeCreationType::DISCRETIONARY;
    return TradeCreationType::ORGANIC;
}

inline TradeCreationType trade_creation_type_for(OrderTypeInternal type) {
    switch (type) {
        case OrderTypeInternal::SYNTHETIC_CONTRACT_MINT:
            return TradeCreationType::MINT;
        case OrderTypeInternal::DISCRETIONARY_ORDER:
            return TradeCreationType::DISCRETIONARY;
        default:
            return TradeCreationType::ORGANIC;
    }
}

enum class ForceCloseReason {
    REMAINING_NOTIONAL_BELOW_MIN,
};

inline const char *force_close_reason_to_str(ForceCloseReason reason) {
    switch (reason) {
        case ForceCloseReason::REMAINING_NOTIONAL_BELOW_MIN:
            return "REMAINING_NOTIONAL_BELOW_MIN";
    }
    return "UNKNOWN";
}

inline std::string order_type_internal_to_str(OrderTypeInternal type_internal) {
    switch (type_internal) {
        case OrderTypeInternal::DEFAULT:
            return "default";
        case OrderTypeInternal::PASSIVE_TAKE_PROFIT:
            return "passive_take_profit";
        case OrderTypeInternal::NONTRACKED_INVENTORY_CLOSE:
            return "nontracked_inventory_close";
        case OrderTypeInternal::STARTUP_HEALTH_CHECK:
            return "startup_health_check";
        case OrderTypeInternal::SYNTHETIC_CONTRACT_MINT:
            return "synthetic_contract_mint";
        case OrderTypeInternal::SYNTHETIC_CONTRACT_SETTLEMENT:
            return "synthetic_contract_settlement";
        case OrderTypeInternal::DISCRETIONARY_ORDER:
            return "discretionary";
        default:
            return "default";
    }
}

inline OrderTypeInternal order_type_internal_from_str(const std::string &str) {
    if (str == "default") return OrderTypeInternal::DEFAULT;
    if (str == "passive_take_profit")
        return OrderTypeInternal::PASSIVE_TAKE_PROFIT;
    if (str == "nontracked_inventory_close")
        return OrderTypeInternal::NONTRACKED_INVENTORY_CLOSE;
    if (str == "startup_health_check")
        return OrderTypeInternal::STARTUP_HEALTH_CHECK;
    if (str == "synthetic_contract_mint")
        return OrderTypeInternal::SYNTHETIC_CONTRACT_MINT;
    if (str == "synthetic_contract_settlement")
        return OrderTypeInternal::SYNTHETIC_CONTRACT_SETTLEMENT;
    if (str == "discretionary") return OrderTypeInternal::DISCRETIONARY_ORDER;
    return OrderTypeInternal::DEFAULT;
}

// ── Exchange and Market enums for fast routing ──────────────────────────

enum class Exchange {
    BINANCE,
    BYBIT,
    HYPERLIQUID,
    BITGET,
    GATEIO,
    POLYMARKET,
    ASTER,
    POLYGON,
    IBKR,
    LIGHTER,
    UNKNOWN,
};

enum class Market {
    SPOT,
    SWAP,
    UNKNOWN,
};

inline Exchange exchange_from_str(const std::string& s) {
    if (s == "binance") return Exchange::BINANCE;
    if (s == "bybit") return Exchange::BYBIT;
    if (s == "hyperliquid") return Exchange::HYPERLIQUID;
    if (s == "bitget") return Exchange::BITGET;
    if (s == "gateio") return Exchange::GATEIO;
    if (s == "polymarket") return Exchange::POLYMARKET;
    if (s == "aster") return Exchange::ASTER;
    if (s == "polygon") return Exchange::POLYGON;
    if (s == "ibkr") return Exchange::IBKR;
    if (s == "lighter") return Exchange::LIGHTER;
    return Exchange::UNKNOWN;
}

inline const std::string& exchange_to_str(Exchange e) {
    static const std::string binance = "binance";
    static const std::string bybit = "bybit";
    static const std::string hyperliquid = "hyperliquid";
    static const std::string bitget = "bitget";
    static const std::string gateio = "gateio";
    static const std::string polymarket = "polymarket";
    static const std::string aster = "aster";
    static const std::string polygon = "polygon";
    static const std::string ibkr = "ibkr";
    static const std::string lighter = "lighter";
    static const std::string unknown = "";
    switch (e) {
        case Exchange::BINANCE: return binance;
        case Exchange::BYBIT: return bybit;
        case Exchange::HYPERLIQUID: return hyperliquid;
        case Exchange::BITGET: return bitget;
        case Exchange::GATEIO: return gateio;
        case Exchange::POLYMARKET: return polymarket;
        case Exchange::ASTER: return aster;
        case Exchange::POLYGON: return polygon;
        case Exchange::IBKR: return ibkr;
        case Exchange::LIGHTER: return lighter;
        default: return unknown;
    }
}

inline Market market_from_str(const std::string& s) {
    if (s == "spot") return Market::SPOT;
    if (s == "swap") return Market::SWAP;
    return Market::UNKNOWN;
}

inline const std::string& market_to_str(Market m) {
    static const std::string spot = "spot";
    static const std::string swap = "swap";
    static const std::string unknown = "";
    switch (m) {
        case Market::SPOT: return spot;
        case Market::SWAP: return swap;
        default: return unknown;
    }
}

// ── Account mode ─────────────────────────────────────────────────────────

enum class AccountMode {
    SHARED,
    ISOLATED,
};

inline AccountMode account_mode_from_str(const std::string& s) {
    if (s == "isolated") return AccountMode::ISOLATED;
    return AccountMode::SHARED;
}

// ── Exchange capabilities ────────────────────────────────────────────────

inline bool exchange_supports_global_open_orders(Exchange e) {
    switch (e) {
        case Exchange::ASTER: return true;
        case Exchange::HYPERLIQUID: return true;
        default: return false;
    }
}

}  // namespace Core