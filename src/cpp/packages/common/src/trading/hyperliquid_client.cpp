#include "trading/hyperliquid_client.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>

#include "curl_multi_manager.h"
#include "rate_limiter.h"
#include "trading/hyperliquid_signer.h"

namespace HyperliquidDirect {

namespace {

// Reads an FFI HlSignedAction into owned strings and frees it.
struct SignOut {
    bool ok = false;
    std::string action_json;
    std::string r;
    std::string s;
    int v = 0;
    std::string err;
};

SignOut take(HlSignedAction resp) {
    SignOut out;
    if (resp.err != nullptr) {
        out.err = resp.err;
    } else {
        out.ok = true;
        out.action_json = resp.action_json ? resp.action_json : "";
        out.r = resp.sig_r ? resp.sig_r : "";
        out.s = resp.sig_s ? resp.sig_s : "";
        out.v = resp.sig_v;
    }
    HlFree(&resp);
    return out;
}

std::string normalize_decimal(double v, int max_decimals) {
    if (max_decimals < 0) max_decimals = 0;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", max_decimals, v);
    std::string s(buf);
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    return s;
}

}  // namespace

HyperliquidClient::HyperliquidClient() = default;
HyperliquidClient::~HyperliquidClient() = default;

HyperliquidClient::HyperliquidClient(HyperliquidClient&& other) noexcept
    : base_url_(std::move(other.base_url_)),
      mainnet_(other.mainnet_),
      accounts_(std::move(other.accounts_)),
      asset_by_symbol_(std::move(other.asset_by_symbol_)),
      last_nonce_ms_(other.last_nonce_ms_) {}

HyperliquidClient& HyperliquidClient::operator=(
    HyperliquidClient&& other) noexcept {
    if (this != &other) {
        base_url_ = std::move(other.base_url_);
        mainnet_ = other.mainnet_;
        accounts_ = std::move(other.accounts_);
        asset_by_symbol_ = std::move(other.asset_by_symbol_);
        last_nonce_ms_ = other.last_nonce_ms_;
    }
    return *this;
}

void HyperliquidClient::set_base_url(const std::string& url) { base_url_ = url; }
void HyperliquidClient::set_mainnet(bool mainnet) { mainnet_ = mainnet; }

void HyperliquidClient::add_account(long long account_id,
                                    const AccountCredentials& creds) {
    std::lock_guard<std::mutex> lock(accounts_mutex_);
    accounts_[account_id] = creds;
}

bool HyperliquidClient::has_account(long long account_id) const {
    std::lock_guard<std::mutex> lock(accounts_mutex_);
    return accounts_.count(account_id) > 0;
}

std::optional<std::string> HyperliquidClient::account_address(
    long long account_id) const {
    std::lock_guard<std::mutex> lock(accounts_mutex_);
    auto it = accounts_.find(account_id);
    if (it == accounts_.end()) return std::nullopt;
    return it->second.account_address;
}

std::optional<AccountCredentials> HyperliquidClient::get_credentials(
    long long account_id) const {
    std::lock_guard<std::mutex> lock(accounts_mutex_);
    auto it = accounts_.find(account_id);
    if (it == accounts_.end()) return std::nullopt;
    return it->second;
}

long long HyperliquidClient::next_nonce() {
    std::lock_guard<std::mutex> lock(nonce_mutex_);
    long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    if (now_ms <= last_nonce_ms_) now_ms = last_nonce_ms_ + 1;
    last_nonce_ms_ = now_ms;
    return now_ms;
}

std::string HyperliquidClient::pk_to_hex(const std::array<uint8_t, 32>& pk) {
    static const char hex_chars[] = "0123456789abcdef";
    std::string hex = "0x";
    hex.reserve(66);
    for (uint8_t b : pk) {
        hex += hex_chars[b >> 4];
        hex += hex_chars[b & 0x0f];
    }
    return hex;
}

std::optional<std::string> HyperliquidClient::valid_cloid(
    const std::string& s) {
    // HL cloid: 0x + 32 hex chars (128-bit).
    if (s.size() != 34 || s[0] != '0' || (s[1] != 'x' && s[1] != 'X'))
        return std::nullopt;
    for (size_t i = 2; i < s.size(); i++) {
        char c = s[i];
        bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                   (c >= 'A' && c <= 'F');
        if (!hex) return std::nullopt;
    }
    return s;
}

// ── Price / size formatting ────────────────────────────────────────────────

std::string HyperliquidClient::format_size(double sz, int sz_decimals) {
    return normalize_decimal(sz, sz_decimals);
}

std::string HyperliquidClient::format_price(double px, int sz_decimals) {
    // Perps: max decimals = 6 - szDecimals, AND <= 5 significant figures.
    // Integer prices are always allowed regardless of sig figs.
    int max_dec = 6 - sz_decimals;
    if (max_dec < 0) max_dec = 0;

    double a = std::fabs(px);
    int dec;
    if (a == 0.0) {
        dec = 0;
    } else {
        int e = static_cast<int>(std::floor(std::log10(a)));
        int sig_dec = 4 - e;  // decimals to keep 5 significant figures
        if (sig_dec < 0) sig_dec = 0;
        dec = std::min(max_dec, sig_dec);
    }
    return normalize_decimal(px, dec);
}

// ── Meta ───────────────────────────────────────────────────────────────────

bool HyperliquidClient::load_meta() {
    HttpRequest req;
    req.url = base_url_ + "/info";
    req.method = "POST";
    req.body = R"({"type":"meta"})";
    req.headers = {"Content-Type: application/json"};
    req.timeout_ms = 10000;

    auto [status, body] = CurlMultiManager::instance().execute(req);
    if (status < 200 || status >= 300 || body.empty()) {
        std::cerr << "[HL-API] load_meta failed: status=" << status
                  << " body=" << body.substr(0, 200) << std::endl;
        return false;
    }
    auto json = nlohmann::json::parse(body, nullptr, false);
    if (json.is_discarded() || !json.contains("universe") ||
        !json["universe"].is_array()) {
        std::cerr << "[HL-API] load_meta: bad meta response" << std::endl;
        return false;
    }

    std::unordered_map<std::string, AssetInfo> built;
    uint32_t idx = 0;
    for (const auto& item : json["universe"]) {
        AssetInfo info;
        info.index = idx++;
        info.sz_decimals = item.value("szDecimals", 0);
        built[item.value("name", "")] = info;
    }
    {
        std::lock_guard<std::mutex> lock(meta_mutex_);
        asset_by_symbol_ = std::move(built);
    }
    return true;
}

std::optional<AssetInfo> HyperliquidClient::lookup_asset(
    const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(meta_mutex_);
    auto it = asset_by_symbol_.find(symbol);
    if (it != asset_by_symbol_.end()) return it->second;
    // Fall back to stripping a common quote suffix (XRPUSDT -> XRP).
    static const char* suffixes[] = {"USDT", "USDC", "USD", "PERP"};
    for (const char* suf : suffixes) {
        size_t n = std::char_traits<char>::length(suf);
        if (symbol.size() > n &&
            symbol.compare(symbol.size() - n, n, suf) == 0) {
            auto jt = asset_by_symbol_.find(symbol.substr(0, symbol.size() - n));
            if (jt != asset_by_symbol_.end()) return jt->second;
        }
    }
    return std::nullopt;
}

// ── /exchange POST ───────────────────────────────────────────────────────

std::pair<int, std::string> HyperliquidClient::post_exchange(
    const std::string& action_json, long long nonce, const std::string& sig_r,
    const std::string& sig_s, int sig_v) {
    static const std::string RATE_KEY = "hyperliquid";

    nlohmann::json action = nlohmann::json::parse(action_json, nullptr, false);
    if (action.is_discarded()) return {0, "bad action json from signer"};

    nlohmann::json body;
    body["action"] = action;
    body["nonce"] = nonce;
    body["signature"] = {{"r", sig_r}, {"s", sig_s}, {"v", sig_v}};

    HttpRequest req;
    req.url = base_url_ + "/exchange";
    req.method = "POST";
    req.body = body.dump();
    req.headers = {"Content-Type: application/json"};
    req.timeout_ms = 10000;

    auto [status, resp] = CurlMultiManager::instance().execute(req);
    if (status == 429) RateLimiter::instance().record_limit(RATE_KEY, 120);
    return {status, resp};
}

// ── Order creation ─────────────────────────────────────────────────────────

std::optional<nlohmann::json> HyperliquidClient::create_order(
    const Core::Order& order) {
    auto results = create_orders_bulk({order});
    if (results.empty()) return std::nullopt;
    nlohmann::json j;
    j["success"] = results[0].success;
    j["exchange_order_id"] = results[0].order_id;
    j["error_msg"] = results[0].error_msg;
    return j;
}

std::vector<Core::PolymarketBulkOrderResult>
HyperliquidClient::create_orders_bulk(const std::vector<Core::Order>& orders) {
    std::vector<Core::PolymarketBulkOrderResult> results(orders.size());
    if (orders.empty()) return results;

    auto creds_opt = get_credentials(orders[0].account_id);
    if (!creds_opt.has_value()) {
        for (auto& r : results) r.error_msg = "Account not found";
        return results;
    }
    const auto& creds = creds_opt.value();

    // Resolve assets + format prices/sizes. Unresolved symbols fail in place;
    // the remainder is signed and submitted as a single action.
    std::vector<std::string> price_strs, size_strs, tif_strs, cloid_strs;
    std::vector<size_t> submit_idx;
    price_strs.reserve(orders.size());
    size_strs.reserve(orders.size());
    tif_strs.reserve(orders.size());
    cloid_strs.reserve(orders.size());

    for (size_t i = 0; i < orders.size(); i++) {
        const auto& o = orders[i];
        auto asset = lookup_asset(o.symbol);
        if (!asset.has_value()) {
            results[i].error_msg = "unknown HL asset for symbol " + o.symbol;
            continue;
        }
        price_strs.push_back(
            format_price(o.price.toDouble(), asset->sz_decimals));
        size_strs.push_back(
            format_size(o.quantity.toDouble(), asset->sz_decimals));
        tif_strs.push_back(o.post_only ? "Alo"
                           : o.type == Core::OrderType::MARKET ? "Ioc"
                                                               : "Gtc");
        auto cl = valid_cloid(o.client_order_id);
        cloid_strs.push_back(cl.has_value() ? cl.value() : "");
        submit_idx.push_back(i);
    }

    if (submit_idx.empty()) return results;

    std::vector<HlOrderSpec> specs;
    specs.reserve(submit_idx.size());
    for (size_t k = 0; k < submit_idx.size(); k++) {
        const auto& o = orders[submit_idx[k]];
        auto asset = lookup_asset(o.symbol);
        HlOrderSpec spec;
        spec.asset = asset->index;
        spec.is_buy = o.side == Core::OrderSide::BUY ? 1 : 0;
        spec.price = price_strs[k].c_str();
        spec.size = size_strs[k].c_str();
        spec.reduce_only = o.reduce_only ? 1 : 0;
        spec.tif = tif_strs[k].c_str();
        spec.cloid = cloid_strs[k].c_str();
        specs.push_back(spec);
    }

    long long nonce = next_nonce();
    std::string pk = pk_to_hex(creds.agent_private_key);
    SignOut signed_action =
        take(SignOrderAction(pk.c_str(), mainnet_ ? 1 : 0, nonce, "",
                             specs.data(), static_cast<int>(specs.size())));
    if (!signed_action.ok) {
        for (auto idx : submit_idx)
            results[idx].error_msg = "sign failed: " + signed_action.err;
        return results;
    }

    auto [status, body] = post_exchange(signed_action.action_json, nonce,
                                        signed_action.r, signed_action.s,
                                        signed_action.v);

    if (status < 200 || status >= 300 || body.empty()) {
        std::string msg = "HTTP " + std::to_string(status) + ": " +
                          body.substr(0, 200);
        for (auto idx : submit_idx) results[idx].error_msg = msg;
        return results;
    }

    auto json = nlohmann::json::parse(body, nullptr, false);
    if (json.is_discarded() || json.value("status", "") != "ok") {
        std::string msg = "exchange error: " + body.substr(0, 200);
        std::cout << "[HL-API] order action error: " << msg << std::endl;
        for (auto idx : submit_idx) results[idx].error_msg = msg;
        return results;
    }

    const auto& statuses = json["response"]["data"]["statuses"];
    for (size_t k = 0; k < submit_idx.size(); k++) {
        size_t idx = submit_idx[k];
        if (k >= statuses.size()) {
            results[idx].error_msg = "missing status in HL order response";
            continue;
        }
        const auto& st = statuses[k];
        if (st.contains("resting")) {
            results[idx].success = true;
            results[idx].order_id =
                std::to_string(st["resting"].value("oid", 0LL));
        } else if (st.contains("filled")) {
            results[idx].success = true;
            results[idx].order_id =
                std::to_string(st["filled"].value("oid", 0LL));
        } else if (st.contains("error")) {
            results[idx].error_msg = st.value("error", "order error");
            std::cout << "[HL-API] order rejected: " << results[idx].error_msg
                      << std::endl;
        } else {
            results[idx].error_msg = "unrecognized HL order status";
        }
    }
    return results;
}

// ── Cancellation ───────────────────────────────────────────────────────────

std::vector<Core::PolymarketBulkCancelResult> HyperliquidClient::cancel_orders(
    const std::vector<std::string>& order_ids, long long account_id,
    const std::string& symbol) {
    std::vector<Core::PolymarketBulkCancelResult> results(order_ids.size());
    for (size_t i = 0; i < order_ids.size(); i++)
        results[i].order_id = order_ids[i];
    if (order_ids.empty()) return results;

    auto creds_opt = get_credentials(account_id);
    if (!creds_opt.has_value()) {
        for (auto& r : results) r.error_msg = "Account not found";
        return results;
    }
    auto asset = lookup_asset(symbol);
    if (!asset.has_value()) {
        for (auto& r : results) r.error_msg = "unknown HL asset for " + symbol;
        return results;
    }

    std::vector<HlCancelSpec> specs;
    std::vector<size_t> submit_idx;
    specs.reserve(order_ids.size());
    for (size_t i = 0; i < order_ids.size(); i++) {
        try {
            HlCancelSpec spec;
            spec.asset = asset->index;
            spec.oid = std::stoull(order_ids[i]);
            specs.push_back(spec);
            submit_idx.push_back(i);
        } catch (const std::exception&) {
            results[i].error_msg = "invalid order_id format: " + order_ids[i];
        }
    }
    if (submit_idx.empty()) return results;

    long long nonce = next_nonce();
    std::string pk = pk_to_hex(creds_opt->agent_private_key);
    SignOut signed_action =
        take(SignCancelAction(pk.c_str(), mainnet_ ? 1 : 0, nonce, "",
                             specs.data(), static_cast<int>(specs.size())));
    if (!signed_action.ok) {
        for (auto idx : submit_idx)
            results[idx].error_msg = "sign failed: " + signed_action.err;
        return results;
    }

    auto [status, body] = post_exchange(signed_action.action_json, nonce,
                                        signed_action.r, signed_action.s,
                                        signed_action.v);

    if (status < 200 || status >= 300 || body.empty()) {
        std::string msg = "HTTP " + std::to_string(status) + ": " +
                          body.substr(0, 200);
        for (auto idx : submit_idx) results[idx].error_msg = msg;
        return results;
    }

    auto json = nlohmann::json::parse(body, nullptr, false);
    if (json.is_discarded() || json.value("status", "") != "ok") {
        std::string msg = "exchange error: " + body.substr(0, 200);
        for (auto idx : submit_idx) results[idx].error_msg = msg;
        return results;
    }

    const auto& statuses = json["response"]["data"]["statuses"];
    for (size_t k = 0; k < submit_idx.size(); k++) {
        size_t idx = submit_idx[k];
        if (k >= statuses.size()) {
            results[idx].error_msg = "missing status in HL cancel response";
            continue;
        }
        const auto& st = statuses[k];
        if (st.is_string() && st.get<std::string>() == "success") {
            results[idx].success = true;
        } else if (st.is_object() && st.contains("error")) {
            results[idx].error_msg = st.value("error", "cancel error");
        } else {
            results[idx].error_msg = "unrecognized HL cancel status";
        }
    }
    return results;
}

bool HyperliquidClient::cancel_all_orders(long long account_id,
                                          const std::string& symbol) {
    // HL has no single cancel-all-for-symbol primitive without enumerating
    // open orders; engine reconcile drives explicit cancels. Not implemented.
    (void)account_id;
    (void)symbol;
    return false;
}

void HyperliquidClient::warmup_connection() {
    if (!base_url_.empty()) {
        CurlMultiManager::instance().warmup(base_url_ + "/info");
    }
}

}  // namespace HyperliquidDirect
