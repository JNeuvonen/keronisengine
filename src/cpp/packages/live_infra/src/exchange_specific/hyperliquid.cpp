#include "live_infra/exchange_specific/hyperliquid.h"

#include <cstdlib>

#include "ccxt_proxy.h"
#include "ccxt_utils.h"
#include "common_types/log_event_types.h"
#include "disk_logging/log.h"
#include "live_infra/account_stream_manager.h"
#include "live_infra/config.h"
#include "market_making/dtypes/constants.h"
#include "market_making/dtypes/order_res.h"
#include "nlohmann/json.hpp"
#include "trading/hyperliquid_client.h"
#include "ws_stream.h"

namespace Live::HyperliquidExchange {

namespace {

bool use_testnet() {
    const char* env = std::getenv("HYPERLIQUID_USE_TESTNET");
    return env && std::string(env) == "1";
}

std::string ws_base_url() {
    const char* env = std::getenv("HYPERLIQUID_WS_BASE_URL");
    if (env) return std::string(env);
    return use_testnet() ? "wss://api.hyperliquid-testnet.xyz/ws"
                         : "wss://api.hyperliquid.xyz/ws";
}

AccountStreamConfig build_stream_config(long long account_id,
                                        const std::string& address) {
    AccountStreamConfig stream_config;
    stream_config.exchange_name = "hyperliquid";
    stream_config.market_type = "swap";
    stream_config.account_id = account_id;

    // Subscription-auth: no listen key. build_ws_url ignores the key arg.
    const std::string url = ws_base_url();
    stream_config.build_ws_url = [url](const std::string&) { return url; };

    stream_config.on_connect_messages = [address]() {
        std::vector<std::string> msgs;
        nlohmann::json sub;
        sub["method"] = "subscribe";
        sub["subscription"] = {{"type", "orderUpdates"}, {"user", address}};
        msgs.push_back(sub.dump());
        return msgs;
    };

    stream_config.parse_order_update = [](const std::string& msg) {
        return parse_order_trade_update(msg);
    };
    return stream_config;
}

void register_account_streams() {
    auto* client = CcxtProxy::instance().get_hyperliquid_client();
    if (!client) return;

    for (const auto& [account_id, access_and_secret] :
         Config::getInstance().hyperliquid_futures_accounts) {
        (void)access_and_secret;
        auto address = client->account_address(account_id);
        if (!address.has_value()) continue;
        AccountStreamManager::getInstance().add_account(
            build_stream_config(account_id, address.value()));
    }
}

}  // namespace

std::vector<Core::OrderResNormalized> parse_order_trade_update(
    const std::string& msg) {
    auto j = nlohmann::json::parse(msg, nullptr, false);
    if (j.is_discarded()) return {};
    if (j.value("channel", "") != "orderUpdates") return {};
    if (!j.contains("data") || !j["data"].is_array()) return {};

    std::vector<Core::OrderResNormalized> out;
    for (const auto& wo : j["data"]) {
        if (!wo.contains("order")) continue;
        const auto& o = wo["order"];

        Core::OrderResNormalized res;
        res.symbol = CCXTUtils::hyperliquid_coin_to_symbol(o.value("coin", ""));
        res.exchange_order_id = std::to_string(o.value("oid", 0LL));
        if (o.contains("cloid") && o["cloid"].is_string())
            res.client_order_id = o["cloid"].get<std::string>();

        std::string st = wo.value("status", "");
        if (st == "open" || st == "triggered") {
            res.status = "open";
        } else if (st == "filled") {
            res.status = Core::ORDER_STATUS_NOT_WORKING;
        } else if (st.find("anceled") != std::string::npos ||
                   st.find("ejected") != std::string::npos) {
            res.status = "canceled";
        } else {
            res.status = st;
        }

        res.price = mpfr::mpreal(o.value("limitPx", "0"));
        mpfr::mpreal orig_sz(o.value("origSz", "0"));
        mpfr::mpreal rem_sz(o.value("sz", "0"));
        res.orig_quantity = orig_sz;
        res.filled_quantity = orig_sz - rem_sz;  // cumulative filled
        res.timestamp = o.value("timestamp", 0LL);
        out.push_back(res);
    }
    return out;
}

void init_and_register_account_streams(
    std::function<void(const Core::OrderResNormalized&)> on_order_update) {
    LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_STARTUP_PHASE,
             "Initializing hyperliquid direct signing", nlohmann::json{});
    CcxtProxy::instance().init_hyperliquid_direct_signing();
    if (CcxtProxy::instance().is_hyperliquid_direct_signing_enabled()) {
        LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_STARTUP_PHASE,
                 "Hyperliquid direct signing ENABLED", nlohmann::json{});
    }

    register_account_streams();

    AccountStreamManager::getInstance().set_order_update_callback(
        std::move(on_order_update));
    AccountStreamManager::getInstance().start();
}

}  // namespace Live::HyperliquidExchange
