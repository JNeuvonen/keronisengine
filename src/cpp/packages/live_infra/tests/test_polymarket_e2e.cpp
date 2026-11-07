#include <catch2/catch_all.hpp>
#include <curl/curl.h>

#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "crypto/eip712.h"
#include "crypto/keccak256.h"
#include "crypto/secp256k1_signer.h"
#include "http_reqs.h"
#include "market_making/dtypes/order.h"
#include "trading/polymarket_client.h"

// ──────────────────────────────────────────────────────────────────────────
// Helper: HTTP GET with response body
// ──────────────────────────────────────────────────────────────────────────

static std::pair<int, std::string> simple_http_get(const std::string& url) {
    CURL* curl = curl_easy_init();
    if (!curl) return {-1, ""};

    std::string response;
    long http_code = 0;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Accept: */*");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, HttpUtils::CurlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);

    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return {static_cast<int>(http_code), response};
}

// ──────────────────────────────────────────────────────────────────────────
// EIP-712 hash verification against Python reference values
// ──────────────────────────────────────────────────────────────────────────

TEST_CASE("EIP-712 hash comparison with Python reference",
          "[polymarket][e2e][live]") {
    // Fixed values — Python (web3.py + eth_abi) computed these hashes:
    // struct_hash = 1241e65e8b94368466bf2b07271f5cb6f2bf8aeee3c609bb5e197fa3deca0c15
    // domain_sep  = 1a573e3617c78403b5b4b892827992f027b03d4eaf570048b8ee8cdd84d151be
    // eip712_hash = ba0673649093e89a2a938020f59f0d8735a8b30644334afbf8ee718347e86c5b

    EIP712::OrderFields fields;
    fields.salt = 12345;
    fields.maker = "0xf39Fd6e51aad88F6F4ce6aB8827279cffFb92266";
    fields.signer = "0xf39Fd6e51aad88F6F4ce6aB8827279cffFb92266";
    fields.taker = "0x0000000000000000000000000000000000000000";
    fields.token_id =
        "104812032440315568595318241870590180481232715981192720184511444933968102038477";
    fields.maker_amount = 50000;
    fields.taker_amount = 5000000;
    fields.expiration = 0;
    fields.nonce = 0;
    fields.fee_rate_bps = 0;
    fields.side = 0;
    fields.signature_type = 0;

    auto struct_hash = EIP712::compute_struct_hash(fields);
    auto struct_hex = Keccak::to_hex(struct_hash);
    std::cout << "[cmp] C++ struct_hash = " << struct_hex << std::endl;
    REQUIRE(struct_hex ==
            "1241e65e8b94368466bf2b07271f5cb6f2bf8aeee3c609bb5e197fa3deca0c15");

    auto domain_sep = EIP712::normal_domain_separator();
    auto domain_hex = Keccak::to_hex(domain_sep);
    std::cout << "[cmp] C++ domain_sep  = " << domain_hex << std::endl;
    REQUIRE(domain_hex ==
            "1a573e3617c78403b5b4b892827992f027b03d4eaf570048b8ee8cdd84d151be");

    auto final_hash = EIP712::hash_order(fields, false);
    auto final_hex = Keccak::to_hex(final_hash);
    std::cout << "[cmp] C++ eip712_hash = " << final_hex << std::endl;
    REQUIRE(final_hex ==
            "ba0673649093e89a2a938020f59f0d8735a8b30644334afbf8ee718347e86c5b");
}

// ──────────────────────────────────────────────────────────────────────────
// E2E test: fetch creds → resolve market → create order → verify acceptance
// Tagged [live] so it's excluded from normal test runs.
// Requires: Python proxy on localhost:8000, network access to polymarket.
// ──────────────────────────────────────────────────────────────────────────

TEST_CASE("Polymarket E2E: create real order via C++ signing",
          "[polymarket][e2e][live]") {
    const std::string PROXY_BASE = "http://localhost:8000/api/v1/ccxt";
    const std::string GAMMA_API = "https://gamma-api.polymarket.com";
    const std::string CLOB_API = "https://clob.polymarket.com";

    // ── Step 1: Fetch credentials from Python proxy ──────────────────────

    std::cout << "\n[e2e] Fetching credentials from Python proxy..." << std::endl;

    auto [cred_code, cred_body] = simple_http_get(PROXY_BASE + "/get-credentials");
    REQUIRE(cred_code == 200);

    auto cred_json = nlohmann::json::parse(cred_body);
    REQUIRE(cred_json.contains("accounts"));
    REQUIRE(!cred_json["accounts"].empty());

    // Use the first account
    auto it = cred_json["accounts"].begin();
    long long account_id = std::stoll(it.key());
    auto& acct = it.value();

    std::string private_key_hex = acct["private_key"].get<std::string>();
    std::string api_key = acct["api_key"].get<std::string>();
    std::string api_secret = acct["api_secret"].get<std::string>();
    std::string api_passphrase = acct["api_passphrase"].get<std::string>();
    std::string maker_address = acct["maker_address"].get<std::string>();
    std::string signer_address = acct["signer_address"].get<std::string>();

    // Verify address derivation matches
    auto privkey = Secp256k1Signer::parse_private_key(private_key_hex);
    auto derived_address = Secp256k1Signer::derive_address(privkey.data());
    std::cout << "[e2e] Credentials loaded: signer=" << signer_address
              << " maker=" << maker_address << std::endl;
    std::cout << "[e2e] Derived address:    " << derived_address << std::endl;

    // Case-insensitive comparison (EIP-55 checksum may differ)
    std::string derived_lower = derived_address;
    std::string signer_lower = signer_address;
    std::transform(derived_lower.begin(), derived_lower.end(),
                   derived_lower.begin(), ::tolower);
    std::transform(signer_lower.begin(), signer_lower.end(),
                   signer_lower.begin(), ::tolower);
    REQUIRE(derived_lower == signer_lower);

    // Build PolymarketClient with credentials
    PolymarketDirect::PolymarketClient client;
    PolymarketDirect::AccountCredentials creds;
    creds.private_key = privkey;
    creds.api_key = api_key;
    creds.api_secret = api_secret;
    creds.api_passphrase = api_passphrase;
    creds.maker_address = maker_address;
    creds.signer_address = signer_address;

    if (acct.contains("builder_api_key") &&
        !acct["builder_api_key"].get<std::string>().empty()) {
        creds.builder_api_key = acct["builder_api_key"].get<std::string>();
        creds.builder_secret = acct["builder_secret"].get<std::string>();
        creds.builder_passphrase = acct["builder_passphrase"].get<std::string>();
    }

    client.add_account(account_id, creds);

    // ── Step 2: Resolve a safe test market ───────────────────────────────

    // Use a long-dated, low-probability market that's safe to place a
    // minimum resting order on.
    std::string slug = "china-x-taiwan-military-clash-before-2027";

    std::cout << "[e2e] Resolving market slug: " << slug << std::endl;

    auto [gamma_code, gamma_body] =
        simple_http_get(GAMMA_API + "/events/slug/" + slug);

    // If Gamma API fails, try the proxy's resolve-market endpoint
    std::string token_id;
    std::string condition_id;
    bool neg_risk = false;

    if (gamma_code == 200) {
        auto gamma_json = nlohmann::json::parse(gamma_body);

        // Gamma returns an object with "markets" array
        REQUIRE(gamma_json.contains("markets"));
        REQUIRE(!gamma_json["markets"].empty());

        auto& market = gamma_json["markets"][0];
        condition_id = market["conditionId"].get<std::string>();
        neg_risk = market.value("negRisk", false);

        // Get token_id from the first token in the market
        REQUIRE(market.contains("clobTokenIds"));
        auto token_ids_str = market["clobTokenIds"].get<std::string>();
        // clobTokenIds is a JSON string like "[\"123\",\"456\"]"
        auto token_ids_json = nlohmann::json::parse(token_ids_str);
        REQUIRE(!token_ids_json.empty());
        token_id = token_ids_json[0].get<std::string>();
    } else {
        // Fallback: use proxy's resolve-market endpoint
        std::cout << "[e2e] Gamma API failed (code=" << gamma_code
                  << "), trying proxy resolve-market..." << std::endl;

        auto [resolve_code, resolve_body] =
            simple_http_get(PROXY_BASE + "/resolve-market?slug=" + slug);
        REQUIRE(resolve_code == 200);

        auto resolve_json = nlohmann::json::parse(resolve_body);
        condition_id = resolve_json["condition_id"].get<std::string>();
        neg_risk = resolve_json["neg_risk"].get<bool>();

        REQUIRE(resolve_json.contains("tokens"));
        REQUIRE(!resolve_json["tokens"].empty());
        token_id = resolve_json["tokens"][0]["token_id"].get<std::string>();
    }

    std::cout << "[e2e] Market resolved: condition_id=" << condition_id
              << std::endl;
    std::cout << "[e2e] token_id=" << token_id << " neg_risk="
              << (neg_risk ? "true" : "false") << std::endl;

    // Fetch tick_size and fee_rate from CLOB API
    std::string tick_size = "0.01";
    int fee_rate_bps = 0;
    {
        auto [tick_code, tick_body] =
            simple_http_get(CLOB_API + "/tick-size?token_id=" + token_id);
        if (tick_code == 200) {
            auto j = nlohmann::json::parse(tick_body);
            if (j.contains("minimum_tick_size")) {
                auto& val = j["minimum_tick_size"];
                if (val.is_string()) {
                    tick_size = val.get<std::string>();
                } else {
                    // API sometimes returns a number instead of string
                    tick_size = std::to_string(val.get<double>());
                    // Clean trailing zeros: "0.010000" -> "0.01"
                    tick_size.erase(tick_size.find_last_not_of('0') + 1);
                    if (tick_size.back() == '.') tick_size.pop_back();
                }
            }
        }
        std::cout << "[e2e] tick_size=" << tick_size << std::endl;
    }
    {
        auto [fee_code, fee_body] =
            simple_http_get(CLOB_API + "/fee-rate?token_id=" + token_id);
        if (fee_code == 200) {
            auto j = nlohmann::json::parse(fee_body);
            if (j.contains("base_fee")) {
                auto& val = j["base_fee"];
                fee_rate_bps = val.is_number() ? val.get<int>()
                                               : std::stoi(val.get<std::string>());
            }
        }
        std::cout << "[e2e] fee_rate_bps=" << fee_rate_bps << std::endl;
    }

    // ── Step 3: Build a safe resting BUY order ──────────────────────────

    Core::Order order;
    order.type = Core::OrderType::LIMIT;
    order.side = Core::OrderSide::BUY;
    order.price_str = tick_size;  // Minimum price (e.g. "0.01")
    order.quantity_str = "5";     // Minimum size
    order.is_prediction_market_order = true;
    order.clob_id = token_id;
    order.account_id = account_id;
    order.post_only = false;
    order.server_sending_deadline_ms = 0;

    std::cout << "[e2e] Creating order: BUY " << order.quantity_str << " @ "
              << order.price_str << " token=" << token_id << std::endl;

    // ── Step 4: Create order via PolymarketClient::create_order() ───────

    auto result = client.create_order(order);

    REQUIRE(result.has_value());

    auto& resp = result.value();
    std::cout << "[e2e] Order response: " << resp.dump(2) << std::endl;

    // Verify the response contains an orderID
    REQUIRE(resp.contains("orderID"));
    std::string order_id = resp["orderID"].get<std::string>();
    REQUIRE(!order_id.empty());
    std::cout << "[e2e] Order created: orderID=" << order_id << std::endl;

    // ── Step 5: Cancel the order via PolymarketClient::cancel_orders() ──

    std::cout << "[e2e] Canceling order: " << order_id << std::endl;

    auto cancel_results = client.cancel_orders({order_id}, account_id);

    REQUIRE(cancel_results.size() == 1);
    REQUIRE(cancel_results[0].success);
    REQUIRE(cancel_results[0].error_msg.empty());

    std::cout << "[e2e] Order canceled successfully." << std::endl;
}

// ──────────────────────────────────────────────────────────────────────────
// E2E test: bulk create 2 orders → verify → cancel both
// Tagged [live] so it's excluded from normal test runs.
// Requires: Python proxy on localhost:8000, network access to polymarket.
// ──────────────────────────────────────────────────────────────────────────

TEST_CASE("Polymarket E2E: bulk create + cancel orders",
          "[polymarket][e2e][live]") {
    const std::string PROXY_BASE = "http://localhost:8000/api/v1/ccxt";
    const std::string GAMMA_API = "https://gamma-api.polymarket.com";
    const std::string CLOB_API = "https://clob.polymarket.com";

    // ── Step 1: Fetch credentials from Python proxy ──────────────────────

    std::cout << "\n[e2e] Fetching credentials from Python proxy..." << std::endl;

    auto [cred_code, cred_body] = simple_http_get(PROXY_BASE + "/get-credentials");
    REQUIRE(cred_code == 200);

    auto cred_json = nlohmann::json::parse(cred_body);
    REQUIRE(cred_json.contains("accounts"));
    REQUIRE(!cred_json["accounts"].empty());

    auto it = cred_json["accounts"].begin();
    long long account_id = std::stoll(it.key());
    auto& acct = it.value();

    std::string private_key_hex = acct["private_key"].get<std::string>();
    std::string api_key = acct["api_key"].get<std::string>();
    std::string api_secret = acct["api_secret"].get<std::string>();
    std::string api_passphrase = acct["api_passphrase"].get<std::string>();
    std::string maker_address = acct["maker_address"].get<std::string>();
    std::string signer_address = acct["signer_address"].get<std::string>();

    auto privkey = Secp256k1Signer::parse_private_key(private_key_hex);

    PolymarketDirect::PolymarketClient client;
    PolymarketDirect::AccountCredentials creds;
    creds.private_key = privkey;
    creds.api_key = api_key;
    creds.api_secret = api_secret;
    creds.api_passphrase = api_passphrase;
    creds.maker_address = maker_address;
    creds.signer_address = signer_address;

    if (acct.contains("builder_api_key") &&
        !acct["builder_api_key"].get<std::string>().empty()) {
        creds.builder_api_key = acct["builder_api_key"].get<std::string>();
        creds.builder_secret = acct["builder_secret"].get<std::string>();
        creds.builder_passphrase = acct["builder_passphrase"].get<std::string>();
    }

    client.add_account(account_id, creds);

    // ── Step 2: Resolve a safe test market ───────────────────────────────

    std::string slug = "china-x-taiwan-military-clash-before-2027";

    std::cout << "[e2e] Resolving market slug: " << slug << std::endl;

    auto [gamma_code, gamma_body] =
        simple_http_get(GAMMA_API + "/events/slug/" + slug);

    std::string token_id;
    std::string condition_id;
    bool neg_risk = false;

    if (gamma_code == 200) {
        auto gamma_json = nlohmann::json::parse(gamma_body);
        REQUIRE(gamma_json.contains("markets"));
        REQUIRE(!gamma_json["markets"].empty());

        auto& market = gamma_json["markets"][0];
        condition_id = market["conditionId"].get<std::string>();
        neg_risk = market.value("negRisk", false);

        REQUIRE(market.contains("clobTokenIds"));
        auto token_ids_str = market["clobTokenIds"].get<std::string>();
        auto token_ids_json = nlohmann::json::parse(token_ids_str);
        REQUIRE(!token_ids_json.empty());
        token_id = token_ids_json[0].get<std::string>();
    } else {
        std::cout << "[e2e] Gamma API failed (code=" << gamma_code
                  << "), trying proxy resolve-market..." << std::endl;

        auto [resolve_code, resolve_body] =
            simple_http_get(PROXY_BASE + "/resolve-market?slug=" + slug);
        REQUIRE(resolve_code == 200);

        auto resolve_json = nlohmann::json::parse(resolve_body);
        condition_id = resolve_json["condition_id"].get<std::string>();
        neg_risk = resolve_json["neg_risk"].get<bool>();

        REQUIRE(resolve_json.contains("tokens"));
        REQUIRE(!resolve_json["tokens"].empty());
        token_id = resolve_json["tokens"][0]["token_id"].get<std::string>();
    }

    std::cout << "[e2e] Market resolved: condition_id=" << condition_id
              << std::endl;
    std::cout << "[e2e] token_id=" << token_id << " neg_risk="
              << (neg_risk ? "true" : "false") << std::endl;

    // Fetch tick_size from CLOB API
    std::string tick_size = "0.01";
    {
        auto [tick_code, tick_body] =
            simple_http_get(CLOB_API + "/tick-size?token_id=" + token_id);
        if (tick_code == 200) {
            auto j = nlohmann::json::parse(tick_body);
            if (j.contains("minimum_tick_size")) {
                auto& val = j["minimum_tick_size"];
                if (val.is_string()) {
                    tick_size = val.get<std::string>();
                } else {
                    tick_size = std::to_string(val.get<double>());
                    tick_size.erase(tick_size.find_last_not_of('0') + 1);
                    if (tick_size.back() == '.') tick_size.pop_back();
                }
            }
        }
        std::cout << "[e2e] tick_size=" << tick_size << std::endl;
    }

    // ── Step 3: Build 2 orders at different minimum prices ───────────────

    double tick_val = std::stod(tick_size);
    std::string price1 = tick_size;
    std::string price2 = std::to_string(tick_val * 2);
    // Clean trailing zeros from price2
    price2.erase(price2.find_last_not_of('0') + 1);
    if (price2.back() == '.') price2.pop_back();

    Core::Order order1;
    order1.type = Core::OrderType::LIMIT;
    order1.side = Core::OrderSide::BUY;
    order1.price_str = price1;
    order1.quantity_str = "5";
    order1.is_prediction_market_order = true;
    order1.clob_id = token_id;
    order1.account_id = account_id;
    order1.post_only = false;
    order1.server_sending_deadline_ms = 0;

    Core::Order order2;
    order2.type = Core::OrderType::LIMIT;
    order2.side = Core::OrderSide::BUY;
    order2.price_str = price2;
    order2.quantity_str = "5";
    order2.is_prediction_market_order = true;
    order2.clob_id = token_id;
    order2.account_id = account_id;
    order2.post_only = false;
    order2.server_sending_deadline_ms = 0;

    std::cout << "[e2e] Bulk creating 2 orders: BUY 5 @ " << price1
              << " and BUY 5 @ " << price2 << std::endl;

    // ── Step 4: Bulk create via PolymarketClient::create_orders_bulk() ───

    std::vector<Core::Order> orders = {order1, order2};
    auto results = client.create_orders_bulk(orders);

    REQUIRE(results.size() == 2);
    for (size_t i = 0; i < results.size(); i++) {
        std::cout << "[e2e] Order " << i << ": orderID=" << results[i].order_id
                  << " status=" << results[i].status
                  << " success=" << results[i].success << std::endl;
        REQUIRE(results[i].success);
        REQUIRE(!results[i].order_id.empty());
    }

    // ── Step 5: Cancel both orders ───────────────────────────────────────

    std::vector<std::string> order_ids = {results[0].order_id,
                                          results[1].order_id};

    std::cout << "[e2e] Canceling " << order_ids.size() << " orders..."
              << std::endl;

    auto cancel_results = client.cancel_orders(order_ids, account_id);

    REQUIRE(cancel_results.size() == 2);
    for (size_t i = 0; i < cancel_results.size(); i++) {
        REQUIRE(cancel_results[i].success);
        REQUIRE(cancel_results[i].error_msg.empty());
    }

    std::cout << "[e2e] All orders canceled successfully." << std::endl;
}

// ──────────────────────────────────────────────────────────────────────────
// E2E test: in-flight cancel — sign orders, then race create vs cancel
// Sign N orders, get deterministic IDs, then:
//   Thread 1: submit_signed_orders (HTTP POST)
//   Thread 2: sleep 1ms, cancel_orders using pre-computed IDs
// Measure success rate of in-flight cancels.
// ──────────────────────────────────────────────────────────────────────────

TEST_CASE("Polymarket E2E: in-flight cancel race",
          "[polymarket][e2e][live]") {
    const std::string PROXY_BASE = "http://localhost:8000/api/v1/ccxt";
    const std::string GAMMA_API = "https://gamma-api.polymarket.com";
    const std::string CLOB_API = "https://clob.polymarket.com";

    // ── Fetch credentials ────────────────────────────────────────────────

    auto [cred_code, cred_body] = simple_http_get(PROXY_BASE + "/get-credentials");
    REQUIRE(cred_code == 200);

    auto cred_json = nlohmann::json::parse(cred_body);
    REQUIRE(cred_json.contains("accounts"));
    REQUIRE(!cred_json["accounts"].empty());

    auto it = cred_json["accounts"].begin();
    long long account_id = std::stoll(it.key());
    auto& acct = it.value();

    auto privkey = Secp256k1Signer::parse_private_key(
        acct["private_key"].get<std::string>());

    PolymarketDirect::PolymarketClient client;
    PolymarketDirect::AccountCredentials creds;
    creds.private_key = privkey;
    creds.api_key = acct["api_key"].get<std::string>();
    creds.api_secret = acct["api_secret"].get<std::string>();
    creds.api_passphrase = acct["api_passphrase"].get<std::string>();
    creds.maker_address = acct["maker_address"].get<std::string>();
    creds.signer_address = acct["signer_address"].get<std::string>();

    if (acct.contains("builder_api_key") &&
        !acct["builder_api_key"].get<std::string>().empty()) {
        creds.builder_api_key = acct["builder_api_key"].get<std::string>();
        creds.builder_secret = acct["builder_secret"].get<std::string>();
        creds.builder_passphrase = acct["builder_passphrase"].get<std::string>();
    }

    client.add_account(account_id, creds);

    // ── Resolve market ───────────────────────────────────────────────────

    std::string slug = "china-x-taiwan-military-clash-before-2027";
    auto [gamma_code, gamma_body] =
        simple_http_get(GAMMA_API + "/events/slug/" + slug);

    std::string token_id;
    if (gamma_code == 200) {
        auto gamma_json = nlohmann::json::parse(gamma_body);
        REQUIRE(gamma_json.contains("markets"));
        auto& market = gamma_json["markets"][0];
        auto token_ids_json = nlohmann::json::parse(
            market["clobTokenIds"].get<std::string>());
        token_id = token_ids_json[0].get<std::string>();
    } else {
        auto [rc, rb] =
            simple_http_get(PROXY_BASE + "/resolve-market?slug=" + slug);
        REQUIRE(rc == 200);
        auto rj = nlohmann::json::parse(rb);
        token_id = rj["tokens"][0]["token_id"].get<std::string>();
    }

    std::cout << "\n[e2e] In-flight cancel test, token_id=" << token_id
              << std::endl;

    // Fetch tick_size
    std::string tick_size = "0.01";
    {
        auto [tc, tb] =
            simple_http_get(CLOB_API + "/tick-size?token_id=" + token_id);
        if (tc == 200) {
            auto j = nlohmann::json::parse(tb);
            if (j.contains("minimum_tick_size")) {
                auto& val = j["minimum_tick_size"];
                if (val.is_string()) {
                    tick_size = val.get<std::string>();
                } else {
                    tick_size = std::to_string(val.get<double>());
                    tick_size.erase(tick_size.find_last_not_of('0') + 1);
                    if (tick_size.back() == '.') tick_size.pop_back();
                }
            }
        }
    }

    // ── Build N orders at minimum price ──────────────────────────────────

    const int NUM_ORDERS = 5;
    std::vector<Core::Order> orders;
    orders.reserve(NUM_ORDERS);

    double tick_val = std::stod(tick_size);
    for (int i = 0; i < NUM_ORDERS; i++) {
        Core::Order o;
        o.type = Core::OrderType::LIMIT;
        o.side = Core::OrderSide::BUY;
        // Use slightly different prices so each order gets a unique salt/hash
        std::string price = std::to_string(tick_val * (i + 1));
        price.erase(price.find_last_not_of('0') + 1);
        if (price.back() == '.') price.pop_back();
        o.price_str = price;
        o.quantity_str = "5";
        o.is_prediction_market_order = true;
        o.clob_id = token_id;
        o.account_id = account_id;
        o.post_only = false;
        o.server_sending_deadline_ms = 0;
        orders.push_back(o);
    }

    // ── Sign orders (get deterministic IDs before HTTP) ──────────────────

    auto signed_orders = client.sign_orders(orders);
    REQUIRE(signed_orders.size() == NUM_ORDERS);

    std::vector<std::string> pre_computed_ids;
    pre_computed_ids.reserve(NUM_ORDERS);
    for (const auto& so : signed_orders) {
        REQUIRE(!so.order_id.empty());
        pre_computed_ids.push_back(so.order_id);
        std::cout << "[e2e] Signed order_id=" << so.order_id << std::endl;
    }

    // ── Race: submit vs cancel ───────────────────────────────────────────

    std::vector<Core::PolymarketBulkOrderResult> create_results;
    std::vector<Core::PolymarketBulkCancelResult> cancel_results;

    std::thread create_thread([&]() {
        create_results =
            client.submit_signed_orders(signed_orders, account_id);
    });

    std::thread cancel_thread([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        cancel_results =
            client.cancel_orders(pre_computed_ids, account_id);
    });

    create_thread.join();
    cancel_thread.join();

    // ── Report results ───────────────────────────────────────────────────

    std::cout << "\n[e2e] === In-flight cancel results ===" << std::endl;

    int create_success = 0, create_fail = 0;
    for (size_t i = 0; i < create_results.size(); i++) {
        std::cout << "[e2e] CREATE[" << i
                  << "] success=" << create_results[i].success
                  << " status=" << create_results[i].status
                  << " order_id=" << create_results[i].order_id
                  << " error=" << create_results[i].error_msg << std::endl;
        if (create_results[i].success) create_success++;
        else create_fail++;
    }

    int cancel_success = 0, cancel_fail = 0;
    for (size_t i = 0; i < cancel_results.size(); i++) {
        std::cout << "[e2e] CANCEL[" << i
                  << "] success=" << cancel_results[i].success
                  << " order_id=" << cancel_results[i].order_id
                  << " error=" << cancel_results[i].error_msg << std::endl;
        if (cancel_results[i].success) cancel_success++;
        else cancel_fail++;
    }

    std::cout << "\n[e2e] CREATE: " << create_success << "/" << NUM_ORDERS
              << " succeeded" << std::endl;
    std::cout << "[e2e] CANCEL: " << cancel_success << "/" << NUM_ORDERS
              << " succeeded (in-flight)" << std::endl;

    // Verify order IDs match between sign and create
    for (size_t i = 0; i < create_results.size(); i++) {
        CHECK(create_results[i].order_id == pre_computed_ids[i]);
    }

    // Clean up: cancel any orders that might still be resting
    // (create succeeded but in-flight cancel missed)
    std::vector<std::string> cleanup_ids;
    for (size_t i = 0; i < create_results.size(); i++) {
        if (create_results[i].success &&
            (i >= cancel_results.size() || !cancel_results[i].success)) {
            cleanup_ids.push_back(create_results[i].order_id);
        }
    }
    if (!cleanup_ids.empty()) {
        std::cout << "[e2e] Cleanup: canceling " << cleanup_ids.size()
                  << " remaining orders..." << std::endl;
        auto cleanup = client.cancel_orders(cleanup_ids, account_id);
        for (size_t i = 0; i < cleanup.size(); i++) {
            std::cout << "[e2e] CLEANUP[" << i
                      << "] success=" << cleanup[i].success << std::endl;
        }
    }

    std::cout << "[e2e] In-flight cancel test complete." << std::endl;
}

// ──────────────────────────────────────────────────────────────────────────
// E2E test: find minimum reliable cancel delay in 10ms buckets.
// For each bucket (10, 20, 30, ...), run 15 trials of sign→create+cancel.
// A bucket passes only if all 15/15 cancels succeed.
// Stops at the first bucket that passes.
// ──────────────────────────────────────────────────────────────────────────

TEST_CASE("Polymarket E2E: cancel delay sweep",
          "[polymarket][e2e][live]") {
    const std::string PROXY_BASE = "http://localhost:8000/api/v1/ccxt";
    const std::string GAMMA_API = "https://gamma-api.polymarket.com";
    const std::string CLOB_API = "https://clob.polymarket.com";

    // ── Fetch credentials ────────────────────────────────────────────────

    auto [cred_code, cred_body] = simple_http_get(PROXY_BASE + "/get-credentials");
    REQUIRE(cred_code == 200);

    auto cred_json = nlohmann::json::parse(cred_body);
    auto it = cred_json["accounts"].begin();
    long long account_id = std::stoll(it.key());
    auto& acct = it.value();

    auto privkey = Secp256k1Signer::parse_private_key(
        acct["private_key"].get<std::string>());

    PolymarketDirect::PolymarketClient client;
    PolymarketDirect::AccountCredentials creds;
    creds.private_key = privkey;
    creds.api_key = acct["api_key"].get<std::string>();
    creds.api_secret = acct["api_secret"].get<std::string>();
    creds.api_passphrase = acct["api_passphrase"].get<std::string>();
    creds.maker_address = acct["maker_address"].get<std::string>();
    creds.signer_address = acct["signer_address"].get<std::string>();

    if (acct.contains("builder_api_key") &&
        !acct["builder_api_key"].get<std::string>().empty()) {
        creds.builder_api_key = acct["builder_api_key"].get<std::string>();
        creds.builder_secret = acct["builder_secret"].get<std::string>();
        creds.builder_passphrase = acct["builder_passphrase"].get<std::string>();
    }

    client.add_account(account_id, creds);

    // ── Resolve market ───────────────────────────────────────────────────

    std::string slug = "china-x-taiwan-military-clash-before-2027";
    auto [gamma_code, gamma_body] =
        simple_http_get(GAMMA_API + "/events/slug/" + slug);

    std::string token_id;
    if (gamma_code == 200) {
        auto gamma_json = nlohmann::json::parse(gamma_body);
        auto token_ids_json = nlohmann::json::parse(
            gamma_json["markets"][0]["clobTokenIds"].get<std::string>());
        token_id = token_ids_json[0].get<std::string>();
    } else {
        auto [rc, rb] =
            simple_http_get(PROXY_BASE + "/resolve-market?slug=" + slug);
        REQUIRE(rc == 200);
        auto rj = nlohmann::json::parse(rb);
        token_id = rj["tokens"][0]["token_id"].get<std::string>();
    }

    std::string tick_size = "0.01";
    {
        auto [tc, tb] =
            simple_http_get(CLOB_API + "/tick-size?token_id=" + token_id);
        if (tc == 200) {
            auto j = nlohmann::json::parse(tb);
            if (j.contains("minimum_tick_size")) {
                auto& val = j["minimum_tick_size"];
                if (val.is_string()) {
                    tick_size = val.get<std::string>();
                } else {
                    tick_size = std::to_string(val.get<double>());
                    tick_size.erase(tick_size.find_last_not_of('0') + 1);
                    if (tick_size.back() == '.') tick_size.pop_back();
                }
            }
        }
    }

    double tick_val = std::stod(tick_size);

    std::cout << "\n[sweep] Cancel delay sweep, token=" << token_id
              << " tick_size=" << tick_size << std::endl;

    // ── Sweep 10ms buckets ───────────────────────────────────────────────

    const int NUM_TRIALS = 15;
    const int MAX_DELAY_MS = 200;

    for (int delay_ms = 10; delay_ms <= MAX_DELAY_MS; delay_ms += 10) {
        int successes = 0;

        for (int trial = 0; trial < NUM_TRIALS; trial++) {
            // Build 1 order per trial (unique price via trial index + delay)
            Core::Order order;
            order.type = Core::OrderType::LIMIT;
            order.side = Core::OrderSide::BUY;
            order.price_str = tick_size;
            order.quantity_str = "5";
            order.is_prediction_market_order = true;
            order.clob_id = token_id;
            order.account_id = account_id;
            order.post_only = false;
            order.server_sending_deadline_ms = 0;

            auto signed_bodies = client.sign_orders({order});
            if (signed_bodies.empty()) continue;

            std::vector<Core::PolymarketBulkOrderResult> create_res;
            std::vector<Core::PolymarketBulkCancelResult> cancel_res;

            std::thread create_t([&]() {
                create_res =
                    client.submit_signed_orders(signed_bodies, account_id);
            });

            std::thread cancel_t([&]() {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(delay_ms));
                cancel_res = client.cancel_orders(
                    {signed_bodies[0].order_id}, account_id);
            });

            create_t.join();
            cancel_t.join();

            bool cancel_ok = !cancel_res.empty() && cancel_res[0].success;
            if (cancel_ok) {
                successes++;
            } else {
                // Cleanup: cancel the resting order
                client.cancel_orders({signed_bodies[0].order_id}, account_id);
            }
        }

        std::cout << "[sweep] delay=" << delay_ms << "ms  cancel_success="
                  << successes << "/" << NUM_TRIALS << std::endl;

        if (successes == NUM_TRIALS) {
            std::cout << "[sweep] MINIMUM RELIABLE DELAY: " << delay_ms
                      << "ms (15/15)" << std::endl;
            break;
        }

        // Cooldown between buckets to avoid rate limiting
        std::this_thread::sleep_for(std::chrono::milliseconds(15000));
    }

    std::cout << "[sweep] Done." << std::endl;
}

// ──────────────────────────────────────────────────────────────────────────
// E2E test: verify 20ms cancel guard is reliable with bulk orders.
// Creates 15 orders at once, waits 20ms, cancels all 15.
// Repeats 100 times. All 100 iterations must succeed (100% cancel rate).
// Tagged [live] so it's excluded from normal test runs.
// ──────────────────────────────────────────────────────────────────────────

TEST_CASE("Polymarket E2E: 20ms cancel guard reliability (15 orders x 100 rounds)",
          "[polymarket][e2e][live]") {
    const std::string PROXY_BASE = "http://localhost:8000/api/v1/ccxt";
    const std::string GAMMA_API = "https://gamma-api.polymarket.com";
    const std::string CLOB_API = "https://clob.polymarket.com";

    // ── Fetch credentials ────────────────────────────────────────────────

    auto [cred_code, cred_body] = simple_http_get(PROXY_BASE + "/get-credentials");
    REQUIRE(cred_code == 200);

    auto cred_json = nlohmann::json::parse(cred_body);
    auto it = cred_json["accounts"].begin();
    long long account_id = std::stoll(it.key());
    auto& acct = it.value();

    auto privkey = Secp256k1Signer::parse_private_key(
        acct["private_key"].get<std::string>());

    PolymarketDirect::PolymarketClient client;
    PolymarketDirect::AccountCredentials creds;
    creds.private_key = privkey;
    creds.api_key = acct["api_key"].get<std::string>();
    creds.api_secret = acct["api_secret"].get<std::string>();
    creds.api_passphrase = acct["api_passphrase"].get<std::string>();
    creds.maker_address = acct["maker_address"].get<std::string>();
    creds.signer_address = acct["signer_address"].get<std::string>();

    if (acct.contains("builder_api_key") &&
        !acct["builder_api_key"].get<std::string>().empty()) {
        creds.builder_api_key = acct["builder_api_key"].get<std::string>();
        creds.builder_secret = acct["builder_secret"].get<std::string>();
        creds.builder_passphrase = acct["builder_passphrase"].get<std::string>();
    }

    client.add_account(account_id, creds);

    // ── Resolve market ───────────────────────────────────────────────────

    std::string slug = "china-x-taiwan-military-clash-before-2027";
    auto [gamma_code, gamma_body] =
        simple_http_get(GAMMA_API + "/events/slug/" + slug);

    std::string token_id;
    if (gamma_code == 200) {
        auto gamma_json = nlohmann::json::parse(gamma_body);
        auto token_ids_json = nlohmann::json::parse(
            gamma_json["markets"][0]["clobTokenIds"].get<std::string>());
        token_id = token_ids_json[0].get<std::string>();
    } else {
        auto [rc, rb] =
            simple_http_get(PROXY_BASE + "/resolve-market?slug=" + slug);
        REQUIRE(rc == 200);
        auto rj = nlohmann::json::parse(rb);
        token_id = rj["tokens"][0]["token_id"].get<std::string>();
    }

    std::string tick_size = "0.01";
    {
        auto [tc, tb] =
            simple_http_get(CLOB_API + "/tick-size?token_id=" + token_id);
        if (tc == 200) {
            auto j = nlohmann::json::parse(tb);
            if (j.contains("minimum_tick_size")) {
                auto& val = j["minimum_tick_size"];
                if (val.is_string()) {
                    tick_size = val.get<std::string>();
                } else {
                    tick_size = std::to_string(val.get<double>());
                    tick_size.erase(tick_size.find_last_not_of('0') + 1);
                    if (tick_size.back() == '.') tick_size.pop_back();
                }
            }
        }
    }

    double tick_val = std::stod(tick_size);

    std::cout << "\n[guard] 20ms cancel guard test, token=" << token_id
              << " tick_size=" << tick_size << std::endl;

    // ── Run 100 rounds ─────────────────────────────────────────────────

    const int NUM_ROUNDS = 100;
    const int NUM_ORDERS = 15;
    const int CANCEL_DELAY_MS = 20;

    int total_rounds_passed = 0;
    int total_cancel_failures = 0;

    for (int round = 0; round < NUM_ROUNDS; round++) {
        // Build 15 orders at different minimum prices
        std::vector<Core::Order> orders;
        orders.reserve(NUM_ORDERS);

        for (int i = 0; i < NUM_ORDERS; i++) {
            Core::Order o;
            o.type = Core::OrderType::LIMIT;
            o.side = Core::OrderSide::BUY;
            std::string price = std::to_string(tick_val * (i + 1));
            price.erase(price.find_last_not_of('0') + 1);
            if (price.back() == '.') price.pop_back();
            o.price_str = price;
            o.quantity_str = "5";
            o.is_prediction_market_order = true;
            o.clob_id = token_id;
            o.account_id = account_id;
            o.post_only = false;
            o.server_sending_deadline_ms = 0;
            orders.push_back(o);
        }

        // Sign orders (deterministic IDs)
        auto signed_bodies = client.sign_orders(orders);
        if (signed_bodies.size() != NUM_ORDERS) {
            std::cout << "[guard] Round " << round
                      << ": signing failed, skipping" << std::endl;
            continue;
        }

        std::vector<std::string> order_ids;
        order_ids.reserve(NUM_ORDERS);
        for (const auto& sb : signed_bodies) {
            order_ids.push_back(sb.order_id);
        }

        // Submit bulk
        auto create_results =
            client.submit_signed_orders(signed_bodies, account_id);

        // Wait the guard delay
        std::this_thread::sleep_for(std::chrono::milliseconds(CANCEL_DELAY_MS));

        // Cancel all
        auto cancel_results = client.cancel_orders(order_ids, account_id);

        // Count results
        int cancel_ok = 0;
        for (size_t i = 0; i < cancel_results.size(); i++) {
            if (cancel_results[i].success) {
                cancel_ok++;
            }
        }

        bool round_passed = (cancel_ok == NUM_ORDERS);
        if (round_passed) {
            total_rounds_passed++;
        } else {
            total_cancel_failures += (NUM_ORDERS - cancel_ok);
            std::cout << "[guard] Round " << round << " FAILED: cancel_success="
                      << cancel_ok << "/" << NUM_ORDERS << std::endl;

            // Cleanup any orders that weren't cancelled
            std::vector<std::string> cleanup_ids;
            for (size_t i = 0; i < cancel_results.size(); i++) {
                if (!cancel_results[i].success) {
                    cleanup_ids.push_back(order_ids[i]);
                }
            }
            if (!cleanup_ids.empty()) {
                client.cancel_orders(cleanup_ids, account_id);
            }
        }

        if ((round + 1) % 10 == 0) {
            std::cout << "[guard] Progress: " << (round + 1) << "/"
                      << NUM_ROUNDS << " rounds, " << total_rounds_passed
                      << " passed" << std::endl;
        }
    }

    std::cout << "\n[guard] === Results ===" << std::endl;
    std::cout << "[guard] Rounds passed: " << total_rounds_passed << "/"
              << NUM_ROUNDS << std::endl;
    std::cout << "[guard] Total cancel failures: " << total_cancel_failures
              << std::endl;

    REQUIRE(total_rounds_passed == NUM_ROUNDS);

    std::cout << "[guard] 20ms cancel guard is reliable: "
              << NUM_ROUNDS << "/" << NUM_ROUNDS << " rounds with "
              << NUM_ORDERS << " orders each." << std::endl;
}

// ──────────────────────────────────────────────────────────────────────────
// E2E test: find maximum batch size for bulk order submission.
// Tests batch sizes 1, 2, 5, 10, 15, 20, 25, 30 via submit_signed_orders.
// For each size, creates orders, submits, checks success, then cancels.
// Reports which sizes succeed and which fail.
// ──────────────────────────────────────────────────────────────────────────

TEST_CASE("Polymarket E2E: batch size limits for bulk order submission",
          "[polymarket][e2e][live]") {
    const std::string PROXY_BASE = "http://localhost:8000/api/v1/ccxt";
    const std::string GAMMA_API = "https://gamma-api.polymarket.com";
    const std::string CLOB_API = "https://clob.polymarket.com";

    // ── Fetch credentials ────────────────────────────────────────────────

    auto [cred_code, cred_body] =
        simple_http_get(PROXY_BASE + "/get-credentials");
    REQUIRE(cred_code == 200);

    auto cred_json = nlohmann::json::parse(cred_body);
    auto it = cred_json["accounts"].begin();
    long long account_id = std::stoll(it.key());
    auto& acct = it.value();

    auto privkey = Secp256k1Signer::parse_private_key(
        acct["private_key"].get<std::string>());

    PolymarketDirect::PolymarketClient client;
    PolymarketDirect::AccountCredentials creds;
    creds.private_key = privkey;
    creds.api_key = acct["api_key"].get<std::string>();
    creds.api_secret = acct["api_secret"].get<std::string>();
    creds.api_passphrase = acct["api_passphrase"].get<std::string>();
    creds.maker_address = acct["maker_address"].get<std::string>();
    creds.signer_address = acct["signer_address"].get<std::string>();

    if (acct.contains("builder_api_key") &&
        !acct["builder_api_key"].get<std::string>().empty()) {
        creds.builder_api_key = acct["builder_api_key"].get<std::string>();
        creds.builder_secret = acct["builder_secret"].get<std::string>();
        creds.builder_passphrase =
            acct["builder_passphrase"].get<std::string>();
    }

    client.add_account(account_id, creds);

    // ── Resolve market ───────────────────────────────────────────────────

    std::string slug = "china-x-taiwan-military-clash-before-2027";
    auto [gamma_code, gamma_body] =
        simple_http_get(GAMMA_API + "/events/slug/" + slug);

    std::string token_id;
    if (gamma_code == 200) {
        auto gamma_json = nlohmann::json::parse(gamma_body);
        auto token_ids_json = nlohmann::json::parse(
            gamma_json["markets"][0]["clobTokenIds"].get<std::string>());
        token_id = token_ids_json[0].get<std::string>();
    } else {
        auto [rc, rb] =
            simple_http_get(PROXY_BASE + "/resolve-market?slug=" + slug);
        REQUIRE(rc == 200);
        auto rj = nlohmann::json::parse(rb);
        token_id = rj["tokens"][0]["token_id"].get<std::string>();
    }

    std::string tick_size = "0.01";
    {
        auto [tc, tb] =
            simple_http_get(CLOB_API + "/tick-size?token_id=" + token_id);
        if (tc == 200) {
            auto j = nlohmann::json::parse(tb);
            if (j.contains("minimum_tick_size")) {
                auto& val = j["minimum_tick_size"];
                if (val.is_string()) {
                    tick_size = val.get<std::string>();
                } else {
                    tick_size = std::to_string(val.get<double>());
                    tick_size.erase(tick_size.find_last_not_of('0') + 1);
                    if (tick_size.back() == '.') tick_size.pop_back();
                }
            }
        }
    }

    double tick_val = std::stod(tick_size);

    std::cout << "\n[batch] Batch size limit test, token=" << token_id
              << " tick_size=" << tick_size << std::endl;

    // ── Test different batch sizes ───────────────────────────────────────

    std::vector<int> batch_sizes = {1, 2, 5, 10, 15, 16, 20, 25, 30};
    int max_working_batch = 0;

    for (int batch_size : batch_sizes) {
        // Build N orders at different minimum prices
        std::vector<Core::Order> orders;
        orders.reserve(batch_size);

        for (int i = 0; i < batch_size; i++) {
            Core::Order o;
            o.type = Core::OrderType::LIMIT;
            o.side = Core::OrderSide::BUY;
            std::string price = std::to_string(tick_val * (i + 1));
            price.erase(price.find_last_not_of('0') + 1);
            if (price.back() == '.') price.pop_back();
            o.price_str = price;
            o.quantity_str = "5";
            o.is_prediction_market_order = true;
            o.clob_id = token_id;
            o.account_id = account_id;
            o.post_only = false;
            o.server_sending_deadline_ms = 0;
            orders.push_back(o);
        }

        // Sign orders
        auto signed_bodies = client.sign_orders(orders);
        if ((int)signed_bodies.size() != batch_size) {
            std::cout << "[batch] batch_size=" << batch_size
                      << " SIGNING FAILED (got " << signed_bodies.size()
                      << ")" << std::endl;
            continue;
        }

        // Collect order IDs for cleanup
        std::vector<std::string> order_ids;
        order_ids.reserve(batch_size);
        for (const auto& sb : signed_bodies) {
            order_ids.push_back(sb.order_id);
        }

        // Submit
        auto results =
            client.submit_signed_orders(signed_bodies, account_id);

        int success_count = 0;
        for (const auto& r : results) {
            if (r.success) success_count++;
        }

        std::cout << "[batch] batch_size=" << batch_size
                  << " success=" << success_count << "/" << batch_size;
        if (!results.empty() && !results[0].error_msg.empty()) {
            std::cout << " error=" << results[0].error_msg;
        }
        std::cout << std::endl;

        if (success_count == batch_size) {
            max_working_batch = batch_size;
        }

        // Cancel all orders (cleanup)
        if (success_count > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            auto cancel_results =
                client.cancel_orders(order_ids, account_id);
            int cancel_ok = 0;
            for (const auto& cr : cancel_results) {
                if (cr.success) cancel_ok++;
            }
            std::cout << "[batch] cleanup: canceled " << cancel_ok << "/"
                      << batch_size << std::endl;
        }

        // Cooldown between batch sizes to avoid rate limiting
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    std::cout << "\n[batch] === Results ===" << std::endl;
    std::cout << "[batch] Max working batch size: " << max_working_batch
              << std::endl;

    // At minimum, batch size of 1 should work
    REQUIRE(max_working_batch >= 1);
}

// ──────────────────────────────────────────────────────────────────────────
// E2E test: verify that HTTP error details are propagated in results.
// Submits a batch of 20 orders (likely to fail) and checks that
// error_msg is populated (not empty) when success=false.
// ──────────────────────────────────────────────────────────────────────────

TEST_CASE("Polymarket E2E: error propagation on failed batch",
          "[polymarket][e2e][live]") {
    const std::string PROXY_BASE = "http://localhost:8000/api/v1/ccxt";
    const std::string GAMMA_API = "https://gamma-api.polymarket.com";
    const std::string CLOB_API = "https://clob.polymarket.com";

    // ── Fetch credentials ────────────────────────────────────────────────

    auto [cred_code, cred_body] =
        simple_http_get(PROXY_BASE + "/get-credentials");
    REQUIRE(cred_code == 200);

    auto cred_json = nlohmann::json::parse(cred_body);
    auto it = cred_json["accounts"].begin();
    long long account_id = std::stoll(it.key());
    auto& acct = it.value();

    auto privkey = Secp256k1Signer::parse_private_key(
        acct["private_key"].get<std::string>());

    PolymarketDirect::PolymarketClient client;
    PolymarketDirect::AccountCredentials creds;
    creds.private_key = privkey;
    creds.api_key = acct["api_key"].get<std::string>();
    creds.api_secret = acct["api_secret"].get<std::string>();
    creds.api_passphrase = acct["api_passphrase"].get<std::string>();
    creds.maker_address = acct["maker_address"].get<std::string>();
    creds.signer_address = acct["signer_address"].get<std::string>();

    if (acct.contains("builder_api_key") &&
        !acct["builder_api_key"].get<std::string>().empty()) {
        creds.builder_api_key = acct["builder_api_key"].get<std::string>();
        creds.builder_secret = acct["builder_secret"].get<std::string>();
        creds.builder_passphrase =
            acct["builder_passphrase"].get<std::string>();
    }

    client.add_account(account_id, creds);

    // ── Resolve market ───────────────────────────────────────────────────

    std::string slug = "china-x-taiwan-military-clash-before-2027";
    auto [gamma_code, gamma_body] =
        simple_http_get(GAMMA_API + "/events/slug/" + slug);

    std::string token_id;
    if (gamma_code == 200) {
        auto gamma_json = nlohmann::json::parse(gamma_body);
        auto token_ids_json = nlohmann::json::parse(
            gamma_json["markets"][0]["clobTokenIds"].get<std::string>());
        token_id = token_ids_json[0].get<std::string>();
    } else {
        auto [rc, rb] =
            simple_http_get(PROXY_BASE + "/resolve-market?slug=" + slug);
        REQUIRE(rc == 200);
        auto rj = nlohmann::json::parse(rb);
        token_id = rj["tokens"][0]["token_id"].get<std::string>();
    }

    std::string tick_size = "0.01";
    {
        auto [tc, tb] =
            simple_http_get(CLOB_API + "/tick-size?token_id=" + token_id);
        if (tc == 200) {
            auto j = nlohmann::json::parse(tb);
            if (j.contains("minimum_tick_size")) {
                auto& val = j["minimum_tick_size"];
                if (val.is_string()) {
                    tick_size = val.get<std::string>();
                } else {
                    tick_size = std::to_string(val.get<double>());
                    tick_size.erase(tick_size.find_last_not_of('0') + 1);
                    if (tick_size.back() == '.') tick_size.pop_back();
                }
            }
        }
    }

    double tick_val = std::stod(tick_size);

    std::cout << "\n[errprop] Error propagation test, token=" << token_id
              << std::endl;

    // ── Build 20 orders (likely above batch limit) ───────────────────────

    const int BATCH_SIZE = 20;
    std::vector<Core::Order> orders;
    orders.reserve(BATCH_SIZE);

    for (int i = 0; i < BATCH_SIZE; i++) {
        Core::Order o;
        o.type = Core::OrderType::LIMIT;
        o.side = Core::OrderSide::BUY;
        std::string price = std::to_string(tick_val * (i + 1));
        price.erase(price.find_last_not_of('0') + 1);
        if (price.back() == '.') price.pop_back();
        o.price_str = price;
        o.quantity_str = "5";
        o.is_prediction_market_order = true;
        o.clob_id = token_id;
        o.account_id = account_id;
        o.post_only = false;
        o.server_sending_deadline_ms = 0;
        orders.push_back(o);
    }

    auto signed_bodies = client.sign_orders(orders);
    REQUIRE((int)signed_bodies.size() == BATCH_SIZE);

    auto results =
        client.submit_signed_orders(signed_bodies, account_id);

    REQUIRE((int)results.size() == BATCH_SIZE);

    // Check: if any order failed, its error_msg must NOT be empty
    int failures_with_error = 0;
    int failures_without_error = 0;
    int successes = 0;

    for (size_t i = 0; i < results.size(); i++) {
        if (results[i].success) {
            successes++;
        } else if (!results[i].error_msg.empty()) {
            failures_with_error++;
        } else {
            failures_without_error++;
        }
    }

    std::cout << "[errprop] Results: successes=" << successes
              << " failures_with_error=" << failures_with_error
              << " failures_without_error=" << failures_without_error
              << std::endl;

    if (!results.empty() && !results[0].error_msg.empty()) {
        std::cout << "[errprop] First error: " << results[0].error_msg
                  << std::endl;
    }

    // The key assertion: no silent failures (success=false with empty error)
    CHECK(failures_without_error == 0);

    // Cleanup: cancel any orders that succeeded
    if (successes > 0) {
        std::vector<std::string> cleanup_ids;
        for (const auto& r : results) {
            if (r.success) cleanup_ids.push_back(r.order_id);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        client.cancel_orders(cleanup_ids, account_id);
    }

    std::cout << "[errprop] Done." << std::endl;
}

// ──────────────────────────────────────────────────────────────────────────
// E2E test: Query filled order immediately after match
// Verifies whether CLOB REST API returns correct size_matched immediately
// after an order is matched, or if there's a propagation delay.
// Guarded by POLYMARKET_FILL_TEST=1 env var to prevent accidental spending.
// ──────────────────────────────────────────────────────────────────────────

TEST_CASE("Query matched order fill propagation delay",
          "[polymarket][e2e][live]") {
    // Guard: require explicit opt-in via env var
    const char* fill_test_env = std::getenv("POLYMARKET_FILL_TEST");
    if (!fill_test_env || std::string(fill_test_env) != "1") {
        std::cout << "\n[fill] SKIPPED: set POLYMARKET_FILL_TEST=1 to run "
                     "(this test spends real money)\n"
                  << std::endl;
        return;
    }

    const std::string PROXY_BASE = "http://localhost:8000/api/v1/ccxt";
    const std::string GAMMA_API = "https://gamma-api.polymarket.com";
    const std::string CLOB_API = "https://clob.polymarket.com";

    // ── Step 1: Fetch credentials ─────────────────────────────────────────

    std::cout << "\n[fill] Fetching credentials..." << std::endl;

    auto [cred_code, cred_body] =
        simple_http_get(PROXY_BASE + "/get-credentials");
    REQUIRE(cred_code == 200);

    auto cred_json = nlohmann::json::parse(cred_body);
    REQUIRE(cred_json.contains("accounts"));
    REQUIRE(!cred_json["accounts"].empty());

    auto it = cred_json["accounts"].begin();
    long long account_id = std::stoll(it.key());
    auto& acct = it.value();

    auto privkey = Secp256k1Signer::parse_private_key(
        acct["private_key"].get<std::string>());

    PolymarketDirect::PolymarketClient client;
    PolymarketDirect::AccountCredentials creds;
    creds.private_key = privkey;
    creds.api_key = acct["api_key"].get<std::string>();
    creds.api_secret = acct["api_secret"].get<std::string>();
    creds.api_passphrase = acct["api_passphrase"].get<std::string>();
    creds.maker_address = acct["maker_address"].get<std::string>();
    creds.signer_address = acct["signer_address"].get<std::string>();

    if (acct.contains("builder_api_key") &&
        !acct["builder_api_key"].get<std::string>().empty()) {
        creds.builder_api_key = acct["builder_api_key"].get<std::string>();
        creds.builder_secret = acct["builder_secret"].get<std::string>();
        creds.builder_passphrase =
            acct["builder_passphrase"].get<std::string>();
    }

    client.add_account(account_id, creds);

    // ── Step 2: Resolve Taiwan market ─────────────────────────────────────

    std::string slug = "china-x-taiwan-military-clash-before-2027";
    std::cout << "[fill] Resolving market: " << slug << std::endl;

    auto [gamma_code, gamma_body] =
        simple_http_get(GAMMA_API + "/events/slug/" + slug);

    std::string token_id;
    std::string condition_id;

    if (gamma_code == 200) {
        auto gamma_json = nlohmann::json::parse(gamma_body);
        REQUIRE(gamma_json.contains("markets"));
        REQUIRE(!gamma_json["markets"].empty());
        auto& market = gamma_json["markets"][0];
        condition_id = market["conditionId"].get<std::string>();
        auto token_ids_json = nlohmann::json::parse(
            market["clobTokenIds"].get<std::string>());
        REQUIRE(!token_ids_json.empty());
        token_id = token_ids_json[0].get<std::string>();
    } else {
        auto [rc, rb] =
            simple_http_get(PROXY_BASE + "/resolve-market?slug=" + slug);
        REQUIRE(rc == 200);
        auto rj = nlohmann::json::parse(rb);
        condition_id = rj["condition_id"].get<std::string>();
        token_id = rj["tokens"][0]["token_id"].get<std::string>();
    }

    std::cout << "[fill] token_id=" << token_id << std::endl;
    std::cout << "[fill] condition_id=" << condition_id << std::endl;

    // ── Step 3: Fetch best ask price from order book ──────────────────────

    auto [book_code, book_body] =
        simple_http_get(CLOB_API + "/book?token_id=" + token_id);
    REQUIRE(book_code == 200);

    auto book_json = nlohmann::json::parse(book_body);
    REQUIRE(book_json.contains("asks"));
    REQUIRE(!book_json["asks"].empty());

    std::string best_ask_price =
        book_json["asks"][0]["price"].get<std::string>();
    std::string best_ask_size =
        book_json["asks"][0]["size"].get<std::string>();

    std::cout << "[fill] Best ask: " << best_ask_price << " x "
              << best_ask_size << std::endl;

    // Sanity: don't buy if best ask is too expensive (> $0.99)
    double ask_price_val = std::stod(best_ask_price);
    REQUIRE(ask_price_val <= 0.99);

    // ── Step 4: Create a BUY order at best ask (should match immediately) ─

    Core::Order order;
    order.type = Core::OrderType::LIMIT;
    order.side = Core::OrderSide::BUY;
    order.price_str = best_ask_price;
    order.quantity_str = "5";
    order.is_prediction_market_order = true;
    order.clob_id = token_id;
    order.account_id = account_id;
    order.post_only = false;
    order.server_sending_deadline_ms = 0;

    std::cout << "[fill] Creating BUY order: " << order.quantity_str << " @ "
              << order.price_str << " (should match immediately)" << std::endl;

    auto t_create_start = std::chrono::steady_clock::now();
    auto result = client.create_order(order);
    auto t_create_end = std::chrono::steady_clock::now();

    REQUIRE(result.has_value());
    auto& resp = result.value();

    std::cout << "[fill] Create response: " << resp.dump(2) << std::endl;

    REQUIRE(resp.contains("orderID"));
    std::string order_id = resp["orderID"].get<std::string>();
    REQUIRE(!order_id.empty());

    std::string create_status =
        resp.contains("status") ? resp["status"].get<std::string>() : "N/A";

    auto create_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         t_create_end - t_create_start)
                         .count();
    std::cout << "[fill] Order created: id=" << order_id
              << " status=" << create_status << " (took " << create_ms
              << "ms)" << std::endl;

    // ── Step 5: Query the order at multiple delays ────────────────────────

    // Build the query URL
    std::string query_url =
        PROXY_BASE + "/query-order?exchange_name=polymarket&market_type=spot"
                     "&account_id=" +
        std::to_string(account_id) + "&symbol=" + condition_id +
        "&client_order_id=" + order_id;

    std::vector<int> delays_ms = {0, 50, 200, 1000};

    std::cout << "\n[fill] === Query results at different delays ===" << std::endl;
    std::cout << "[fill] " << std::left << std::setw(12) << "delay_ms"
              << std::setw(16) << "status" << std::setw(16) << "filled"
              << std::setw(16) << "amount" << "query_time_ms" << std::endl;
    std::cout << "[fill] " << std::string(70, '-') << std::endl;

    for (int delay : delays_ms) {
        if (delay > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }

        auto t_query_start = std::chrono::steady_clock::now();
        auto [q_code, q_body] = simple_http_get(query_url);
        auto t_query_end = std::chrono::steady_clock::now();

        auto query_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            t_query_end - t_query_start)
                            .count();

        if (q_code != 200) {
            std::cout << "[fill] " << std::left << std::setw(12) << delay
                      << "HTTP_ERROR (code=" << q_code << ")" << std::endl;
            continue;
        }

        auto q_json = nlohmann::json::parse(q_body);

        // Proxy wraps response in {"data": {...}} — unwrap if present
        auto& data = q_json.contains("data") ? q_json["data"] : q_json;

        auto get_str = [](const nlohmann::json& j,
                          const std::string& key) -> std::string {
            if (!j.contains(key) || j[key].is_null()) return "N/A";
            auto& v = j[key];
            if (v.is_string()) return v.get<std::string>();
            if (v.is_number_float()) {
                std::ostringstream oss;
                oss << std::fixed << std::setprecision(6) << v.get<double>();
                return oss.str();
            }
            if (v.is_number_integer()) return std::to_string(v.get<long long>());
            return v.dump();
        };

        std::string q_status = get_str(data, "status");
        std::string filled_str = get_str(data, "filled");
        std::string amount_str = get_str(data, "amount");

        std::cout << "[fill] " << std::left << std::setw(12) << delay
                  << std::setw(16) << q_status << std::setw(16)
                  << filled_str << std::setw(16) << amount_str << query_ms
                  << "ms" << std::endl;

        // Log full response on first query for debugging
        if (delay == 0) {
            std::cout << "[fill] Full query response (delay=0): "
                      << q_json.dump(2) << std::endl;
        }
    }

    std::cout << "\n[fill] === Summary ===" << std::endl;
    std::cout << "[fill] Create response status: " << create_status
              << std::endl;
    std::cout << "[fill] Order ID: " << order_id << std::endl;
    std::cout << "[fill] Expected fill qty: " << order.quantity_str
              << std::endl;
    std::cout << "[fill] If size_matched shows 0 at delay=0 but correct "
                 "value at later delays,"
              << std::endl;
    std::cout << "[fill] this confirms a propagation delay in the CLOB API "
                 "that explains the position leak."
              << std::endl;

    std::cout << "\n[fill] Done. (No cleanup needed — order was matched, "
                 "not resting.)"
              << std::endl;
}

// ──────────────────────────────────────────────────────────────────────────
// E2E test: sign → submit_signed_order (singular /order endpoint)
// Tagged [live] so it's excluded from normal test runs.
// Requires: Python proxy on localhost:8000, network access to polymarket.
// ──────────────────────────────────────────────────────────────────────────

TEST_CASE("Polymarket E2E: sign and submit single order via /order endpoint",
          "[polymarket][e2e][live]") {
    const std::string PROXY_BASE = "http://localhost:8000/api/v1/ccxt";
    const std::string GAMMA_API = "https://gamma-api.polymarket.com";
    const std::string CLOB_API = "https://clob.polymarket.com";

    // ── Step 1: Fetch credentials from Python proxy ──────────────────────

    std::cout << "\n[e2e-single] Fetching credentials from Python proxy..."
              << std::endl;

    auto [cred_code, cred_body] =
        simple_http_get(PROXY_BASE + "/get-credentials");
    REQUIRE(cred_code == 200);

    auto cred_json = nlohmann::json::parse(cred_body);
    REQUIRE(cred_json.contains("accounts"));
    REQUIRE(!cred_json["accounts"].empty());

    auto it = cred_json["accounts"].begin();
    long long account_id = std::stoll(it.key());
    auto& acct = it.value();

    auto privkey = Secp256k1Signer::parse_private_key(
        acct["private_key"].get<std::string>());

    PolymarketDirect::PolymarketClient client;
    PolymarketDirect::AccountCredentials creds;
    creds.private_key = privkey;
    creds.api_key = acct["api_key"].get<std::string>();
    creds.api_secret = acct["api_secret"].get<std::string>();
    creds.api_passphrase = acct["api_passphrase"].get<std::string>();
    creds.maker_address = acct["maker_address"].get<std::string>();
    creds.signer_address = acct["signer_address"].get<std::string>();

    if (acct.contains("builder_api_key") &&
        !acct["builder_api_key"].get<std::string>().empty()) {
        creds.builder_api_key = acct["builder_api_key"].get<std::string>();
        creds.builder_secret = acct["builder_secret"].get<std::string>();
        creds.builder_passphrase =
            acct["builder_passphrase"].get<std::string>();
    }

    client.add_account(account_id, creds);

    std::cout << "[e2e-single] Credentials loaded: signer="
              << creds.signer_address << std::endl;

    // ── Step 2: Resolve a safe test market ───────────────────────────────

    std::string slug = "china-x-taiwan-military-clash-before-2027";
    std::cout << "[e2e-single] Resolving market slug: " << slug << std::endl;

    auto [gamma_code, gamma_body] =
        simple_http_get(GAMMA_API + "/events/slug/" + slug);

    std::string token_id;
    std::string condition_id;
    bool neg_risk = false;

    if (gamma_code == 200) {
        auto gamma_json = nlohmann::json::parse(gamma_body);
        REQUIRE(gamma_json.contains("markets"));
        REQUIRE(!gamma_json["markets"].empty());

        auto& market = gamma_json["markets"][0];
        condition_id = market["conditionId"].get<std::string>();
        neg_risk = market.value("negRisk", false);

        REQUIRE(market.contains("clobTokenIds"));
        auto token_ids_str = market["clobTokenIds"].get<std::string>();
        auto token_ids_json = nlohmann::json::parse(token_ids_str);
        REQUIRE(!token_ids_json.empty());
        token_id = token_ids_json[0].get<std::string>();
    } else {
        std::cout << "[e2e-single] Gamma API failed (code=" << gamma_code
                  << "), trying proxy resolve-market..." << std::endl;

        auto [resolve_code, resolve_body] =
            simple_http_get(PROXY_BASE + "/resolve-market?slug=" + slug);
        REQUIRE(resolve_code == 200);

        auto resolve_json = nlohmann::json::parse(resolve_body);
        condition_id = resolve_json["condition_id"].get<std::string>();
        neg_risk = resolve_json["neg_risk"].get<bool>();

        REQUIRE(resolve_json.contains("tokens"));
        REQUIRE(!resolve_json["tokens"].empty());
        token_id = resolve_json["tokens"][0]["token_id"].get<std::string>();
    }

    std::cout << "[e2e-single] Market resolved: token_id=" << token_id
              << std::endl;

    // ── Step 3: Build a safe resting BUY order at minimum price ─────────

    Core::Order order;
    order.type = Core::OrderType::LIMIT;
    order.side = Core::OrderSide::BUY;
    order.price_str = "0.01";
    order.quantity_str = "5";
    order.is_prediction_market_order = true;
    order.clob_id = token_id;
    order.account_id = account_id;
    order.post_only = false;
    order.server_sending_deadline_ms = 0;

    std::cout << "[e2e-single] Order: BUY " << order.quantity_str << " @ "
              << order.price_str << std::endl;

    // ── Step 4: Sign the order ──────────────────────────────────────────

    auto signed_bodies = client.sign_orders({order});
    REQUIRE(signed_bodies.size() == 1);
    REQUIRE(!signed_bodies[0].order_id.empty());

    std::cout << "[e2e-single] Signed order_id=" << signed_bodies[0].order_id
              << std::endl;

    // ── Step 5: Submit via submit_signed_order (singular /order) ────────

    auto result =
        client.submit_signed_order(signed_bodies[0], account_id);

    std::cout << "[e2e-single] Result: success=" << result.success
              << " order_id=" << result.order_id
              << " status=" << result.status
              << " error=" << result.error_msg << std::endl;

    REQUIRE(result.success);
    REQUIRE(!result.order_id.empty());
    REQUIRE(result.order_id == signed_bodies[0].order_id);

    // ── Step 6: Cancel the order to clean up ────────────────────────────

    std::cout << "[e2e-single] Canceling order: " << result.order_id
              << std::endl;

    auto cancel_results = client.cancel_orders({result.order_id}, account_id);
    REQUIRE(cancel_results.size() == 1);
    REQUIRE(cancel_results[0].success);

    std::cout << "[e2e-single] Order canceled successfully." << std::endl;
}

TEST_CASE("Polymarket E2E: session heartbeat (POST /heartbeats)",
          "[polymarket][e2e][live][heartbeat]") {
    const std::string PROXY_BASE = "http://localhost:8000/api/v1/ccxt";

    auto [cred_code, cred_body] =
        simple_http_get(PROXY_BASE + "/get-credentials");
    REQUIRE(cred_code == 200);
    auto cred_json = nlohmann::json::parse(cred_body);
    REQUIRE(cred_json.contains("accounts"));
    REQUIRE(!cred_json["accounts"].empty());

    auto it = cred_json["accounts"].begin();
    long long account_id = std::stoll(it.key());
    auto& acct = it.value();

    // Heartbeat uses L2 auth only — no EIP-712 / private key needed.
    PolymarketDirect::PolymarketClient client;
    PolymarketDirect::AccountCredentials creds;
    creds.api_key = acct["api_key"].get<std::string>();
    creds.api_secret = acct["api_secret"].get<std::string>();
    creds.api_passphrase = acct["api_passphrase"].get<std::string>();
    creds.signer_address = acct["signer_address"].get<std::string>();
    client.add_account(account_id, creds);

    std::cout << "[e2e-hb] POST /heartbeats for account " << account_id
              << std::endl;
    bool ok = client.send_heartbeat(account_id);
    std::cout << "[e2e-hb] result: " << (ok ? "OK (2xx)" : "FAILED")
              << std::endl;
    REQUIRE(ok);
}
