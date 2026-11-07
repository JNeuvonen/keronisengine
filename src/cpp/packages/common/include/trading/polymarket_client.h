#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "crypto/eip712.h"
#include "market_making/dtypes/order.h"
#include "market_making/dtypes/def_only_structs.h"

namespace PolymarketDirect {

struct AccountCredentials {
    std::array<uint8_t, 32> private_key;
    std::string api_key;
    std::string api_secret;
    std::string api_passphrase;
    std::string maker_address;   // funder/maker (checksummed)
    std::string signer_address;  // signing EOA (checksummed)
    // 0 = EOA, 1 = POLY_PROXY, 2 = POLY_GNOSIS_SAFE. For EOA, maker == signer.
    int signature_type = 0;

    // Builder credentials (optional)
    std::string builder_api_key;
    std::string builder_secret;
    std::string builder_passphrase;

    bool has_builder_creds() const {
        return !builder_api_key.empty() && !builder_secret.empty() &&
               !builder_passphrase.empty();
    }
};

struct TokenOptions {
    std::string tick_size;  // "0.1", "0.01", "0.001", "0.0001"
    bool neg_risk;
    int fee_rate_bps;
};

struct RoundConfig {
    int price;
    int size;
    int amount;
};

class PolymarketClient {
   public:
    PolymarketClient();
    ~PolymarketClient();

    PolymarketClient(const PolymarketClient&) = delete;
    PolymarketClient& operator=(const PolymarketClient&) = delete;
    PolymarketClient(PolymarketClient&& other) noexcept;
    PolymarketClient& operator=(PolymarketClient&& other) noexcept;

    // Result of signing an order: the JSON body + deterministic order ID
    struct SignedOrderBody {
        nlohmann::ordered_json body;
        std::string order_id;  // EIP-712 hash hex (== exchange orderID)
    };

    // Assemble + sign a v2 order body from fully-resolved fields. Deterministic
    // given its inputs (does NOT generate salt/timestamp), so the serialized
    // payload can be parity-tested byte-for-byte against the Python CLOB
    // reference. build_signed_order_body fills the fields then calls this.
    static SignedOrderBody build_v2_order_body(
        const std::array<uint8_t, 32>& private_key, const std::string& api_key,
        const std::string& side_str, const EIP712::OrderFieldsV2& fields,
        bool neg_risk, const std::string& order_type, bool post_only);

    // Initialize with credentials for one or more accounts
    void add_account(long long account_id, const AccountCredentials& creds);

    // Create a single order, returns CLOB API response JSON
    std::optional<nlohmann::json> create_order(const Core::Order& order);

    // Create multiple orders in bulk, returns per-order results
    std::vector<Core::PolymarketBulkOrderResult> create_orders_bulk(
        const std::vector<Core::Order>& orders);

    // Sign orders locally without sending. Returns signed bodies with
    // deterministic order IDs derived from EIP-712 hashes.
    std::vector<SignedOrderBody> sign_orders(
        const std::vector<Core::Order>& orders);

    // Submit pre-signed order bodies to the CLOB API (bulk /orders endpoint).
    std::vector<Core::PolymarketBulkOrderResult> submit_signed_orders(
        const std::vector<SignedOrderBody>& signed_orders,
        long long account_id);

    // Submit a single pre-signed order to the CLOB API (singular /order endpoint).
    Core::PolymarketBulkOrderResult submit_signed_order(
        const SignedOrderBody& signed_order, long long account_id);

    // Cancel orders in bulk, returns per-order results
    std::vector<Core::PolymarketBulkCancelResult> cancel_orders(
        const std::vector<std::string>& order_ids, long long account_id);

    // Session heartbeat (POST /heartbeats): keeps the venue cancel-on-disconnect
    // timer from auto-cancelling this account's open orders. Returns true on 2xx.
    bool send_heartbeat(long long account_id);

    // Check if an account is configured
    bool has_account(long long account_id) const;

    // Pre-warm TCP+TLS connection to CLOB API (call after init)
    void warmup_connection();

    // Check if currently in 429 rate-limit cooldown
    bool is_rate_limited() const;

   private:

    // Per-account credentials
    std::unordered_map<long long, AccountCredentials> accounts_;
    mutable std::mutex accounts_mutex_;

    // Token options cache: token_id -> options
    std::unordered_map<std::string, TokenOptions> token_options_cache_;
    mutable std::mutex cache_mutex_;

    // Resolve token options (tick_size, neg_risk, fee_rate_bps)
    TokenOptions resolve_token_options(const std::string& token_id);

    // Get rounding config for a tick_size
    static RoundConfig get_round_config(const std::string& tick_size);

    // Amount calculation (port of py_clob_client/order_builder/builder.py)
    struct AmountResult {
        uint8_t side;  // 0=BUY, 1=SELL
        uint64_t maker_amount;
        uint64_t taker_amount;
    };
    static AmountResult calculate_order_amounts(const std::string& side,
                                                 double size, double price,
                                                 const RoundConfig& config);

    // Sign a single order and produce the signed order JSON body + order ID
    SignedOrderBody build_signed_order_body(
        const AccountCredentials& creds, const Core::Order& order,
        const TokenOptions& opts, const std::string& order_type,
        bool post_only);

    // HMAC-SHA256 auth (port of py_clob_client/signing/hmac.py)
    static std::string compute_hmac_signature(const std::string& secret,
                                               const std::string& timestamp,
                                               const std::string& method,
                                               const std::string& request_path,
                                               const std::string& body);

    // Build Level 2 auth headers
    std::unordered_map<std::string, std::string> build_l2_headers(
        const AccountCredentials& creds, const std::string& method,
        const std::string& request_path, const std::string& body,
        const std::string& timestamp);

    // Build builder auth headers (merged with L2)
    std::unordered_map<std::string, std::string> build_full_headers(
        const AccountCredentials& creds, const std::string& method,
        const std::string& request_path, const std::string& body);

    // HTTP POST to Polymarket CLOB API
    std::pair<int, std::string> http_post(
        const std::string& url, const std::string& body,
        const std::unordered_map<std::string, std::string>& headers);

    // HTTP GET from Polymarket CLOB API
    std::pair<int, std::string> http_get(const std::string& url);

    // HTTP DELETE with body to Polymarket CLOB API
    std::pair<int, std::string> http_delete(
        const std::string& url, const std::string& body,
        const std::unordered_map<std::string, std::string>& headers);

    // Rounding helpers
    static double round_normal(double x, int sig_digits);
    static double round_down(double x, int sig_digits);
    static double round_up(double x, int sig_digits);
    static int decimal_places(double x);
    static uint64_t to_token_decimals(double x);
};

}  // namespace PolymarketDirect
