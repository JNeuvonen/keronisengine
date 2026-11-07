#include "polymarket_trade_tick.h"

#include <arrow/array.h>
#include <arrow/table.h>
#include <arrow/type.h>

std::vector<PolymarketTradeTick> PolymarketTradeTick::from_arrow_table(
    const std::shared_ptr<arrow::Table>& table) {
    if (!table) return {};

    std::vector<PolymarketTradeTick> ticks;
    int64_t num_rows = table->num_rows();
    ticks.reserve(num_rows);

    auto get_large_str = [&](const std::string& name) {
        return std::static_pointer_cast<arrow::LargeStringArray>(
            table->GetColumnByName(name)->chunk(0));
    };
    auto get_f64 = [&](const std::string& name) {
        return std::static_pointer_cast<arrow::DoubleArray>(
            table->GetColumnByName(name)->chunk(0));
    };
    auto get_i64 = [&](const std::string& name) {
        return std::static_pointer_cast<arrow::Int64Array>(
            table->GetColumnByName(name)->chunk(0));
    };

    auto side_arr = get_large_str("side");
    auto condition_id_arr = get_large_str("conditionId");
    auto size_arr = get_f64("size");
    auto price_arr = get_f64("price");
    auto timestamp_arr = get_i64("timestamp");
    auto slug_arr = get_large_str("slug");
    auto outcome_arr = get_large_str("outcome");
    auto outcome_index_arr = get_i64("outcomeIndex");

    const auto* size_raw = size_arr->raw_values();
    const auto* price_raw = price_arr->raw_values();
    const auto* timestamp_raw = timestamp_arr->raw_values();
    const auto* outcome_index_raw = outcome_index_arr->raw_values();

    for (int64_t i = 0; i < num_rows; ++i) {
        PolymarketTradeTick tick;
        tick.side = side_arr->GetString(i);
        tick.condition_id = condition_id_arr->GetString(i);
        tick.size = size_raw[i];
        tick.price = price_raw[i];
        tick.timestamp = timestamp_raw[i];
        tick.slug = slug_arr->GetString(i);
        tick.outcome = outcome_arr->GetString(i);
        tick.outcome_index = static_cast<int>(outcome_index_raw[i]);
        ticks.push_back(std::move(tick));
    }

    return ticks;
}
