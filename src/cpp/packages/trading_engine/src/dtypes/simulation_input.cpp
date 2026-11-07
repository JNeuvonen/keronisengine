#include "market_making/dtypes/simulation_input.h"

nlohmann::json Core::SimulationInput::to_json() const {
    nlohmann::json json;
    json["market_data"] = this->market_data.to_json();
    json["simulation_start_date_ms"] = this->simulation_start_date_ms;
    json["simulation_end_date_ms"] = this->simulation_end_date_ms;
    json["market_data_reading_offset_ms"] = this->market_data_reading_offset_ms;
    json["num_required_klines_on_datasets"] =
        this->num_required_klines_on_datasets;
    json["fee_rate_coeff"] = this->fee_rate_coeff;

    json["exchanges"] = nlohmann::json::array();
    for (const auto &exchange : this->exchanges) {
        json["exchanges"].push_back(exchange.to_json());
    }

    json["is_strategy_development"] = this->is_strategy_development;
    json["emit_trading_logs"] = this->emit_trading_logs;
    json["ohlcv_delay_ms"] = this->ohlcv_delay_ms;
    json["swap_leverage"] = this->swap_leverage;
    json["aggregate_klines_from_trades"] = this->aggregate_klines_from_trades;
    json["generate_predictions_from_klines"] =
        this->generate_predictions_from_klines;
    json["show_realtime_dashboard"] = this->show_realtime_dashboard;
    json["skip_reading_data_with_offset_on_trades"] =
        this->skip_reading_data_with_offset_on_trades;
    json["use_trade_price_for_fills"] = this->use_trade_price_for_fills;
    json["ack_cancels_at_exchange_removal"] =
        this->ack_cancels_at_exchange_removal;
    return json;
}