#include "trading/polymarket_client.h"

#include <openssl/hmac.h>
#include <openssl/sha.h>

#include <arpa/inet.h>
#include <netdb.h>

#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <unordered_set>

#include "crypto/base64_utils.h"
#include "crypto/eip712.h"
#include "crypto/keccak256.h"
#include "crypto/secp256k1_signer.h"
#include "curl_multi_manager.h"
#include "env_utils.h"
#include "rate_limiter.h"

namespace PolymarketDirect {

static const std::string CLOB_HOST =
    EnvUtils::env_or("POLYMARKET_REST_BASE_URL", "https://clob.polymarket.com");
static const std::string CLOB_HOSTNAME = "clob.polymarket.com";
static const std::string ZERO_ADDRESS =
    "0x0000000000000000000000000000000000000000";
static const std::string BYTES32_ZERO =
    "0x0000000000000000000000000000000000000000000000000000000000000000";

// ─── Lifecycle ──────────────────────────────────────────────────────────

PolymarketClient::PolymarketClient() = default;
PolymarketClient::~PolymarketClient() = default;

PolymarketClient::PolymarketClient(PolymarketClient&& other) noexcept
    : accounts_(std::move(other.accounts_)),
      token_options_cache_(std::move(other.token_options_cache_)) {}

PolymarketClient& PolymarketClient::operator=(PolymarketClient&& other) noexcept {
    if (this != &other) {
        accounts_ = std::move(other.accounts_);
        token_options_cache_ = std::move(other.token_options_cache_);
    }
    return *this;
}

void PolymarketClient::warmup_connection() {
    auto& mgr = CurlMultiManager::instance();

    // Resolve clob.polymarket.com DNS once at startup and pin it
    struct addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* result = nullptr;
    int rc = getaddrinfo(CLOB_HOSTNAME.c_str(), nullptr, &hints, &result);
    if (rc == 0 && result) {
        char ip[INET_ADDRSTRLEN];
        auto* addr =
            reinterpret_cast<struct sockaddr_in*>(result->ai_addr);
        inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip));
        mgr.add_dns_pinning(CLOB_HOSTNAME, 443, ip);
        freeaddrinfo(result);
    } else {
        std::cout << "[POLYMARKET_DIRECT] DNS resolve failed for "
                  << CLOB_HOSTNAME << ": " << gai_strerror(rc)
                  << " — falling back to runtime DNS" << std::endl;
    }

    mgr.warmup(CLOB_HOST + "/tick-size?token_id=0");
    std::cout << "[POLYMARKET_DIRECT] Connection warmup complete" << std::endl;

    // Start application-level heartbeat (every 15s) to prevent LB idle drops
    mgr.start_heartbeat(CLOB_HOST + "/tick-size?token_id=0", 15);
}

// ─── Account management ─────────────────────────────────────────────────

void PolymarketClient::add_account(long long account_id,
                                   const AccountCredentials& creds) {
    std::lock_guard<std::mutex> lock(accounts_mutex_);
    accounts_[account_id] = creds;
}

bool PolymarketClient::has_account(long long account_id) const {
    std::lock_guard<std::mutex> lock(accounts_mutex_);
    return accounts_.count(account_id) > 0;
}

// ─── Rounding helpers (port of py_clob_client/order_builder/helpers.py) ──

// Banker's rounding (round half to even), independent of FPU rounding mode.
// std::nearbyint uses the *current* rounding mode which can be changed by
// other code (GPU libs, etc.), silently breaking our amount calculation.
// Python's round() always uses ROUND_HALF_EVEN regardless of FPU state.
static double bankers_round(double x) {
    double r = std::round(x);  // half away from zero
    // Check if we're exactly at a halfway point
    double diff = x - (r - std::copysign(0.5, x));
    if (diff == 0.0) {
        // Exactly halfway — round to even
        if (std::fmod(std::fabs(r), 2.0) == 1.0) {
            r -= std::copysign(1.0, x);
        }
    }
    return r;
}

double PolymarketClient::round_normal(double x, int sig_digits) {
    double factor = std::pow(10.0, sig_digits);
    double scaled = x * factor;
    double rounded = bankers_round(scaled);
    return rounded / factor;
}

double PolymarketClient::round_down(double x, int sig_digits) {
    double factor = std::pow(10.0, sig_digits);
    return std::floor(x * factor) / factor;
}

double PolymarketClient::round_up(double x, int sig_digits) {
    double factor = std::pow(10.0, sig_digits);
    return std::ceil(x * factor) / factor;
}

int PolymarketClient::decimal_places(double x) {
    // Port of Python: abs(Decimal(x.__str__()).as_tuple().exponent)
    // Uses std::to_chars which produces the shortest round-trip representation,
    // matching Python's str(float) behavior. This is critical because
    // setprecision(15) exposes floating-point noise (e.g. 7.5*0.63 becomes
    // "4.72499999999999996" instead of "4.725"), causing incorrect rounding.
    char buf[32];
    auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), x);
    if (ec != std::errc()) return 0;
    std::string_view sv(buf, ptr - buf);
    auto dot = sv.find('.');
    if (dot == std::string_view::npos) return 0;
    auto last_nonzero = sv.find_last_not_of('0');
    if (last_nonzero <= dot) return 0;
    return static_cast<int>(last_nonzero - dot);
}

uint64_t PolymarketClient::to_token_decimals(double x) {
    // Must match Python's to_token_decimals exactly:
    //   f = (10**6) * x
    //   if decimal_places(f) > 0:
    //       f = round_normal(f, 0)
    //   return int(f)
    //
    // Python's round() uses banker's rounding (half to even).
    // Use bankers_round() instead of std::nearbyint to be independent of
    // the FPU rounding mode.
    // Example: 2500000.5 → Python round() = 2500000, std::round() = 2500001.
    double f = 1000000.0 * x;
    if (decimal_places(f) > 0) {
        f = bankers_round(f);
    }
    return static_cast<uint64_t>(f);
}

// ─── Amount calculation ─────────────────────────────────────────────────

RoundConfig PolymarketClient::get_round_config(const std::string& tick_size) {
    if (tick_size == "0.1") return {1, 2, 3};
    if (tick_size == "0.01") return {2, 2, 4};
    if (tick_size == "0.001") return {3, 2, 5};
    if (tick_size == "0.0001") return {4, 2, 6};
    // Default to finest granularity
    return {4, 2, 6};
}

PolymarketClient::AmountResult PolymarketClient::calculate_order_amounts(
    const std::string& side, double size, double price,
    const RoundConfig& config) {
    double raw_price = round_normal(price, config.price);

    if (side == "BUY" || side == "buy") {
        double raw_taker_amt = round_down(size, config.size);
        double raw_maker_amt = raw_taker_amt * raw_price;

        if (decimal_places(raw_maker_amt) > config.amount) {
            raw_maker_amt = round_up(raw_maker_amt, config.amount + 4);
            if (decimal_places(raw_maker_amt) > config.amount) {
                raw_maker_amt = round_down(raw_maker_amt, config.amount);
            }
        }

        return {0, to_token_decimals(raw_maker_amt),
                to_token_decimals(raw_taker_amt)};
    } else {
        // SELL
        double raw_maker_amt = round_down(size, config.size);
        double raw_taker_amt = raw_maker_amt * raw_price;

        if (decimal_places(raw_taker_amt) > config.amount) {
            raw_taker_amt = round_up(raw_taker_amt, config.amount + 4);
            if (decimal_places(raw_taker_amt) > config.amount) {
                raw_taker_amt = round_down(raw_taker_amt, config.amount);
            }
        }

        return {1, to_token_decimals(raw_maker_amt),
                to_token_decimals(raw_taker_amt)};
    }
}

// ─── Token options resolution ───────────────────────────────────────────

TokenOptions PolymarketClient::resolve_token_options(
    const std::string& token_id) {
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto it = token_options_cache_.find(token_id);
        if (it != token_options_cache_.end()) {
            return it->second;
        }
    }

    // Fetch tick_size, neg_risk, and fee_rate from CLOB API.
    // All three must succeed — signing with wrong values produces an invalid
    // order hash. Matches Python's raise_for_status() behavior.
    std::string tick_size;
    bool neg_risk = false;
    int fee_rate_bps = 0;

    // Fetch tick size
    {
        auto [code, body] =
            http_get(CLOB_HOST + "/tick-size?token_id=" + token_id);
        if (code < 200 || code >= 300) {
            throw std::runtime_error(
                "resolve_token_options: tick-size fetch failed (HTTP " +
                std::to_string(code) + ") for token " + token_id);
        }
        auto j = nlohmann::json::parse(body);
        if (!j.contains("minimum_tick_size")) {
            throw std::runtime_error(
                "resolve_token_options: tick-size response missing "
                "minimum_tick_size for token " + token_id);
        }
        auto& val = j["minimum_tick_size"];
        if (val.is_string()) {
            tick_size = val.get<std::string>();
        } else {
            // API sometimes returns a number instead of string
            tick_size = std::to_string(val.get<double>());
            tick_size.erase(tick_size.find_last_not_of('0') + 1);
            if (tick_size.back() == '.') tick_size.pop_back();
        }
    }

    // Fetch neg_risk
    {
        auto [code, body] =
            http_get(CLOB_HOST + "/neg-risk?token_id=" + token_id);
        if (code < 200 || code >= 300) {
            throw std::runtime_error(
                "resolve_token_options: neg-risk fetch failed (HTTP " +
                std::to_string(code) + ") for token " + token_id);
        }
        auto j = nlohmann::json::parse(body);
        if (!j.contains("neg_risk")) {
            throw std::runtime_error(
                "resolve_token_options: neg-risk response missing "
                "neg_risk for token " + token_id);
        }
        neg_risk = j["neg_risk"].get<bool>();
    }

    // Fetch fee rate
    {
        auto [code, body] =
            http_get(CLOB_HOST + "/fee-rate?token_id=" + token_id);
        if (code < 200 || code >= 300) {
            throw std::runtime_error(
                "resolve_token_options: fee-rate fetch failed (HTTP " +
                std::to_string(code) + ") for token " + token_id);
        }
        auto j = nlohmann::json::parse(body);
        if (j.contains("base_fee")) {
            auto& val = j["base_fee"];
            fee_rate_bps = val.is_number()
                               ? val.get<int>()
                               : std::stoi(val.get<std::string>());
        }
    }

    TokenOptions opts{tick_size, neg_risk, fee_rate_bps};

    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        token_options_cache_[token_id] = opts;
    }

    return opts;
}

// ─── HMAC-SHA256 authentication ─────────────────────────────────────────

std::string PolymarketClient::compute_hmac_signature(
    const std::string& secret, const std::string& timestamp,
    const std::string& method, const std::string& request_path,
    const std::string& body) {
    // Decode URL-safe base64 secret
    auto key_bytes = Base64Utils::decode_urlsafe(secret);

    // Build message: timestamp + method + requestPath + body
    std::string message = timestamp + method + request_path;
    if (!body.empty()) {
        message += body;
    }

    // HMAC-SHA256
    unsigned char digest[SHA256_DIGEST_LENGTH];
    unsigned int digest_len = SHA256_DIGEST_LENGTH;
    HMAC(EVP_sha256(), key_bytes.data(), static_cast<int>(key_bytes.size()),
         reinterpret_cast<const unsigned char*>(message.c_str()),
         message.size(), digest, &digest_len);

    // URL-safe base64 encode result (with padding, matching Python's
    // base64.urlsafe_b64encode which Polymarket expects)
    return Base64Utils::encode_urlsafe_padded(digest, digest_len);
}

// ─── Header construction ────────────────────────────────────────────────

std::unordered_map<std::string, std::string> PolymarketClient::build_l2_headers(
    const AccountCredentials& creds, const std::string& method,
    const std::string& request_path, const std::string& body,
    const std::string& timestamp) {
    auto hmac_sig = compute_hmac_signature(creds.api_secret, timestamp, method,
                                           request_path, body);
    return {
        {"POLY_ADDRESS", creds.signer_address},
        {"POLY_SIGNATURE", hmac_sig},
        {"POLY_TIMESTAMP", timestamp},
        {"POLY_API_KEY", creds.api_key},
        {"POLY_PASSPHRASE", creds.api_passphrase},
    };
}

std::unordered_map<std::string, std::string>
PolymarketClient::build_full_headers(const AccountCredentials& creds,
                                     const std::string& method,
                                     const std::string& request_path,
                                     const std::string& body) {
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::to_string(
        std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch())
            .count());

    auto headers =
        build_l2_headers(creds, method, request_path, body, timestamp);

    // Add builder headers if available
    if (creds.has_builder_creds()) {
        auto builder_sig = compute_hmac_signature(
            creds.builder_secret, timestamp, method, request_path, body);
        headers["POLY_BUILDER_API_KEY"] = creds.builder_api_key;
        headers["POLY_BUILDER_PASSPHRASE"] = creds.builder_passphrase;
        headers["POLY_BUILDER_SIGNATURE"] = builder_sig;
        headers["POLY_BUILDER_TIMESTAMP"] = timestamp;
    }

    headers["User-Agent"] = "py_clob_client";
    headers["Accept"] = "*/*";
    headers["Content-Type"] = "application/json";
    headers["Connection"] = "keep-alive";

    return headers;
}

// ─── Order signing ──────────────────────────────────────────────────────

PolymarketClient::SignedOrderBody PolymarketClient::build_signed_order_body(
    const AccountCredentials& creds, const Core::Order& order,
    const TokenOptions& opts, const std::string& order_type, bool post_only) {
    auto round_config = get_round_config(opts.tick_size);
    std::string side_str =
        (order.side == Core::OrderSide::BUY) ? "BUY" : "SELL";
    double price = std::stod(order.price_str);
    double size = std::stod(order.quantity_str);

    auto amounts = calculate_order_amounts(side_str, size, price, round_config);

    // salt and timestamp (ms). Both are unique-per-order; timestamp is part of
    // the v2 signed struct so it must match the JSON body.
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
    static thread_local std::mt19937_64 rng(std::random_device{}());
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    uint64_t salt =
        static_cast<uint64_t>(dist(rng) * static_cast<double>(now_ms));
    auto timestamp = static_cast<uint64_t>(now_ms);

    // For POLY_1271 (3) the signer is the funder; otherwise the EOA signs.
    std::string signer_addr = (creds.signature_type == 3)
                                  ? creds.maker_address
                                  : creds.signer_address;

    // Build EIP-712 v2 order fields
    EIP712::OrderFieldsV2 fields;
    fields.salt = salt;
    fields.maker = creds.maker_address;
    fields.signer = signer_addr;
    fields.token_id = order.clob_id;
    fields.maker_amount = amounts.maker_amount;
    fields.taker_amount = amounts.taker_amount;
    fields.side = amounts.side;
    fields.signature_type = static_cast<uint8_t>(creds.signature_type);
    fields.timestamp = timestamp;
    fields.metadata = BYTES32_ZERO;
    fields.builder = BYTES32_ZERO;

    return build_v2_order_body(creds.private_key, creds.api_key, side_str,
                               fields, opts.neg_risk, order_type, post_only);
}

PolymarketClient::SignedOrderBody PolymarketClient::build_v2_order_body(
    const std::array<uint8_t, 32>& private_key, const std::string& api_key,
    const std::string& side_str, const EIP712::OrderFieldsV2& fields,
    bool neg_risk, const std::string& order_type, bool post_only) {
    // Hash and sign
    auto hash = EIP712::hash_order_v2(fields, neg_risk);
    auto order_id_hex = "0x" + Keccak::to_hex(hash);
    auto signature = Secp256k1Signer::sign_hash_hex(hash, private_key.data());

    // Build order dict matching py-clob-client-v2 order_to_json_v2: salt, maker,
    // signer, tokenId, makerAmount, takerAmount, side, expiration,
    // signatureType, timestamp, metadata, builder, signature.
    nlohmann::ordered_json order_json;
    order_json["salt"] = fields.salt;
    order_json["maker"] = fields.maker;
    order_json["signer"] = fields.signer;
    order_json["tokenId"] = fields.token_id;
    order_json["makerAmount"] = std::to_string(fields.maker_amount);
    order_json["takerAmount"] = std::to_string(fields.taker_amount);
    order_json["side"] = side_str;
    order_json["expiration"] = "0";
    order_json["signatureType"] = static_cast<int>(fields.signature_type);
    order_json["timestamp"] = std::to_string(fields.timestamp);
    order_json["metadata"] = fields.metadata;
    order_json["builder"] = fields.builder;
    order_json["signature"] = signature;

    // Full body: {"order": {...}, "owner": api_key, "orderType": "GTC",
    // "deferExec": false, "postOnly": false}
    nlohmann::ordered_json body;
    body["order"] = order_json;
    body["owner"] = api_key;
    body["orderType"] = order_type;
    body["deferExec"] = false;
    body["postOnly"] = post_only;

    return {std::move(body), std::move(order_id_hex)};
}

bool PolymarketClient::is_rate_limited() const {
    return RateLimiter::instance().is_limited("polymarket");
}

// ─── Single order creation ──────────────────────────────────────────────

std::optional<nlohmann::json> PolymarketClient::create_order(
    const Core::Order& order) {
    if (RateLimiter::instance().is_limited("polymarket")) return std::nullopt;

    AccountCredentials creds;
    {
        std::lock_guard<std::mutex> lock(accounts_mutex_);
        auto it = accounts_.find(order.account_id);
        if (it == accounts_.end()) {
            return std::nullopt;
        }
        creds = it->second;
    }

    auto opts = resolve_token_options(order.clob_id);

    std::string order_type = "GTC";
    if (order.type == Core::OrderType::FILL_OR_KILL) {
        order_type = "FOK";
    } else if (order.type == Core::OrderType::IMMEDIATE_OR_CANCEL) {
        order_type = "FAK";
    }

    // Check server_sending_deadline_ms
    if (order.server_sending_deadline_ms > 0) {
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
        if (now_ms > order.server_sending_deadline_ms) {
            std::cout
                << "[POLYMARKET_DIRECT] Order expired server-side, deadline="
                << std::abs(order.server_sending_deadline_ms - now_ms) << "ms"
                << std::endl;
            return std::nullopt;
        }
    }

    auto signed_order = build_signed_order_body(creds, order, opts, order_type,
                                                order.post_only);
    std::string serialized = signed_order.body.dump(
        -1, ' ', false, nlohmann::json::error_handler_t::replace);

    auto headers = build_full_headers(creds, "POST", "/order", serialized);

    auto [code, response] =
        http_post(CLOB_HOST + "/order", serialized, headers);

    if (code >= 200 && code < 300) {
        try {
            return nlohmann::json::parse(response);
        } catch (const std::exception& e) {
            std::cout << "[POLYMARKET_DIRECT] create_order: JSON parse error "
                         "after HTTP " << code << ": " << e.what()
                      << " | raw: " << response.substr(0, 200) << std::endl;
            return std::nullopt;
        }
    }

    if (code == 429 || code == 418) {
        RateLimiter::instance().record_limit("polymarket", 15);
    }

    return std::nullopt;
}

// ─── Bulk order creation ────────────────────────────────────────────────

std::vector<Core::PolymarketBulkOrderResult>
PolymarketClient::create_orders_bulk(const std::vector<Core::Order>& orders) {
    std::vector<Core::PolymarketBulkOrderResult> results(orders.size());

    if (orders.empty()) return results;

    if (RateLimiter::instance().is_limited("polymarket")) {
        for (auto& r : results) r.error_msg = "rate limited (cooldown)";
        return results;
    }

    // All orders in a bulk must be from the same account
    long long account_id = orders[0].account_id;
    AccountCredentials creds;
    {
        std::lock_guard<std::mutex> lock(accounts_mutex_);
        auto it = accounts_.find(account_id);
        if (it == accounts_.end()) {
            return results;
        }
        creds = it->second;
    }

    // Build array of signed order bodies
    nlohmann::ordered_json body_array = nlohmann::ordered_json::array();

    for (size_t i = 0; i < orders.size(); i++) {
        try {
            // Check server_sending_deadline_ms
            if (orders[i].server_sending_deadline_ms > 0) {
                auto now_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
                if (now_ms > orders[i].server_sending_deadline_ms) {
                    std::cout << "[POLYMARKET_DIRECT] Order " << i
                              << " expired server-side (post-sign)"
                              << std::endl;
                    results[i].error_msg = "order expired server-side";
                    continue;
                }
            }

            auto opts = resolve_token_options(orders[i].clob_id);
            std::string order_type = "GTC";
            if (orders[i].type == Core::OrderType::FILL_OR_KILL) {
                order_type = "FOK";
            } else if (orders[i].type == Core::OrderType::IMMEDIATE_OR_CANCEL) {
                order_type = "FAK";
            }
            auto signed_order = build_signed_order_body(
                creds, orders[i], opts, order_type, orders[i].post_only);
            // Pre-populate order_id from deterministic EIP-712 hash
            // This is available BEFORE the HTTP call to CLOB
            results[i].order_id = signed_order.order_id;
            body_array.push_back(signed_order.body);
        } catch (const std::exception& e) {
            results[i].error_msg = e.what();
        }
    }

    if (body_array.empty()) return results;

    std::string serialized = body_array.dump(
        -1, ' ', false, nlohmann::json::error_handler_t::replace);
    auto headers = build_full_headers(creds, "POST", "/orders", serialized);

    auto [code, response] =
        http_post(CLOB_HOST + "/orders", serialized, headers);

    if (code < 200 || code >= 300) {
        std::string err_msg = "HTTP " + std::to_string(code);
        if (!response.empty()) {
            err_msg += ": " + response.substr(0, 200);
        }
        std::cout << "[POLYMARKET_DIRECT] create_orders_bulk failed: "
                  << err_msg << " (batch_size=" << orders.size() << ")"
                  << std::endl;
        if (code == 429) {
            RateLimiter::instance().record_limit("polymarket", 15);
        }
        for (auto& r : results) {
            r.error_msg = err_msg;
        }
        return results;
    }

    try {
        auto json_arr = nlohmann::json::parse(response);
        if (!json_arr.is_array()) {
            std::cout << "[POLYMARKET_DIRECT] create_orders_bulk: response "
                         "is not an array: "
                      << response.substr(0, 200) << std::endl;
            return results;
        }
        for (size_t i = 0; i < json_arr.size() && i < results.size(); i++) {
            auto& item = json_arr[i];
            results[i].success = item.value("success", false);
            results[i].error_msg = item.value("errorMsg", "");
            results[i].status = item.value("status", "");
            results[i].taking_amount = item.value("takingAmount", "0");
            results[i].making_amount = item.value("makingAmount", "0");
            // polymarket's assigned id, not the locally-computed hash (they
            // diverge post-v2 → mismatched id is uncancellable).
            results[i].order_id = item.value("orderID", std::string{});
        }
    } catch (const std::exception& e) {
        std::cout << "[POLYMARKET_DIRECT] create_orders_bulk: JSON parse "
                     "error: "
                  << e.what() << std::endl;
    }

    return results;
}

// ─── Sign/submit split (for in-flight cancel support) ───────────────────

std::vector<PolymarketClient::SignedOrderBody> PolymarketClient::sign_orders(
    const std::vector<Core::Order>& orders) {
    std::vector<SignedOrderBody> results;
    if (orders.empty()) return results;
    results.reserve(orders.size());

    long long account_id = orders[0].account_id;
    AccountCredentials creds;
    {
        std::lock_guard<std::mutex> lock(accounts_mutex_);
        auto it = accounts_.find(account_id);
        if (it == accounts_.end()) {
            return results;
        }
        creds = it->second;
    }

    for (size_t i = 0; i < orders.size(); i++) {
        auto opts = resolve_token_options(orders[i].clob_id);
        std::string order_type = "GTC";
        if (orders[i].type == Core::OrderType::FILL_OR_KILL) {
            order_type = "FOK";
        } else if (orders[i].type == Core::OrderType::IMMEDIATE_OR_CANCEL) {
            order_type = "FAK";
        }
        results.push_back(build_signed_order_body(creds, orders[i], opts,
                                                   order_type,
                                                   orders[i].post_only));
    }

    return results;
}

std::vector<Core::PolymarketBulkOrderResult>
PolymarketClient::submit_signed_orders(
    const std::vector<SignedOrderBody>& signed_orders, long long account_id) {
    std::vector<Core::PolymarketBulkOrderResult> results(signed_orders.size());

    if (signed_orders.empty()) return results;

    // Rate limit cooldown — skip if recently 429'd
    if (RateLimiter::instance().is_limited("polymarket")) {
        for (auto& r : results) {
            r.error_msg = "rate limited (cooldown)";
        }
        return results;
    }

    // Pre-populate order_ids from signatures
    for (size_t i = 0; i < signed_orders.size(); i++) {
        results[i].order_id = signed_orders[i].order_id;
    }

    AccountCredentials creds;
    {
        std::lock_guard<std::mutex> lock(accounts_mutex_);
        auto it = accounts_.find(account_id);
        if (it == accounts_.end()) {
            return results;
        }
        creds = it->second;
    }

    nlohmann::ordered_json body_array = nlohmann::ordered_json::array();
    for (const auto& so : signed_orders) {
        body_array.push_back(so.body);
    }

    std::string serialized = body_array.dump(
        -1, ' ', false, nlohmann::json::error_handler_t::replace);
    auto headers = build_full_headers(creds, "POST", "/orders", serialized);

    auto [code, response] =
        http_post(CLOB_HOST + "/orders", serialized, headers);

    if (code < 200 || code >= 300) {
        std::string err_msg = "HTTP " + std::to_string(code);
        if (!response.empty()) {
            err_msg += ": " + response.substr(0, 200);
        }
        std::cout << "[POLYMARKET_DIRECT] submit_signed_orders failed: "
                  << err_msg << " (batch_size=" << signed_orders.size() << ")"
                  << std::endl;
        if (code == 429) {
            RateLimiter::instance().record_limit("polymarket", 15);
            std::cout << "[POLYMARKET_DIRECT] 429 rate limited, pausing "
                         "order creation for 15s"
                      << std::endl;
        }
        for (auto& r : results) {
            r.error_msg = err_msg;
        }
        return results;
    }

    try {
        auto json_arr = nlohmann::json::parse(response);
        if (!json_arr.is_array()) {
            std::cout << "[POLYMARKET_DIRECT] submit_signed_orders: response "
                         "is not an array: "
                      << response.substr(0, 200) << std::endl;
            return results;
        }
        for (size_t i = 0; i < json_arr.size() && i < results.size(); i++) {
            auto& item = json_arr[i];
            results[i].success = item.value("success", false);
            results[i].error_msg = item.value("errorMsg", "");
            results[i].status = item.value("status", "");
            results[i].taking_amount = item.value("takingAmount", "0");
            results[i].making_amount = item.value("makingAmount", "0");
            // polymarket's assigned id, not the locally-computed hash.
            results[i].order_id = item.value("orderID", std::string{});
        }
    } catch (const std::exception& e) {
        std::cout << "[POLYMARKET_DIRECT] submit_signed_orders: JSON parse "
                     "error: "
                  << e.what() << std::endl;
    }

    return results;
}

// ─── Single signed order submission (POST /order) ───────────────────────

Core::PolymarketBulkOrderResult PolymarketClient::submit_signed_order(
    const SignedOrderBody& signed_order, long long account_id) {
    Core::PolymarketBulkOrderResult result;
    result.order_id = signed_order.order_id;

    if (RateLimiter::instance().is_limited("polymarket")) {
        result.error_msg = "rate limited (cooldown)";
        return result;
    }

    AccountCredentials creds;
    {
        std::lock_guard<std::mutex> lock(accounts_mutex_);
        auto it = accounts_.find(account_id);
        if (it == accounts_.end()) {
            result.error_msg = "account not found";
            return result;
        }
        creds = it->second;
    }

    std::string serialized = signed_order.body.dump(
        -1, ' ', false, nlohmann::json::error_handler_t::replace);
    auto headers = build_full_headers(creds, "POST", "/order", serialized);

    auto [code, response] =
        http_post(CLOB_HOST + "/order", serialized, headers);

    if (code < 200 || code >= 300) {
        std::string err_msg = "HTTP " + std::to_string(code);
        if (!response.empty()) {
            err_msg += ": " + response.substr(0, 200);
        }
        std::cout << "[POLYMARKET_DIRECT] submit_signed_order failed: "
                  << err_msg << std::endl;
        if (code == 429) {
            RateLimiter::instance().record_limit("polymarket", 15);
        }
        result.error_msg = err_msg;
        return result;
    }

    try {
        auto json_obj = nlohmann::json::parse(response);
        result.success = json_obj.value("success", false);
        result.error_msg = json_obj.value("errorMsg", "");
        result.status = json_obj.value("status", "");
        result.taking_amount = json_obj.value("takingAmount", "0");
        result.making_amount = json_obj.value("makingAmount", "0");
        // Use polymarket's assigned order id, not the locally-computed hash —
        // they diverge post-v2, and a mismatched id is uncancellable.
        result.order_id = json_obj.value("orderID", std::string{});
    } catch (const std::exception& e) {
        std::cout << "[POLYMARKET_DIRECT] submit_signed_order: JSON parse "
                     "error: "
                  << e.what() << std::endl;
    }

    return result;
}

// ─── Cancel orders ───────────────────────────────────────────────────────

std::vector<Core::PolymarketBulkCancelResult> PolymarketClient::cancel_orders(
    const std::vector<std::string>& order_ids, long long account_id) {
    std::vector<Core::PolymarketBulkCancelResult> results(order_ids.size());
    for (size_t i = 0; i < order_ids.size(); i++) {
        results[i].order_id = order_ids[i];
    }

    if (order_ids.empty()) return results;

    AccountCredentials creds;
    {
        std::lock_guard<std::mutex> lock(accounts_mutex_);
        auto it = accounts_.find(account_id);
        if (it == accounts_.end()) {
            return results;
        }
        creds = it->second;
    }

    // Body is a JSON array of order ID strings
    nlohmann::json body = nlohmann::json::array();
    for (const auto& id : order_ids) {
        body.push_back(id);
    }
    std::string serialized = body.dump();

    // Cancel uses L2-only headers (no builder headers), matching the
    // official Python SDK (py_clob_client) and polymarket_wrapper.py.
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::to_string(
        std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch())
            .count());
    auto headers =
        build_l2_headers(creds, "DELETE", "/orders", serialized, timestamp);
    headers["User-Agent"] = "py_clob_client";
    headers["Accept"] = "*/*";
    headers["Content-Type"] = "application/json";
    headers["Connection"] = "keep-alive";

    auto [code, response] =
        http_delete(CLOB_HOST + "/orders", serialized, headers);

    if (code < 200 || code >= 300) {
        for (auto& r : results) {
            r.error_msg = "HTTP " + std::to_string(code);
        }
        return results;
    }

    // Response: {"canceled": ["0xabc..."], "not_canceled": {"0xdef...":
    // "reason"}}
    try {
        auto json = nlohmann::json::parse(response);

        std::unordered_set<std::string> canceled_set;
        if (json.contains("canceled") && json["canceled"].is_array()) {
            for (const auto& id : json["canceled"]) {
                canceled_set.insert(id.get<std::string>());
            }
        }

        std::unordered_map<std::string, std::string> not_canceled_map;
        if (json.contains("not_canceled") && json["not_canceled"].is_object()) {
            for (auto& [id, reason] : json["not_canceled"].items()) {
                not_canceled_map[id] = reason.is_string()
                                           ? reason.get<std::string>()
                                           : reason.dump();
            }
        }

        for (size_t i = 0; i < results.size(); i++) {
            const auto& oid = order_ids[i];
            if (canceled_set.count(oid)) {
                results[i].success = true;
            } else {
                auto nit = not_canceled_map.find(oid);
                if (nit != not_canceled_map.end()) {
                    results[i].error_msg = nit->second;
                }
            }
        }
    } catch (const std::exception& e) {
        std::cout << "[POLYMARKET_DIRECT] cancel_orders: JSON parse error: "
                  << e.what() << " | raw: " << response.substr(0, 200)
                  << std::endl;
    }

    return results;
}

// ─── HTTP helpers ───────────────────────────────────────────────────────

std::pair<int, std::string> PolymarketClient::http_post(
    const std::string& url, const std::string& body,
    const std::unordered_map<std::string, std::string>& headers) {
    HttpRequest req;
    req.url = url;
    req.method = "POST";
    req.body = body;
    req.timeout_ms = 30000;
    req.connect_timeout_ms = 5000;
    for (const auto& [key, value] : headers) {
        req.headers.push_back(key + ": " + value);
    }
    return CurlMultiManager::instance().execute(std::move(req));
}

bool PolymarketClient::send_heartbeat(long long account_id) {
    AccountCredentials creds;
    {
        std::lock_guard<std::mutex> lock(accounts_mutex_);
        auto it = accounts_.find(account_id);
        if (it == accounts_.end()) return false;
        creds = it->second;
    }

    // Empty body; L2-only headers (same auth scheme as cancel).
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::to_string(
        std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch())
            .count());
    auto headers =
        build_l2_headers(creds, "POST", "/heartbeats", "", timestamp);
    headers["User-Agent"] = "py_clob_client";
    headers["Accept"] = "*/*";
    headers["Content-Type"] = "application/json";
    headers["Connection"] = "keep-alive";

    auto hb_t0 = std::chrono::steady_clock::now();
    auto [code, response] = http_post(CLOB_HOST + "/heartbeats", "", headers);
    long long rtt_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - hb_t0)
                           .count();
    bool ok = code >= 200 && code < 300;
    // Log every request for diagnostics (cadence, latency, failures).
    std::cout << "[PM-HEARTBEAT] account=" << account_id << " code=" << code
              << " rtt=" << rtt_ms << "ms"
              << (ok ? "" : (" FAILED body=" + response)) << std::endl;
    return ok;
}

std::pair<int, std::string> PolymarketClient::http_get(const std::string& url) {
    HttpRequest req;
    req.url = url;
    req.method = "GET";
    req.timeout_ms = 10000;
    req.connect_timeout_ms = 5000;
    req.headers.push_back("Accept: */*");
    req.headers.push_back("Connection: keep-alive");
    return CurlMultiManager::instance().execute(std::move(req));
}

std::pair<int, std::string> PolymarketClient::http_delete(
    const std::string& url, const std::string& body,
    const std::unordered_map<std::string, std::string>& headers) {
    HttpRequest req;
    req.url = url;
    req.method = "DELETE";
    req.body = body;
    req.timeout_ms = 30000;
    req.connect_timeout_ms = 5000;
    for (const auto& [key, value] : headers) {
        req.headers.push_back(key + ": " + value);
    }
    return CurlMultiManager::instance().execute(std::move(req));
}

}  // namespace PolymarketDirect
