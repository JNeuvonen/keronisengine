#include "trading/aster_client.h"

#include <iostream>
#include <chrono>
#include <cstdlib>

#include "crypto/keccak256.h"
#include "crypto/secp256k1_signer.h"
#include "curl_multi_manager.h"
#include "rate_limiter.h"

namespace AsterDirect {

// ── Construction / move ──────────────────────────────────────────────────

AsterClient::AsterClient() = default;
AsterClient::~AsterClient() = default;

AsterClient::AsterClient(AsterClient&& other) noexcept
    : base_url_(std::move(other.base_url_)),
      accounts_(std::move(other.accounts_)),
      last_nonce_sec_(other.last_nonce_sec_),
      nonce_counter_(other.nonce_counter_),
      chain_id_(other.chain_id_),
      domain_separator_(other.domain_separator_),
      domain_separator_initialized_(other.domain_separator_initialized_) {}

AsterClient& AsterClient::operator=(AsterClient&& other) noexcept {
    if (this != &other) {
        base_url_ = std::move(other.base_url_);
        accounts_ = std::move(other.accounts_);
        last_nonce_sec_ = other.last_nonce_sec_;
        nonce_counter_ = other.nonce_counter_;
        chain_id_ = other.chain_id_;
        domain_separator_ = other.domain_separator_;
        domain_separator_initialized_ = other.domain_separator_initialized_;
    }
    return *this;
}

// ── Configuration ────────────────────────────────────────────────────────

void AsterClient::set_base_url(const std::string& url) { base_url_ = url; }

void AsterClient::set_chain_id(int chain_id) {
    chain_id_ = chain_id;
    domain_separator_ = compute_domain_separator(chain_id_);
    domain_separator_initialized_ = true;
}

// ── Account management ───────────────────────────────────────────────────

void AsterClient::add_account(long long account_id,
                              const AccountCredentials& creds) {
    std::lock_guard<std::mutex> lock(accounts_mutex_);
    accounts_[account_id] = creds;
}

bool AsterClient::has_account(long long account_id) const {
    std::lock_guard<std::mutex> lock(accounts_mutex_);
    return accounts_.count(account_id) > 0;
}

std::optional<AccountCredentials> AsterClient::get_credentials(
    long long account_id) const {
    std::lock_guard<std::mutex> lock(accounts_mutex_);
    auto it = accounts_.find(account_id);
    if (it == accounts_.end()) return std::nullopt;
    return it->second;
}

// ── Nonce ────────────────────────────────────────────────────────────────

long long AsterClient::next_nonce() {
    std::lock_guard<std::mutex> lock(nonce_mutex_);
    auto now_sec =
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    if (now_sec <= last_nonce_sec_) {
        nonce_counter_++;
    } else {
        last_nonce_sec_ = now_sec;
        nonce_counter_ = 0;
    }
    return last_nonce_sec_ * 1000000LL + nonce_counter_;
}

// ── EIP-712 signing ──────────────────────────────────────────────────────

// Domain: AsterSignTransaction, version 1, chainId variable, verifyingContract 0x0
std::array<uint8_t, 32> AsterClient::compute_domain_separator(int chain_id) {
    auto domain_type_hash = Keccak::keccak256(
        "EIP712Domain(string name,string version,uint256 chainId,"
        "address verifyingContract)");
    auto name_hash = Keccak::keccak256("AsterSignTransaction");
    auto version_hash = Keccak::keccak256("1");

    // Encode chain_id as uint256 big-endian
    std::array<uint8_t, 32> chain_id_bytes = {};
    chain_id_bytes[28] = static_cast<uint8_t>((chain_id >> 24) & 0xff);
    chain_id_bytes[29] = static_cast<uint8_t>((chain_id >> 16) & 0xff);
    chain_id_bytes[30] = static_cast<uint8_t>((chain_id >> 8) & 0xff);
    chain_id_bytes[31] = static_cast<uint8_t>(chain_id & 0xff);

    // verifyingContract = address(0)
    std::array<uint8_t, 32> contract = {};

    std::vector<uint8_t> encoded;
    encoded.reserve(160);
    encoded.insert(encoded.end(), domain_type_hash.begin(),
                   domain_type_hash.end());
    encoded.insert(encoded.end(), name_hash.begin(), name_hash.end());
    encoded.insert(encoded.end(), version_hash.begin(),
                   version_hash.end());
    encoded.insert(encoded.end(), chain_id_bytes.begin(),
                   chain_id_bytes.end());
    encoded.insert(encoded.end(), contract.begin(), contract.end());

    return Keccak::keccak256(encoded);
}

void AsterClient::ensure_domain_separator() {
    if (!domain_separator_initialized_) {
        domain_separator_ = compute_domain_separator(chain_id_);
        domain_separator_initialized_ = true;
    }
}

// keccak256("Message(string msg)")
const std::array<uint8_t, 32>& AsterClient::message_type_hash() {
    static const auto hash = Keccak::keccak256("Message(string msg)");
    return hash;
}

std::string AsterClient::sign_params(const std::string& param_string,
                                     const AccountCredentials& creds) const {
    // hashStruct = keccak256(MESSAGE_TYPE_HASH || keccak256(param_string))
    auto msg_hash = Keccak::keccak256(param_string);

    std::vector<uint8_t> struct_data;
    struct_data.reserve(64);
    const auto& type_hash = message_type_hash();
    struct_data.insert(struct_data.end(), type_hash.begin(), type_hash.end());
    struct_data.insert(struct_data.end(), msg_hash.begin(), msg_hash.end());
    auto struct_hash = Keccak::keccak256(struct_data);

    // digest = keccak256(0x19 0x01 || domainSeparator || structHash)
    std::vector<uint8_t> message;
    message.reserve(66);
    message.push_back(0x19);
    message.push_back(0x01);
    message.insert(message.end(), domain_separator_.begin(),
                   domain_separator_.end());
    message.insert(message.end(), struct_hash.begin(), struct_hash.end());
    auto digest = Keccak::keccak256(message);

    // ECDSA sign and return hex (no 0x prefix)
    auto sig = Secp256k1Signer::sign_hash(digest.data(), creds.private_key.data());

    std::string hex;
    hex.reserve(130);
    static const char hex_chars[] = "0123456789abcdef";
    for (uint8_t byte : sig) {
        hex += hex_chars[byte >> 4];
        hex += hex_chars[byte & 0x0f];
    }
    return hex;
}

// ── URL encoding ─────────────────────────────────────────────────────────

std::string AsterClient::url_encode(const std::string& value) {
    std::string encoded;
    encoded.reserve(value.size() * 1.2);
    static const char hex_chars[] = "0123456789ABCDEF";
    for (unsigned char c : value) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~') {
            encoded += static_cast<char>(c);
        } else {
            encoded += '%';
            encoded += hex_chars[c >> 4];
            encoded += hex_chars[c & 0x0f];
        }
    }
    return encoded;
}

std::string AsterClient::build_query_string(
    const std::vector<std::pair<std::string, std::string>>& params) {
    std::string qs;
    for (size_t i = 0; i < params.size(); i++) {
        if (i > 0) qs += '&';
        qs += url_encode(params[i].first);
        qs += '=';
        qs += url_encode(params[i].second);
    }
    return qs;
}

// ── Core::Order mapping ──────────────────────────────────────────────────

const char* AsterClient::map_order_type(const Core::Order& order) {
    if (order.type == Core::OrderType::MARKET) return OrderType::MARKET;
    return OrderType::LIMIT;
}

const char* AsterClient::map_time_in_force(const Core::Order& order) {
    if (order.post_only) return TimeInForce::GTX;
    if (order.type == Core::OrderType::MARKET) return TimeInForce::IOC;
    return TimeInForce::GTC;
}

// ── HTTP helpers ─────────────────────────────────────────────────────────

std::pair<int, std::string> AsterClient::http_signed_request(
    const std::string& method, const std::string& path,
    std::vector<std::pair<std::string, std::string>>& params,
    const AccountCredentials& creds) {
    static const std::string RATE_KEY = "aster";

    // Cancels (DELETE) always go through — never rate-limit them
    if (method != "DELETE" && RateLimiter::instance().is_limited(RATE_KEY)) {
        return {429, "rate limited (local)"};
    }

    ensure_domain_separator();

    // Append auth params
    long long nonce = next_nonce();
    params.emplace_back("nonce", std::to_string(nonce));
    params.emplace_back("user", creds.user_address);
    params.emplace_back("signer", creds.signer_address);

    std::string qs = build_query_string(params);
    std::string signature = sign_params(qs, creds);
    qs += "&signature=" + signature;

    HttpRequest req;
    req.url = base_url_ + path + "?" + qs;
    req.method = method;
    req.headers = {"Content-Type: application/x-www-form-urlencoded"};
    req.timeout_ms = 10000;

    auto [status, body] = CurlMultiManager::instance().execute(req);

    if (status == 429 || status == 418) {
        RateLimiter::instance().record_limit(RATE_KEY, 120);
    }

    return {status, body};
}

// ── Order creation ───────────────────────────────────────────────────────

std::optional<nlohmann::json> AsterClient::create_order(
    const Core::Order& order) {
    auto creds_opt = get_credentials(order.account_id);
    if (!creds_opt.has_value()) return std::nullopt;
    const auto& creds = creds_opt.value();

    std::vector<std::pair<std::string, std::string>> params;
    params.emplace_back("symbol", order.symbol);
    params.emplace_back("side", order.side == Core::OrderSide::BUY ? "BUY" : "SELL");
    params.emplace_back("type", map_order_type(order));
    params.emplace_back("quantity", order.quantity_str.empty()
                                        ? order.quantity.toString()
                                        : order.quantity_str);

    if (order.type == Core::OrderType::LIMIT) {
        params.emplace_back("price", order.price_str.empty()
                                         ? order.price.toString()
                                         : order.price_str);
        params.emplace_back("timeInForce", map_time_in_force(order));
    }

    if (order.reduce_only) {
        params.emplace_back("reduceOnly", "true");
    }

    if (!order.client_order_id.empty()) {
        params.emplace_back("newClientOrderId", order.client_order_id);
    }

    auto [status_code, body] =
        http_signed_request("POST", "/fapi/v3/order", params, creds);

    if (status_code >= 200 && status_code < 300 && !body.empty()) {
        auto json = nlohmann::json::parse(body, nullptr, false);
        if (!json.is_discarded()) {
            if (json.contains("code") && json["code"].get<int>() < 0) {
                std::cout << "[ASTER-API] order rejected: code="
                          << json["code"].get<int>()
                          << " msg=" << json.value("msg", "unknown")
                          << std::endl;
            }
            return json;
        }
    } else if (status_code != 429) {
        std::cout << "[ASTER-API] order HTTP error: status=" << status_code
                  << " body=" << body.substr(0, 200) << std::endl;
    }
    return std::nullopt;
}

std::vector<Core::PolymarketBulkOrderResult> AsterClient::create_orders_bulk(
    const std::vector<Core::Order>& orders) {
    std::vector<Core::PolymarketBulkOrderResult> results;
    results.reserve(orders.size());

    if (orders.empty()) return results;

    // Aster batch limit is 5 orders per request
    auto creds_opt = get_credentials(orders[0].account_id);
    if (!creds_opt.has_value()) {
        for (size_t i = 0; i < orders.size(); i++) {
            Core::PolymarketBulkOrderResult r;
            r.error_msg = "Account not found";
            results.push_back(r);
        }
        return results;
    }
    const auto& creds = creds_opt.value();

    // Process in batches of 5
    for (size_t batch_start = 0; batch_start < orders.size();
         batch_start += 5) {
        size_t batch_end = std::min(batch_start + 5, orders.size());

        nlohmann::json batch_list = nlohmann::json::array();
        for (size_t i = batch_start; i < batch_end; i++) {
            const auto& order = orders[i];
            nlohmann::json entry;
            entry["symbol"] = order.symbol;
            entry["side"] =
                order.side == Core::OrderSide::BUY ? "BUY" : "SELL";
            entry["type"] = map_order_type(order);
            entry["quantity"] = order.quantity_str.empty()
                                    ? order.quantity.toString()
                                    : order.quantity_str;

            if (order.type == Core::OrderType::LIMIT) {
                entry["price"] = order.price_str.empty()
                                     ? order.price.toString()
                                     : order.price_str;
                entry["timeInForce"] = map_time_in_force(order);
            }

            if (order.reduce_only) {
                entry["reduceOnly"] = "true";
            }

            if (!order.client_order_id.empty()) {
                entry["newClientOrderId"] = order.client_order_id;
            }

            batch_list.push_back(entry);
        }

        std::vector<std::pair<std::string, std::string>> params;
        params.emplace_back("batchOrders", batch_list.dump());

        auto [status_code, body] =
            http_signed_request("POST", "/fapi/v3/batchOrders", params, creds);

        if (status_code >= 200 && status_code < 300 && !body.empty()) {
            auto json = nlohmann::json::parse(body, nullptr, false);
            if (json.is_array()) {
                for (const auto& item : json) {
                    Core::PolymarketBulkOrderResult r;
                    if (item.contains("code") &&
                        item["code"].get<int>() != 200) {
                        r.error_msg = item.value("msg", "batch order error");
                        std::cout << "[ASTER-API] order rejected: code="
                                  << item["code"].get<int>()
                                  << " msg=" << r.error_msg << std::endl;
                    } else {
                        r.success = true;
                        r.order_id = std::to_string(
                            item.value("orderId", static_cast<long long>(0)));
                    }
                    results.push_back(r);
                }
                continue;
            }
        }

        // All failed in this batch
        for (size_t i = batch_start; i < batch_end; i++) {
            Core::PolymarketBulkOrderResult r;
            r.error_msg = "HTTP " + std::to_string(status_code) + ": " + body;
            results.push_back(r);
        }
    }

    return results;
}

// ── Cancellation ─────────────────────────────────────────────────────────

std::vector<Core::PolymarketBulkCancelResult> AsterClient::cancel_orders(
    const std::vector<std::string>& order_ids, long long account_id,
    const std::string& symbol) {
    std::vector<Core::PolymarketBulkCancelResult> results;
    results.reserve(order_ids.size());

    auto creds_opt = get_credentials(account_id);
    if (!creds_opt.has_value()) {
        for (const auto& oid : order_ids) {
            Core::PolymarketBulkCancelResult r;
            r.order_id = oid;
            r.error_msg = "Account not found";
            results.push_back(r);
        }
        return results;
    }
    const auto& creds = creds_opt.value();

    if (order_ids.size() == 1) {
        // Single cancel
        std::vector<std::pair<std::string, std::string>> params;
        params.emplace_back("symbol", symbol);
        params.emplace_back("orderId", order_ids[0]);

        auto [status_code, body] =
            http_signed_request("DELETE", "/fapi/v3/order", params, creds);

        Core::PolymarketBulkCancelResult r;
        r.order_id = order_ids[0];
        if (status_code >= 200 && status_code < 300) {
            r.success = true;
        } else {
            r.error_msg =
                "HTTP " + std::to_string(status_code) + ": " + body;
        }
        results.push_back(r);
        return results;
    }

    // Batch cancel (up to 10 per request via batchOrders DELETE)
    nlohmann::json id_list = nlohmann::json::array();
    std::vector<std::string> sent_ids;
    sent_ids.reserve(order_ids.size());
    for (const auto& oid : order_ids) {
        try {
            id_list.push_back(std::stoll(oid));
            sent_ids.push_back(oid);
        } catch (const std::exception&) {
            // Skip malformed ID; mark as failed in results
            Core::PolymarketBulkCancelResult r;
            r.order_id = oid;
            r.error_msg = "invalid order_id format: " + oid;
            results.push_back(r);
        }
    }
    if (id_list.empty()) return results;

    std::vector<std::pair<std::string, std::string>> params;
    params.emplace_back("symbol", symbol);
    params.emplace_back("orderIdList", id_list.dump());

    auto [status_code, body] =
        http_signed_request("DELETE", "/fapi/v3/batchOrders", params, creds);

    if (status_code >= 200 && status_code < 300 && !body.empty()) {
        auto json = nlohmann::json::parse(body, nullptr, false);
        if (json.is_array()) {
            size_t matched_count = std::min(sent_ids.size(), json.size());
            for (size_t i = 0; i < matched_count; i++) {
                Core::PolymarketBulkCancelResult r;
                r.order_id = sent_ids[i];
                if (json[i].contains("code") &&
                    json[i]["code"].get<int>() != 200) {
                    r.error_msg = json[i].value("msg", "cancel error");
                } else {
                    r.success = true;
                }
                results.push_back(r);
            }
            for (size_t i = matched_count; i < sent_ids.size(); i++) {
                Core::PolymarketBulkCancelResult r;
                r.order_id = sent_ids[i];
                r.error_msg = "missing from Aster batch cancel response";
                results.push_back(r);
            }
            return results;
        }
    }

    for (const auto& oid : order_ids) {
        Core::PolymarketBulkCancelResult r;
        r.order_id = oid;
        r.error_msg = "HTTP " + std::to_string(status_code) + ": " + body;
        results.push_back(r);
    }
    return results;
}

bool AsterClient::cancel_all_orders(long long account_id,
                                    const std::string& symbol) {
    auto creds_opt = get_credentials(account_id);
    if (!creds_opt.has_value()) return false;
    const auto& creds = creds_opt.value();

    std::vector<std::pair<std::string, std::string>> params;
    params.emplace_back("symbol", symbol);

    auto [status_code, body] =
        http_signed_request("DELETE", "/fapi/v3/allOpenOrders", params, creds);

    if (status_code < 200 || status_code >= 300) {
        std::cerr << "[AsterClient] cancel_all_orders failed: account_id="
                  << account_id << " symbol=" << symbol
                  << " status_code=" << status_code << " body="
                  << body.substr(0, 200) << std::endl;
        return false;
    }
    return true;
}

// ── Connection warmup ────────────────────────────────────────────────────

void AsterClient::warmup_connection() {
    if (!base_url_.empty()) {
        CurlMultiManager::instance().warmup(base_url_ + "/fapi/v3/time");
    }
}

// ── Listen key (user data stream) ────────────────────────────────────────

std::string AsterClient::create_listen_key(long long account_id) {
    auto creds_opt = get_credentials(account_id);
    if (!creds_opt.has_value()) {
        throw std::runtime_error("Account not found: " +
                                 std::to_string(account_id));
    }

    std::vector<std::pair<std::string, std::string>> params;
    auto [status, body] =
        http_signed_request("POST", "/fapi/v3/listenKey", params,
                            creds_opt.value());

    if (status != 200 || body.empty()) {
        throw std::runtime_error("Failed to create listenKey: HTTP " +
                                 std::to_string(status) + " " + body);
    }

    auto json = nlohmann::json::parse(body, nullptr, false);
    if (json.is_discarded() || !json.contains("listenKey")) {
        throw std::runtime_error("Invalid listenKey response: " + body);
    }

    return json["listenKey"].get<std::string>();
}

void AsterClient::renew_listen_key(long long account_id,
                                   const std::string& listen_key) {
    auto creds_opt = get_credentials(account_id);
    if (!creds_opt.has_value()) return;

    std::vector<std::pair<std::string, std::string>> params;
    params.emplace_back("listenKey", listen_key);

    http_signed_request("PUT", "/fapi/v3/listenKey", params,
                        creds_opt.value());
}

}  // namespace AsterDirect
