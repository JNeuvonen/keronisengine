#pragma once

#include <array>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "market_making/dtypes/def_only_structs.h"
#include "market_making/dtypes/order.h"

namespace AsterDirect {

struct AccountCredentials {
    std::array<uint8_t, 32> private_key;  // API wallet (signer) private key
    std::string user_address;              // main wallet address (checksummed)
    std::string signer_address;            // API wallet address (checksummed)
};

namespace OrderType {
constexpr const char* LIMIT = "LIMIT";
constexpr const char* MARKET = "MARKET";
}  // namespace OrderType

namespace TimeInForce {
constexpr const char* GTC = "GTC";
constexpr const char* IOC = "IOC";
constexpr const char* FOK = "FOK";
constexpr const char* GTX = "GTX";  // post-only
}  // namespace TimeInForce

class AsterClient {
   public:
    AsterClient();
    ~AsterClient();

    AsterClient(const AsterClient&) = delete;
    AsterClient& operator=(const AsterClient&) = delete;
    AsterClient(AsterClient&& other) noexcept;
    AsterClient& operator=(AsterClient&& other) noexcept;

    void set_base_url(const std::string& url);
    void set_chain_id(int chain_id);

    void add_account(long long account_id, const AccountCredentials& creds);
    bool has_account(long long account_id) const;

    // ── Order creation ──────────────────────────────────────────────────

    std::optional<nlohmann::json> create_order(const Core::Order& order);

    std::vector<Core::PolymarketBulkOrderResult> create_orders_bulk(
        const std::vector<Core::Order>& orders);

    // ── Cancellation ────────────────────────────────────────────────────

    std::vector<Core::PolymarketBulkCancelResult> cancel_orders(
        const std::vector<std::string>& order_ids, long long account_id,
        const std::string& symbol);

    bool cancel_all_orders(long long account_id, const std::string& symbol);

    // ── Connection warmup ───────────────────────────────────────────────

    void warmup_connection();

    // ── Listen key (user data stream) ───────────────────────────────────

    std::string create_listen_key(long long account_id);
    void renew_listen_key(long long account_id, const std::string& listen_key);

    // ── EIP-712 (public for testing) ────────────────────────────────────

    static std::array<uint8_t, 32> compute_domain_separator(int chain_id);
    static const std::array<uint8_t, 32>& message_type_hash();

   private:
    std::string base_url_;
    int chain_id_ = 1666;  // mainnet default (714 for testnet)
    std::array<uint8_t, 32> domain_separator_ = {};
    bool domain_separator_initialized_ = false;

    mutable std::mutex accounts_mutex_;
    std::unordered_map<long long, AccountCredentials> accounts_;

    // Nonce: microsecond timestamps with per-second monotonic counter
    mutable std::mutex nonce_mutex_;
    long long last_nonce_sec_ = 0;
    long long nonce_counter_ = 0;

    std::optional<AccountCredentials> get_credentials(
        long long account_id) const;

    // ── Nonce ───────────────────────────────────────────────────────────

    long long next_nonce();

    // ── EIP-712 signing ─────────────────────────────────────────────────

    void ensure_domain_separator();

    // Sign a URL-encoded parameter string, returns hex signature (no 0x)
    std::string sign_params(const std::string& param_string,
                            const AccountCredentials& creds) const;

    // ── URL encoding ────────────────────────────────────────────────────

    static std::string url_encode(const std::string& value);
    static std::string build_query_string(
        const std::vector<std::pair<std::string, std::string>>& params);

    // ── Core::Order mapping ─────────────────────────────────────────────

    static const char* map_order_type(const Core::Order& order);
    static const char* map_time_in_force(const Core::Order& order);

    // ── HTTP helpers ────────────────────────────────────────────────────

    std::pair<int, std::string> http_signed_request(
        const std::string& method, const std::string& path,
        std::vector<std::pair<std::string, std::string>>& params,
        const AccountCredentials& creds);
};

}  // namespace AsterDirect
