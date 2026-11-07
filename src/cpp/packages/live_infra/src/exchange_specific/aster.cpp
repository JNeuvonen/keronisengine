#include "live_infra/exchange_specific/aster.h"

#include <cstdlib>

#include "ccxt_proxy.h"
#include "common_types/log_event_types.h"
#include "disk_logging/log.h"
#include "live_infra/account_stream_manager.h"
#include "live_infra/config.h"
#include "market_making/dtypes/constants.h"
#include "market_making/dtypes/order_res.h"
#include "market_making/engine.h"
#include "nlohmann/json.hpp"
#include "trading/aster_client.h"
#include "ws_stream.h"

namespace Live::AsterExchange {

namespace {

AccountStreamConfig build_stream_config(AsterDirect::AsterClient* aster_client,
                                        long long account_id) {
    AccountStreamConfig stream_config;
    stream_config.exchange_name = "aster";
    stream_config.market_type = "swap";
    stream_config.account_id = account_id;
    stream_config.key_refresh_interval_s = 25 * 60;

    stream_config.fetch_stream_key = [aster_client, account_id]() {
        return aster_client->create_listen_key(account_id);
    };
    stream_config.renew_stream_key = [aster_client, account_id](
                                         const std::string& key) {
        aster_client->renew_listen_key(account_id, key);
    };

    const char* ws_base_env = std::getenv("ASTER_WS_BASE_URL");
    const std::string aster_ws_base =
        ws_base_env ? std::string(ws_base_env)
                    : "wss://fstream.asterdex.com/ws";
    stream_config.build_ws_url = [aster_ws_base](const std::string& key) {
        return aster_ws_base + "/" + key;
    };
    stream_config.parse_order_update = [](const std::string& msg) {
        return parse_order_trade_update(msg);
    };
    return stream_config;
}

void register_account_streams(AsterDirect::AsterClient* aster_client) {
    if (!aster_client) return;

    for (const auto& [account_id, access_and_secret] :
         Config::getInstance().aster_futures_accounts) {
        (void)access_and_secret;
        AccountStreamManager::getInstance().add_account(
            build_stream_config(aster_client, account_id));
    }
}

}  // namespace

std::vector<Core::OrderResNormalized> parse_order_trade_update(
    const std::string& msg) {
    auto j = nlohmann::json::parse(msg, nullptr, false);
    if (j.is_discarded()) return {};
    if (j.value("e", "") != "ORDER_TRADE_UPDATE") return {};
    if (!j.contains("o")) return {};
    const auto& o = j["o"];

    Core::OrderResNormalized res;
    res.symbol = o.value("s", "");
    res.exchange_order_id = std::to_string(o.value("i", 0LL));
    res.client_order_id = o.value("c", "");
    std::string st = o.value("X", "");
    if (st == "NEW" || st == "PARTIALLY_FILLED")
        res.status = "open";
    else if (st == "FILLED")
        res.status = Core::ORDER_STATUS_NOT_WORKING;
    else if (st == "CANCELED" || st == "EXPIRED")
        res.status = "canceled";
    else
        res.status = st;
    std::string avg = o.value("ap", "0");
    std::string px = o.value("p", "0");
    res.price = mpfr::mpreal(avg != "0" ? avg : px);
    res.orig_quantity = mpfr::mpreal(o.value("q", "0"));
    res.filled_quantity = mpfr::mpreal(o.value("z", "0"));
    res.timestamp = o.value("T", 0LL);
    return {res};
}

void init_and_register_account_streams(
    std::function<void(const Core::OrderResNormalized&)> on_order_update) {
    LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_STARTUP_PHASE,
             "Initializing aster direct signing", nlohmann::json{});
    CcxtProxy::instance().init_aster_direct_signing();
    if (CcxtProxy::instance().is_aster_direct_signing_enabled()) {
        LOG_INFO(Core::TRADING_LOG_EVENT_TYPE_LIVE_INFRA_STARTUP_PHASE,
                 "Aster direct signing ENABLED", nlohmann::json{});
    }

    register_account_streams(CcxtProxy::instance().get_aster_client());

    AccountStreamManager::getInstance().set_order_update_callback(
        std::move(on_order_update));
    AccountStreamManager::getInstance().start();
}

}  // namespace Live::AsterExchange
