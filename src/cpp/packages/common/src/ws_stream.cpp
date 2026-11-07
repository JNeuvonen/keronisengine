#include "ws_stream.h"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <random>

#include "ccxt_utils.h"
#include "market_data/config.h"
#include "market_data/utils.h"

bool WsStream::is_wss() const { return wsurl_.rfind("wss://", 0) == 0; }

WsStream::WsStream(const std::string& wsurl, MessageCallback message_callback,
                   const std::string& bind_ip, LogCallback info_callback,
                   LogCallback error_callback)
    : wsurl_(wsurl),
      message_callback_(message_callback),
      bind_ip_(bind_ip),
      is_disconnected_(true),
      is_initialized_(false),
      last_ping_received_(std::chrono::steady_clock::now()),
      last_ping_sent_(std::chrono::steady_clock::now()),
      info_log_(info_callback ? info_callback : [](const std::string&) {}),
      error_log_(error_callback ? error_callback : [](const std::string&) {}) {}

WsStream::~WsStream() { disconnect(); }

static std::string get_bybit_kline_sub_arg(const std::string& symbol,
                                           long long interval_ms) {
    int minutes = MarketData::convert_ms_to_num_of_minutes(interval_ms);
    return "kline." + std::to_string(minutes) + "." + symbol;
}

static std::string get_bitget_kline_sub_arg(const std::string& symbol,
                                            long long interval_ms) {
    return "candle" +
           MarketData::convert_interval_ms_to_bitget_str_format(interval_ms);
}

void WsStream::connect(const std::string& url) {
    wsurl_ = url;
    connect();
}

void WsStream::connect() {
    std::lock_guard<std::mutex> lock(ws_mutex);
    info_log_("connect() - Starting connect method");

    if (!is_disconnected_.load()) {
        return;
    }

    disconnect();

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    is_disconnected_.store(false);
    last_ping_received_.store(std::chrono::steady_clock::now());
    // Without this the staleness watchdog keeps measuring from the previous
    // connection, so it force-disconnects every new connection on its first
    // tick and the stream can never recover inside this process.
    last_msg_received_.store(std::chrono::steady_clock::now());

    if (is_wss()) {
        ws_client_ = std::make_unique<WsClientTls>();
    } else {
        ws_client_ = std::make_unique<WsClientPlain>();
    }

    std::visit(
        [this](auto& client_ptr) {
            setup_websocket();

            if constexpr (std::is_same_v<std::decay_t<decltype(*client_ptr)>,
                                         WsClientTls>) {
                setup_tls();
            }

            websocketpp::lib::error_code ec;
            auto con = client_ptr->get_connection(wsurl_, ec);
            if (ec) {
                error_log_("Failed to create connection: " + ec.message() +
                           " (url: " + wsurl_ + ")");
                return;
            }

            ws_hdl_ = con->get_handle();

            client_ptr->connect(con);

            is_initialized_.store(true);

            client_ptr->set_pong_timeout(60000);
            client_ptr->start_perpetual();
            ws_thread_ = std::thread(&WsStream::run, this);
        },
        ws_client_);
}

void WsStream::setup_websocket() {
    std::visit(
        [this](auto& client_ptr) {
            client_ptr->clear_access_channels(websocketpp::log::alevel::all);
            client_ptr->init_asio();

            if (!bind_ip_.empty()) {
                if constexpr (std::is_same_v<
                                  std::decay_t<decltype(*client_ptr)>,
                                  WsClientTls>) {
                    client_ptr->set_socket_init_handler(
                        [this](websocketpp::connection_hdl,
                               asio::ssl::stream<asio::ip::tcp::socket>& s) {
                            try {
                                asio::ip::address bind_address =
                                    asio::ip::address::from_string(bind_ip_);
                                asio::ip::tcp::endpoint local_ep(bind_address,
                                                                 0);
                                s.lowest_layer().open(local_ep.protocol());
                                s.lowest_layer().set_option(
                                    asio::socket_base::reuse_address(false));
                                s.lowest_layer().bind(local_ep);
                            } catch (const std::exception& e) {
                                error_log_("Socket binding failed: " +
                                           std::string(e.what()));
                                throw;
                            }
                        });
                } else {
                    client_ptr->set_socket_init_handler(
                        [this](websocketpp::connection_hdl,
                               asio::ip::tcp::socket& s) {
                            try {
                                asio::ip::address bind_address =
                                    asio::ip::address::from_string(bind_ip_);
                                asio::ip::tcp::endpoint local_ep(bind_address,
                                                                 0);
                                s.open(local_ep.protocol());
                                s.set_option(
                                    asio::socket_base::reuse_address(false));
                                s.bind(local_ep);
                            } catch (const std::exception& e) {
                                error_log_("Socket binding failed: " +
                                           std::string(e.what()));
                                throw;
                            }
                        });
                }
            }

            client_ptr->set_close_handler([this](
                                              websocketpp::connection_hdl hdl) {
                std::visit(
                    [hdl, this](auto& client_ptr_inner) {
                        auto con = client_ptr_inner->get_con_from_hdl(hdl);
                        const int code = con->get_remote_close_code();
                        if (con) {
                            info_log_("Connection closed from: " +
                                      this->wsurl_ + " with reason: " +
                                      con->get_remote_close_reason() +
                                      " (code: " + std::to_string(code) + ")");
                            is_disconnected_.store(true);
                        }
                    },
                    ws_client_);
            });

            client_ptr->set_fail_handler(
                [this](websocketpp::connection_hdl hdl) {
                    is_disconnected_.store(true);
                    std::visit(
                        [hdl, this](auto& client_ptr_inner) {
                            auto con = client_ptr_inner->get_con_from_hdl(hdl);
                            if (con) {
                                error_log_("Connection failed: " +
                                           con->get_ec().message() +
                                           " (url: " + wsurl_ + ")");
                            }
                        },
                        ws_client_);
                });

            client_ptr->set_message_handler(
                [this](websocketpp::connection_hdl hdl, auto msg) {
                    last_ping_received_.store(std::chrono::steady_clock::now());
                    if (message_callback_) {
                        message_callback_(msg->get_payload());
                        last_msg_received_.store(
                            std::chrono::steady_clock::now());
                        msg_count_.fetch_add(1, std::memory_order_relaxed);
                    }
                });

            client_ptr->set_ping_handler([this](websocketpp::connection_hdl hdl,
                                                std::string payload) {
                try {
                    std::visit(
                        [hdl, &payload](auto& client_ptr_inner) {
                            client_ptr_inner->pong(hdl, payload);
                        },
                        ws_client_);
                    last_ping_received_.store(std::chrono::steady_clock::now());
                    is_disconnected_.store(false);
                    return true;
                } catch (const std::exception& e) {
                    error_log_("Error sending pong: " + std::string(e.what()));
                    return false;
                }
            });

            client_ptr->set_open_handler(
                [this](websocketpp::connection_hdl hdl) {
                    info_log_("Connection opened: " + wsurl_);
                    send_subscribe_messages();
                });
        },
        ws_client_);
}

void WsStream::send_subscribe_messages() {
    std::this_thread::sleep_for(std::chrono::seconds(1));

    for (const auto& msg : on_open_raw_messages_) {
        send_message(msg);
    }

    if (!exchange_name_.has_value() || !market_type_.has_value() ||
        stream_infos_.empty()) {
        info_log_("send_subscribe_messages: skipping - exchange_name=" +
                  (exchange_name_.has_value() ? exchange_name_.value()
                                              : "nullopt") +
                  " market_type=" +
                  (market_type_.has_value() ? market_type_.value()
                                            : "nullopt") +
                  " stream_infos_size=" +
                  std::to_string(stream_infos_.size()));
        return;
    }

    if (stream_type_ == WsStreamConstants::OHLCV_STREAM_TYPE) {
        send_ohlcv_subscribe_messages();
    } else if (stream_type_ == WsStreamConstants::LOB_STREAM_TYPE) {
        send_lob_subscribe_messages();
    } else if (stream_type_ == WsStreamConstants::TRADE_STREAM_TYPE) {
        send_trade_subscribe_messages();
    } else if (stream_type_ == WsStreamConstants::BOOK_TICKER_STREAM_TYPE) {
        send_book_ticker_subscribe_messages();
    }
}

void WsStream::send_ohlcv_subscribe_messages() {
    const std::string exchange_name = exchange_name_.value();
    const std::string market_type = market_type_.value();

    if (exchange_name == "binance") {
        std::vector<std::string> batch_params;
        const int BATCH_SIZE = 200;
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> distrib(1, 1000000);

        for (const auto& stream_info : stream_infos_) {
            if (!stream_info.interval.has_value()) {
                continue;
            }

            std::string symbol_lower = stream_info.symbol;
            std::transform(symbol_lower.begin(), symbol_lower.end(),
                           symbol_lower.begin(), ::tolower);

            std::string stream_name =
                symbol_lower + "@kline_" + stream_info.interval.value();
            batch_params.push_back(stream_name);

            if (batch_params.size() >= BATCH_SIZE) {
                nlohmann::json subscribe_msg;
                subscribe_msg["method"] = "SUBSCRIBE";
                subscribe_msg["params"] = batch_params;
                subscribe_msg["id"] = distrib(gen);
                send_message(subscribe_msg.dump());
                batch_params.clear();
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(100));
            }
        }

        if (!batch_params.empty()) {
            nlohmann::json subscribe_msg;
            subscribe_msg["method"] = "SUBSCRIBE";
            subscribe_msg["params"] = batch_params;
            subscribe_msg["id"] = distrib(gen);
            send_message(subscribe_msg.dump());
        }

        info_log_("Subscribed to " + std::to_string(stream_infos_.size()) +
                  " binance ohlcv streams");
        return;
    }

    if (exchange_name == "bybit") {
        std::vector<nlohmann::json> batch_args;
        const int BATCH_SIZE = this->market_type_ == "spot" ? 10 : 100;

        for (const auto& stream_info : stream_infos_) {
            if (stream_info.symbol.length() >= 4 &&
                stream_info.symbol.substr(stream_info.symbol.length() - 4) ==
                    "USDC") {
                continue;
            }

            long long interval_ms = MarketData::convert_interval_str_to_ms(
                stream_info.interval.value());
            if (interval_ms == -1) {
                continue;
            }

            batch_args.push_back(
                get_bybit_kline_sub_arg(stream_info.symbol, interval_ms));

            if (batch_args.size() >= BATCH_SIZE) {
                nlohmann::json subscribe_msg;
                subscribe_msg["op"] = "subscribe";
                subscribe_msg["req_id"] = "test";
                subscribe_msg["args"] = batch_args;
                send_message(subscribe_msg.dump());
                batch_args.clear();
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(100));
            }
        }

        if (!batch_args.empty()) {
            nlohmann::json subscribe_msg;
            subscribe_msg["op"] = "subscribe";
            subscribe_msg["req_id"] = "test";
            subscribe_msg["args"] = batch_args;
            send_message(subscribe_msg.dump());
        }
    }

    if (exchange_name == "bitget") {
        std::vector<nlohmann::json> batch_args;
        const int BATCH_SIZE = 50;

        for (const auto& stream_info : stream_infos_) {
            long long interval_ms = MarketData::convert_interval_str_to_ms(
                stream_info.interval.value());
            if (interval_ms == -1) {
                continue;
            }

            nlohmann::json arg;
            arg["instType"] =
                (market_type == "futures") ? "USDT-FUTURES" : "SPOT";
            arg["channel"] =
                "candle" + MarketData::convert_interval_ms_to_bitget_str_format(
                               interval_ms);
            arg["instId"] = stream_info.symbol;
            batch_args.push_back(arg);

            if (batch_args.size() >= BATCH_SIZE) {
                nlohmann::json subscribe_msg;
                subscribe_msg["op"] = "subscribe";
                subscribe_msg["args"] = batch_args;
                send_message(subscribe_msg.dump());
                batch_args.clear();
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(100));
            }
        }

        if (!batch_args.empty()) {
            nlohmann::json subscribe_msg;
            subscribe_msg["op"] = "subscribe";
            subscribe_msg["args"] = batch_args;
            send_message(subscribe_msg.dump());
        }
    }

    if (exchange_name == "polygon") {
        nlohmann::json auth_msg;
        auth_msg["action"] = "auth";
        auth_msg["params"] =
            MarketData::Config::getInstance().get_polygon_api_key();
        send_message(auth_msg.dump());

        std::this_thread::sleep_for(std::chrono::seconds(5));

        // One subscribe per channel: a channel the plan lacks is refused
        // on its own and leaves the others streaming.
        for (const auto& stream_info : stream_infos_) {
            nlohmann::json subscribe_msg;
            subscribe_msg["action"] = "subscribe";
            subscribe_msg["params"] = stream_info.symbol;
            send_message(subscribe_msg.dump());
        }
    }

    if (exchange_name == "gateio") {
        auto now = std::chrono::system_clock::now();
        auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                             now.time_since_epoch())
                             .count();

        for (const auto& stream_info : stream_infos_) {
            if (!stream_info.interval.has_value()) {
                continue;
            }

            std::string symbol = stream_info.symbol;
            if (symbol.length() > 4 &&
                symbol.substr(symbol.length() - 4) == "USDT" &&
                symbol.find('_') == std::string::npos) {
                symbol = symbol.substr(0, symbol.length() - 4) + "_USDT";
            }

            nlohmann::json subscribe_msg;
            subscribe_msg["time"] = timestamp;
            subscribe_msg["channel"] = "spot.candlesticks";
            subscribe_msg["event"] = "subscribe";
            subscribe_msg["payload"] =
                nlohmann::json::array({stream_info.interval.value(), symbol});

            send_message(subscribe_msg.dump());
        }
        info_log_("Subscribed to " + std::to_string(stream_infos_.size()) +
                  " gateio ohlcv streams");
    }
}

void WsStream::send_trade_subscribe_messages() {
    const std::string exchange_name = exchange_name_.value();
    const std::string market_type = market_type_.value();

    if (exchange_name == "binance") {
        std::vector<std::string> batch_params;
        const int BATCH_SIZE = 200;
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> distrib(1, 1000000);

        for (const auto& stream_info : stream_infos_) {
            std::string symbol_lower = stream_info.symbol;
            std::transform(symbol_lower.begin(), symbol_lower.end(),
                           symbol_lower.begin(), ::tolower);

            std::string stream_name = symbol_lower + "@trade";
            batch_params.push_back(stream_name);

            if (batch_params.size() >= BATCH_SIZE) {
                nlohmann::json subscribe_msg;
                subscribe_msg["method"] = "SUBSCRIBE";
                subscribe_msg["params"] = batch_params;
                subscribe_msg["id"] = distrib(gen);
                send_message(subscribe_msg.dump());
                batch_params.clear();
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(100));
            }
        }

        if (!batch_params.empty()) {
            nlohmann::json subscribe_msg;
            subscribe_msg["method"] = "SUBSCRIBE";
            subscribe_msg["params"] = batch_params;
            subscribe_msg["id"] = distrib(gen);
            send_message(subscribe_msg.dump());
        }

        info_log_("Subscribed to " + std::to_string(stream_infos_.size()) +
                  " binance trade streams");
        return;
    }

    if (exchange_name == "gateio") {
        auto now = std::chrono::system_clock::now();
        auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                             now.time_since_epoch())
                             .count();

        std::vector<std::string> symbols_batch;
        const int BATCH_SIZE = 50;

        for (const auto& stream_info : stream_infos_) {
            std::string symbol = stream_info.symbol;
            if (symbol.length() > 4 &&
                symbol.substr(symbol.length() - 4) == "USDT" &&
                symbol.find('_') == std::string::npos) {
                symbol = symbol.substr(0, symbol.length() - 4) + "_USDT";
            }

            symbols_batch.push_back(symbol);

            if (symbols_batch.size() >= BATCH_SIZE) {
                nlohmann::json subscribe_msg;
                subscribe_msg["time"] = timestamp;
                subscribe_msg["channel"] = "spot.trades";
                subscribe_msg["event"] = "subscribe";
                subscribe_msg["payload"] = symbols_batch;

                send_message(subscribe_msg.dump());
                symbols_batch.clear();
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(100));
            }
        }

        if (!symbols_batch.empty()) {
            nlohmann::json subscribe_msg;
            subscribe_msg["time"] = timestamp;
            subscribe_msg["channel"] = "spot.trades";
            subscribe_msg["event"] = "subscribe";
            subscribe_msg["payload"] = symbols_batch;

            send_message(subscribe_msg.dump());
        }

        info_log_("Subscribed to " + std::to_string(stream_infos_.size()) +
                  " gateio trade streams");
    }
}

void WsStream::send_lob_subscribe_messages() {
    const std::string exchange_name = exchange_name_.value();
    const std::string market_type = market_type_.value();

    if (exchange_name == "binance") {
        std::vector<std::string> batch_params;
        const int BATCH_SIZE = 200;
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> distrib(1, 1000000);

        for (const auto& stream_info : stream_infos_) {
            std::string symbol_lower = stream_info.symbol;
            std::transform(symbol_lower.begin(), symbol_lower.end(),
                           symbol_lower.begin(), ::tolower);

            std::string stream_name = symbol_lower + "@depth@100ms";
            batch_params.push_back(stream_name);

            if (batch_params.size() >= BATCH_SIZE) {
                nlohmann::json subscribe_msg;
                subscribe_msg["method"] = "SUBSCRIBE";
                subscribe_msg["params"] = batch_params;
                subscribe_msg["id"] = distrib(gen);
                send_message(subscribe_msg.dump());
                batch_params.clear();
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(100));
            }
        }

        if (!batch_params.empty()) {
            nlohmann::json subscribe_msg;
            subscribe_msg["method"] = "SUBSCRIBE";
            subscribe_msg["params"] = batch_params;
            subscribe_msg["id"] = distrib(gen);
            send_message(subscribe_msg.dump());
        }

        info_log_("Subscribed to " + std::to_string(stream_infos_.size()) +
                  " binance lob streams");
        return;
    }

    if (exchange_name == "gateio") {
        auto now = std::chrono::system_clock::now();
        auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                             now.time_since_epoch())
                             .count();

        for (const auto& stream_info : stream_infos_) {
            std::string symbol = stream_info.symbol;
            if (symbol.length() > 4 &&
                symbol.substr(symbol.length() - 4) == "USDT" &&
                symbol.find('_') == std::string::npos) {
                symbol = symbol.substr(0, symbol.length() - 4) + "_USDT";
            }

            nlohmann::json subscribe_msg;
            subscribe_msg["time"] = timestamp;
            subscribe_msg["channel"] = "spot.order_book_update";
            subscribe_msg["event"] = "subscribe";
            subscribe_msg["payload"] = nlohmann::json::array({symbol, "100ms"});

            send_message(subscribe_msg.dump());
        }
        info_log_("Subscribed to " + std::to_string(stream_infos_.size()) +
                  " gateio lob streams");
    }
}

void WsStream::send_book_ticker_subscribe_messages() {
    const std::string exchange_name = exchange_name_.value();
    const std::string market_type = market_type_.value();

    // Binance-compatible book ticker subscription (also works for Aster)
    if (exchange_name == "binance" || exchange_name == "aster") {
        std::vector<std::string> batch_params;
        const int BATCH_SIZE = 200;
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> distrib(1, 1000000);

        for (const auto& stream_info : stream_infos_) {
            std::string symbol_lower = stream_info.symbol;
            std::transform(symbol_lower.begin(), symbol_lower.end(),
                           symbol_lower.begin(), ::tolower);

            std::string stream_name = symbol_lower + "@bookTicker";
            batch_params.push_back(stream_name);

            if (batch_params.size() >= BATCH_SIZE) {
                nlohmann::json subscribe_msg;
                subscribe_msg["method"] = "SUBSCRIBE";
                subscribe_msg["params"] = batch_params;
                subscribe_msg["id"] = distrib(gen);
                send_message(subscribe_msg.dump());
                batch_params.clear();
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }

        if (!batch_params.empty()) {
            nlohmann::json subscribe_msg;
            subscribe_msg["method"] = "SUBSCRIBE";
            subscribe_msg["params"] = batch_params;
            subscribe_msg["id"] = distrib(gen);
            send_message(subscribe_msg.dump());
        }

        info_log_("Subscribed to " + std::to_string(stream_infos_.size()) +
                  " " + exchange_name + " book_ticker streams");
        return;
    }

    // Hyperliquid: subscription-based, one `bbo` subscription per coin. Symbols
    // are canonical (BTCUSDC); HL's WS wants the bare coin (BTC).
    if (exchange_name == "hyperliquid") {
        for (const auto& stream_info : stream_infos_) {
            nlohmann::json subscribe_msg;
            subscribe_msg["method"] = "subscribe";
            subscribe_msg["subscription"] = {
                {"type", "bbo"},
                {"coin",
                 CCXTUtils::hyperliquid_symbol_to_coin(stream_info.symbol)}};
            send_message(subscribe_msg.dump());
        }
        info_log_("Subscribed to " + std::to_string(stream_infos_.size()) +
                  " hyperliquid bbo streams");
        return;
    }
}

void WsStream::setup_tls() {
    std::visit(
        [this](auto& client_ptr) {
            if constexpr (std::is_same_v<std::decay_t<decltype(*client_ptr)>,
                                         WsClientTls>) {
                client_ptr->set_tls_init_handler(
                    [](websocketpp::connection_hdl) {
                        auto ctx =
                            websocketpp::lib::make_shared<asio::ssl::context>(
                                asio::ssl::context::tlsv12);

                        ctx->set_options(
                            asio::ssl::context::default_workarounds |
                            asio::ssl::context::no_sslv2 |
                            asio::ssl::context::no_sslv3 |
                            asio::ssl::context::single_dh_use);

                        return ctx;
                    });
            }
        },
        ws_client_);
}

void WsStream::disconnect() {
    std::visit(
        [this](auto& client_ptr) {
            if (client_ptr && ws_hdl_.lock()) {
                websocketpp::lib::error_code ec;
                client_ptr->close(ws_hdl_, websocketpp::close::status::normal,
                                  "", ec);
            }
            if (client_ptr) {
                client_ptr->stop_perpetual();
                client_ptr->stop();
            }
        },
        ws_client_);

    if (ws_thread_.joinable()) {
        ws_thread_.join();
    }

    std::visit(
        [this](auto& client_ptr) {
            if (client_ptr) {
                client_ptr.reset();
            }
        },
        ws_client_);
}

bool WsStream::is_disconnected() {
    std::lock_guard<std::mutex> lock(ws_mutex);
    if (!is_initialized_.load()) {
        return false;
    }

    if (is_disconnected_.load()) {
        return true;
    }

    if (std::chrono::steady_clock::now() - last_ping_received_.load() >
        std::chrono::seconds(60 * 15)) {
        is_disconnected_.store(true);
        return true;
    }

    return false;
}

std::chrono::steady_clock::time_point WsStream::get_last_msg_received() {
    return last_msg_received_.load();
}

void WsStream::hold_staleness_clock() {
    last_msg_received_.store(std::chrono::steady_clock::now());
}

long long WsStream::get_msg_count() const {
    return msg_count_.load(std::memory_order_relaxed);
}

void WsStream::run() {
    std::visit(
        [this](auto& client_ptr) {
            if (!client_ptr) {
                return;  // Exit if client_ptr is null
            }
            try {
                client_ptr->run();
            } catch (const std::exception& e) {
                error_log_("WebSocket run error: " + std::string(e.what()) +
                           " (url: " + wsurl_ + ")");
                is_disconnected_.store(true);
            }
        },
        ws_client_);
}

bool WsStream::send_message(const std::string& message) {
    bool sent = false;
    std::visit(
        [this, &message, &sent](auto& client_ptr) {
            if (!client_ptr || !ws_hdl_.lock() || is_disconnected_.load()) {
                error_log_("Cannot send message: connection not established");
                sent = false;
                return;
            }
            try {
                websocketpp::lib::error_code ec;
                client_ptr->send(ws_hdl_, message,
                                 websocketpp::frame::opcode::text, ec);
                if (ec) {
                    error_log_("Failed to send message: " + ec.message());
                    sent = false;
                } else {
                    sent = true;
                }
            } catch (const std::exception& e) {
                error_log_("Exception while sending message: " +
                           std::string(e.what()));
                sent = false;
            }
        },
        ws_client_);
    return sent;
}

void WsStream::send_ping() {
    std::lock_guard<std::mutex> lock(ws_mutex);
    if (!is_initialized_.load()) {
        return;
    }

    if (std::chrono::steady_clock::now() - last_ping_sent_.load() <
        std::chrono::seconds(15)) {
        return;
    }
    last_ping_sent_.store(std::chrono::steady_clock::now());

    if (exchange_name_ && *exchange_name_ == "bybit") {
        nlohmann::json ping_msg = {{"op", "ping"}};

        if (!send_message(ping_msg.dump())) {
            error_log_("Failed to send Bybit ping message");
        }
        return;
    }

    if (exchange_name_ && *exchange_name_ == "bitget") {
        if (!send_message("ping")) {
            error_log_("Failed to send Bitget ping message");
        }
        return;
    }

    if (exchange_name_ && *exchange_name_ == "gateio") {
        auto now = std::chrono::system_clock::now();
        auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                             now.time_since_epoch())
                             .count();
        nlohmann::json ping_msg;
        ping_msg["time"] = timestamp;
        ping_msg["channel"] = "spot.ping";
        if (!send_message(ping_msg.dump())) {
            error_log_("Failed to send Gateio ping message");
        }
        return;
    }

    std::visit(
        [this](auto& client_ptr) {
            if (!client_ptr || !ws_hdl_.lock() || is_disconnected_.load()) {
                error_log_("Cannot send ping: connection not established");
                return;
            }
            try {
                websocketpp::lib::error_code ec;
                client_ptr->ping(ws_hdl_, "", ec);
                if (ec) {
                    error_log_("Failed to send ping: " + ec.message() +
                               " (code: " + std::to_string(ec.value()) + ")" +
                               " base url: " + wsurl_);
                }
            } catch (const std::exception& e) {
                error_log_("Exception while sending ping: " +
                           std::string(e.what()));
            }
        },
        ws_client_);
}

void WsStream::set_stream_infos(
    const std::string& exchange_name, const std::string& market_type,
    const std::string& stream_type,
    const std::vector<MarketData::StreamInfo>& stream_infos) {
    this->exchange_name_ = exchange_name;
    this->market_type_ = market_type;
    this->stream_type_ = stream_type;
    this->stream_infos_ = stream_infos;
}

void WsStream::set_on_open_messages(const std::vector<std::string>& messages) {
    this->on_open_raw_messages_ = messages;
}
