#include "market_data/bars_livestreamer.h"

#include "common_types/log_event_types.h"
#include "disk_logging/log.h"
#include "market_data/bar_aggregator.h"
#include "market_data/bar_types_constant.h"
#include "market_data/config.h"
#include "market_data/ohlcv_dataset.h"
#include "market_data/trade_streams.h"
#include "market_data/utils.h"
#include "market_making/utils.h"

namespace MarketData {

BarsLivestreamer::BarsLivestreamer(
    const std::function<void(const std::string&)>& error_log_callback,
    const std::function<void(const std::string&)>& info_log_callback,
    const std::chrono::system_clock::time_point data_start_date)
    : error_log_callback(error_log_callback),
      info_log_callback(info_log_callback),
      data_start_date(data_start_date) {}

void BarsLivestreamer::init_state() {
    MarketData::BarAggregator& aggregator =
        MarketData::BarAggregator::getInstance();
    std::string market_data_library_root =
        MarketData::Config::getInstance().get_data_dir_root();

    for (const auto& exchange_and_market : DEFAULT_LIVE_STREAMED_EXCHANGES) {
        if (exchange_and_market.exchange_name == "polygon") continue;
        std::string key = Core::Utils::get_exchange_market_key(
            exchange_and_market.exchange_name, exchange_and_market.market_type);

        std::vector<std::string> unique_symbols =
            MarketData::get_unique_symbols_from_bar_dir(
                market_data_library_root, exchange_and_market.exchange_name,
                exchange_and_market.market_type);

        exchange_and_market_to_unique_symbols[key] = unique_symbols;
        exchange_and_market_util[key] = exchange_and_market;
    }

    const int NUM_INVIDUAL_STREAMS = 15;
    std::unordered_map<std::string, std::vector<MarketData::StreamsConfig>>
        streams_configs_map;

    int num_datasets_read = 0;

    for (const auto& [key, value] : exchange_and_market_to_unique_symbols) {
        std::vector<MarketData::StreamsConfig> streams_configs(
            NUM_INVIDUAL_STREAMS);
        Core::ExchangeAndMarket exchange_and_market =
            exchange_and_market_util[key];

        const std::string& str_key = Core::Utils::get_exchange_market_key(
            exchange_and_market.exchange_name, exchange_and_market.market_type);

        for (int i = 0; i < value.size(); i++) {
            const std::string& symbol = value[i];
            int group_index = i % NUM_INVIDUAL_STREAMS;

            MarketData::StreamInfo stream_info;
            stream_info.exchange_name = exchange_and_market.exchange_name;
            stream_info.market_type = exchange_and_market.market_type;
            stream_info.symbol = symbol;
            streams_configs[group_index].streams.push_back(stream_info);
        }
        streams_configs_map[str_key] = streams_configs;

        for (const auto& symbol : value) {
            for (const auto& bar_type : DEFAULT_LIVE_STREAMED_BARS) {
                MarketData::OhlcvBarMeta bar_metadata;
                bar_metadata.symbol = symbol;
                bar_metadata.exchange_name = exchange_and_market.exchange_name;
                bar_metadata.market_type = exchange_and_market.market_type;
                bar_metadata.bar_close_policy = bar_type;

                LOG_INFO(
                    Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_INIT_PHASE,
                    "Starting to read " + symbol + " from " +
                        ChronoUtils::chrono_timepoint_to_string(
                            data_start_date),
                    (nlohmann::json{
                        {"phase", "bars_livestreamer_read_start"},
                        {"symbol", symbol},
                        {"exchange_name", exchange_and_market.exchange_name},
                        {"market_type", exchange_and_market.market_type}}));

                auto dataset = std::make_shared<MarketData::OhlcvDataset>(
                    bar_metadata,
                    MarketData::get_dataset_read_start_time(
                        bar_metadata, data_start_date));
                aggregator.add_ohlcv_dataset(dataset);

                LOG_INFO(
                    Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_INIT_PHASE,
                    "Added " + symbol + " for " + bar_type.to_string(),
                    (nlohmann::json{
                        {"phase", "bars_livestreamer_added"},
                        {"symbol", symbol},
                        {"bar_type", bar_type.to_string()}}));
                num_datasets_read++;

                LOG_INFO(
                    Core::TRADING_LOG_EVENT_TYPE_MARKET_DATA_INIT_PHASE,
                    "Read " + std::to_string(num_datasets_read) + " datasets",
                    (nlohmann::json{
                        {"phase", "bars_livestreamer_read_count"},
                        {"datasets_read", num_datasets_read}}));
            }
        }
    }

    MarketData::TradeStreams& trade_streams =
        MarketData::TradeStreams::get_instance();

    trade_streams.set_log_callbacks(this->error_log_callback,
                                    this->info_log_callback);

    for (const auto& [key, value] : streams_configs_map) {
        for (const auto& streams_config : value) {
            trade_streams.add_config(streams_config);
        }
    }
    trade_streams.start_streams();
}

void BarsLivestreamer::start_stream() {
    if (is_initialized) {
        return;
    }
    init_state();
    is_initialized = true;
}

void BarsLivestreamer::disconnect_streams() {
    MarketData::TradeStreams& trade_streams =
        MarketData::TradeStreams::get_instance();
    trade_streams.stop_streams();
}
}  // namespace MarketData