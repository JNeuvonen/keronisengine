#pragma once

#include <array>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "market_making/dtypes/def_only_structs.h"
#include "market_making/dtypes/order.h"

namespace HyperliquidDirect {

struct AccountCredentials {
    std::array<uint8_t, 32> agent_private_key;  // API/agent wallet key
    std::string account_address;                // main wallet (the "user")
};

struct AssetInfo {
    uint32_t index;     // perp asset index in /info meta universe
    int sz_decimals;    // size decimals; perp price max decimals = 6 - sz_decimals
};

// Direct-signing client for Hyperliquid. All writes go to POST /exchange with a
// signature produced by the FFI signer (hyperliquid-signer-linux-amd64.so).
// Reuses Core::PolymarketBulk*Result for engine-side dispatch parity with Aster.
class HyperliquidClient {
   public:
    HyperliquidClient();
    ~HyperliquidClient();

    HyperliquidClient(const HyperliquidClient&) = delete;
    HyperliquidClient& operator=(const HyperliquidClient&) = delete;
    HyperliquidClient(HyperliquidClient&& other) noexcept;
    HyperliquidClient& operator=(HyperliquidClient&& other) noexcept;

    void set_base_url(const std::string& url);
    void set_mainnet(bool mainnet);

    void add_account(long long account_id, const AccountCredentials& creds);
    bool has_account(long long account_id) const;
    std::optional<std::string> account_address(long long account_id) const;

    // Fetches /info meta and builds the symbol→asset map. Returns false on
    // failure (leaves any prior map intact).
    bool load_meta();

    // ── Order creation ──────────────────────────────────────────────────
    std::optional<nlohmann::json> create_order(const Core::Order& order);
    std::vector<Core::PolymarketBulkOrderResult> create_orders_bulk(
        const std::vector<Core::Order>& orders);

    // ── Cancellation ────────────────────────────────────────────────────
    std::vector<Core::PolymarketBulkCancelResult> cancel_orders(
        const std::vector<std::string>& order_ids, long long account_id,
        const std::string& symbol);
    bool cancel_all_orders(long long account_id, const std::string& symbol);

    void warmup_connection();

    // ── Exposed for testing ─────────────────────────────────────────────
    std::optional<AssetInfo> lookup_asset(const std::string& symbol) const;
    static std::string format_size(double sz, int sz_decimals);
    static std::string format_price(double px, int sz_decimals);

   private:
    std::string base_url_;
    bool mainnet_ = true;

    mutable std::mutex accounts_mutex_;
    std::unordered_map<long long, AccountCredentials> accounts_;

    mutable std::mutex meta_mutex_;
    std::unordered_map<std::string, AssetInfo> asset_by_symbol_;

    mutable std::mutex nonce_mutex_;
    long long last_nonce_ms_ = 0;

    std::optional<AccountCredentials> get_credentials(long long account_id) const;
    long long next_nonce();

    static std::string pk_to_hex(const std::array<uint8_t, 32>& pk);
    static std::optional<std::string> valid_cloid(const std::string& s);

    std::pair<int, std::string> post_exchange(const std::string& action_json,
                                              long long nonce,
                                              const std::string& sig_r,
                                              const std::string& sig_s,
                                              int sig_v);
};

}  // namespace HyperliquidDirect
