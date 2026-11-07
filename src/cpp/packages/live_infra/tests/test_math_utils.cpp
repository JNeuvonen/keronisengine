#include <catch2/catch_all.hpp>
#include <cmath>
#include <iostream>

#include "market_making/utils.h"
#include "math_utils.h"
#include "strategies/common/utils.h"
#include "strategy_utils.h"

TEST_CASE("linear_mapping returns correct values", "[math_utils]") {
    SECTION("returns y1 when input equals x1") {
        double result = MathUtils::linear_mapping(0.0, 0.0, 10.0, 1.0, 20.0);
        REQUIRE(result == Catch::Approx(10.0));
    }

    SECTION("returns y2 when input equals x2") {
        double result = MathUtils::linear_mapping(1.0, 0.0, 10.0, 1.0, 20.0);
        REQUIRE(result == Catch::Approx(20.0));
    }

    SECTION("returns midpoint for midpoint input") {
        double result = MathUtils::linear_mapping(0.5, 0.0, 0.0, 1.0, 100.0);
        REQUIRE(result == Catch::Approx(50.0));
    }

    SECTION("clamps to y1 when input below x1 and clamp enabled") {
        double result =
            MathUtils::linear_mapping(-1.0, 0.0, 10.0, 1.0, 20.0, true);
        REQUIRE(result == Catch::Approx(10.0));
    }

    SECTION("clamps to y2 when input above x2 and clamp enabled") {
        double result =
            MathUtils::linear_mapping(2.0, 0.0, 10.0, 1.0, 20.0, true);
        REQUIRE(result == Catch::Approx(20.0));
    }

    SECTION("does not clamp when clamp disabled") {
        double result =
            MathUtils::linear_mapping(2.0, 0.0, 10.0, 1.0, 20.0, false);
        REQUIRE(result == Catch::Approx(30.0));
    }

    SECTION("handles inverse mapping (y1 > y2)") {
        double result = MathUtils::linear_mapping(0.5, 0.0, 100.0, 1.0, 0.0);
        REQUIRE(result == Catch::Approx(50.0));
    }

    SECTION("clamps correctly for inverse mapping") {
        double result =
            MathUtils::linear_mapping(2.0, 0.0, 100.0, 1.0, 0.0, true);
        REQUIRE(result == Catch::Approx(0.0));
    }
}

TEST_CASE("exponential_mapping returns correct values", "[math_utils]") {
    SECTION("returns y1 when input equals x1") {
        double result =
            MathUtils::exponential_mapping(0.0, 0.0, 0.0, 1.0, 100.0, 0.01);
        REQUIRE(result == Catch::Approx(0.0));
    }

    SECTION("approaches y2 as input approaches x2") {
        double result =
            MathUtils::exponential_mapping(1.0, 0.0, 0.0, 1.0, 100.0, 0.01);
        REQUIRE(result == Catch::Approx(99.0).margin(1.0));
    }

    SECTION("returns y2 when x1 equals x2") {
        double result =
            MathUtils::exponential_mapping(5.0, 1.0, 10.0, 1.0, 50.0, 0.01);
        REQUIRE(result == Catch::Approx(50.0));
    }

    SECTION("clamps to range when enabled") {
        double result = MathUtils::exponential_mapping(2.0, 0.0, 0.0, 1.0,
                                                       100.0, 0.01, true);
        REQUIRE(result == Catch::Approx(100.0));
    }

    SECTION("smaller epsilon reaches y2 faster") {
        double result_small =
            MathUtils::exponential_mapping(0.5, 0.0, 0.0, 1.0, 100.0, 0.001);
        double result_large =
            MathUtils::exponential_mapping(0.5, 0.0, 0.0, 1.0, 100.0, 0.1);
        REQUIRE(result_small > result_large);
    }
}

TEST_CASE("power_mapping returns correct values", "[math_utils]") {
    SECTION("returns y1 when input equals x1") {
        double result =
            MathUtils::power_mapping(0.0, 0.0, 0.0, 1.0, 100.0, 2.0, true);
        REQUIRE(result == Catch::Approx(0.0));
    }

    SECTION("returns y2 when input equals x2") {
        double result =
            MathUtils::power_mapping(1.0, 0.0, 0.0, 1.0, 100.0, 2.0, true);
        REQUIRE(result == Catch::Approx(100.0));
    }

    SECTION("power of 2 gives quadratic curve") {
        double result =
            MathUtils::power_mapping(0.5, 0.0, 0.0, 1.0, 100.0, 2.0, true);
        REQUIRE(result == Catch::Approx(25.0));
    }

    SECTION("power of 1 gives linear result") {
        double result =
            MathUtils::power_mapping(0.5, 0.0, 0.0, 1.0, 100.0, 1.0, true);
        REQUIRE(result == Catch::Approx(50.0));
    }

    SECTION("power of 0.5 gives square root curve") {
        double result =
            MathUtils::power_mapping(0.25, 0.0, 0.0, 1.0, 100.0, 0.5, true);
        REQUIRE(result == Catch::Approx(50.0));
    }

    SECTION("returns y2 when x1 equals x2") {
        double result =
            MathUtils::power_mapping(5.0, 1.0, 10.0, 1.0, 50.0, 2.0, true);
        REQUIRE(result == Catch::Approx(50.0));
    }

    SECTION("clamps below range") {
        double result =
            MathUtils::power_mapping(-1.0, 0.0, 0.0, 1.0, 100.0, 2.0, true);
        REQUIRE(result == Catch::Approx(0.0));
    }

    SECTION("clamps above range") {
        double result =
            MathUtils::power_mapping(2.0, 0.0, 0.0, 1.0, 100.0, 2.0, true);
        REQUIRE(result == Catch::Approx(100.0));
    }
}

TEST_CASE("power_mapping_with_start_ramp returns correct values",
          "[math_utils]") {
    SECTION("returns y1 when input below x1") {
        double result = MathUtils::power_mapping_with_start_ramp(
            -1.0, 0.0, 0.0, 1.0, 100.0, 2.0, 10.0, true);
        REQUIRE(result == Catch::Approx(0.0));
    }

    SECTION("returns y1 + y_start at x1") {
        double result = MathUtils::power_mapping_with_start_ramp(
            0.0, 0.0, 0.0, 1.0, 100.0, 2.0, 10.0, true);
        REQUIRE(result == Catch::Approx(10.0));
    }

    SECTION("returns y2 at x2") {
        double result = MathUtils::power_mapping_with_start_ramp(
            1.0, 0.0, 0.0, 1.0, 100.0, 2.0, 10.0, true);
        REQUIRE(result == Catch::Approx(100.0));
    }

    SECTION("follows power curve from start ramp") {
        double result = MathUtils::power_mapping_with_start_ramp(
            0.5, 0.0, 0.0, 1.0, 100.0, 2.0, 20.0, true);
        double expected =
            0.0 + 20.0 + (100.0 - 0.0 - 20.0) * std::pow(0.5, 2.0);
        REQUIRE(result == Catch::Approx(expected));
    }

    SECTION("clamps above range") {
        double result = MathUtils::power_mapping_with_start_ramp(
            2.0, 0.0, 0.0, 1.0, 100.0, 2.0, 10.0, true);
        REQUIRE(result == Catch::Approx(100.0));
    }

    SECTION("returns y2 when x1 equals x2") {
        double result = MathUtils::power_mapping_with_start_ramp(
            5.0, 1.0, 10.0, 1.0, 50.0, 2.0, 5.0, true);
        REQUIRE(result == Catch::Approx(50.0));
    }
}

TEST_CASE("randomize_number_between_range returns values in range",
          "[math_utils]") {
    SECTION("returns values within specified range") {
        for (int i = 0; i < 100; i++) {
            double result =
                MathUtils::randomize_number_between_range(10.0, 20.0);
            REQUIRE(result >= 10.0);
            REQUIRE(result <= 20.0);
        }
    }

    SECTION("works with negative ranges") {
        for (int i = 0; i < 100; i++) {
            double result =
                MathUtils::randomize_number_between_range(-50.0, -10.0);
            REQUIRE(result >= -50.0);
            REQUIRE(result <= -10.0);
        }
    }

    SECTION("works when min equals max") {
        double result = MathUtils::randomize_number_between_range(5.0, 5.0);
        REQUIRE(result == Catch::Approx(5.0));
    }
}

TEST_CASE("randomize_number_between_range_v2 returns values in range",
          "[math_utils]") {
    SECTION("returns double values within range") {
        for (int i = 0; i < 100; i++) {
            double result =
                MathUtils::randomize_number_between_range_v2<double>(0.0,
                                                                     100.0);
            REQUIRE(result >= 0.0);
            REQUIRE(result <= 100.0);
        }
    }

    SECTION("returns int values within range") {
        for (int i = 0; i < 100; i++) {
            int result =
                MathUtils::randomize_number_between_range_v2<int>(1, 10);
            REQUIRE(result >= 1);
            REQUIRE(result <= 10);
        }
    }

    SECTION("returns long long values within range") {
        for (int i = 0; i < 100; i++) {
            long long result =
                MathUtils::randomize_number_between_range_v2<long long>(1000LL,
                                                                        2000LL);
            REQUIRE(result >= 1000LL);
            REQUIRE(result <= 2000LL);
        }
    }
}

TEST_CASE("Test linear mapping", "[test_linear_mapping]") {
    const auto var_linear_mapping =
        MathUtils::linear_mapping(1.002, 1.001, 0, 1.0025, 1, true);

    const auto var_exponential_mapping =
        MathUtils::exponential_mapping(1.002, 1.001, 0, 1.0025, 1, 0.01, true);

    const auto var_power_mapping =
        MathUtils::power_mapping(1.002, 1.001, 0, 1.0025, 1, 2, true);

    const auto var_power_mapping_with_start =
        MathUtils::power_mapping_with_start_ramp(1.002, 1.001, 0, 1.0025, 1, 2,
                                                 0.1, true);

    const auto var_power_mapping_with_start_zero =
        MathUtils::power_mapping_with_start_ramp(1.0011, 1.001, 0, 1.0025, 1, 2,
                                                 0.1, true);

    std::cout << "var_linear_mapping: " << var_linear_mapping << std::endl;
    std::cout << "var_exponential_mapping: " << var_exponential_mapping
              << std::endl;
    std::cout << "var_power_mapping: " << var_power_mapping << std::endl;
    std::cout << "var_power_mapping_with_start: "
              << var_power_mapping_with_start << std::endl;
    std::cout << "var_power_mapping_with_start_zero: "
              << var_power_mapping_with_start_zero << std::endl;
}

TEST_CASE("test_linear_mapping_inverse", "[test_linear_mapping_inverse]") {
    const auto &scale_long_exposure_coeff =
        MathUtils::linear_mapping(0.4, 0.75, 1, 0.0, 10, true);

    std::cout << "scale_long_exposure_coeff: " << scale_long_exposure_coeff
              << std::endl;
}

TEST_CASE("test_is_trade_hold_duration_passed_rounded_klines",
          "[test_is_trade_hold_duration_passed_rounded_klines]") {
    const auto &is_trade_hold_duration_passed_rounded_klines =
        Core::is_trade_hold_duration_passed_rounded_klines(
            1752875128000, 1752879104000, 900000, 1000 * 60 * 60);

    std::cout << "is_trade_hold_duration_passed_rounded_klines: "
              << is_trade_hold_duration_passed_rounded_klines << std::endl;

    const auto &is_trade_hold_duration_passed_rounded_klines_2 =
        Core::is_trade_hold_duration_passed_rounded_klines(
            1752875128000, 1752879104000 - 900000, 900000, 1000 * 60 * 60);

    std::cout << "is_trade_hold_duration_passed_rounded_klines_2: "
              << is_trade_hold_duration_passed_rounded_klines_2 << std::endl;
}

TEST_CASE("test_randomize_number_between_range",
          "[test_randomize_number_between_range]") {
    const auto &randomized_number =
        MathUtils::randomize_number_between_range(100, 200);

    std::cout << "randomized_number: " << randomized_number << std::endl;
}

TEST_CASE("test_calculate_exchange_distributed_quantity",
          "[test_calculate_exchange_distributed_quantity]") {
    mpfr::mpreal price = 5.72;
    mpfr::mpreal max_quote_sizing_notional = 700;
    mpfr::mpreal sizing_coeff_delta = 1.0;
    mpfr::mpreal cumulative_quantity = 0;
    size_t num_exchanges = 4;

    while (sizing_coeff_delta > 0) {
        const auto &order_quantity_info =
            Core::Utils::calculate_exchange_distributed_quantity_info(
                max_quote_sizing_notional, 1.0, sizing_coeff_delta,
                num_exchanges, price.toDouble());

        sizing_coeff_delta -= order_quantity_info.sizing_coeff_delta;
        cumulative_quantity += order_quantity_info.quantity;

        std::cout << "cumulative_quantity: " << cumulative_quantity
                  << std::endl;
        std::cout << "sizing_coeff_delta: " << sizing_coeff_delta << std::endl;
    }

    std::cout << "Final cumulative_quantity: " << cumulative_quantity
              << std::endl;
    std::cout << "Expected cumulative_quantity: "
              << max_quote_sizing_notional / price.toDouble() << std::endl;

    const auto &order_quantity_info =
        Core::Utils::calculate_exchange_distributed_quantity_info(
            max_quote_sizing_notional, 0.5, 0.07, num_exchanges,
            price.toDouble());

    std::cout << "order_quantity_info.quantity: "
              << order_quantity_info.quantity << std::endl;
    std::cout << "order_quantity_info.sizing_coeff_delta: "
              << order_quantity_info.sizing_coeff_delta << std::endl;
}

TEST_CASE("calc_price_coeff_from_vol returns sensible values past max time",
          "[strategy_utils]") {
    const long long TRADE_MAX_TIME_MS = 1000 * 60 * 60 * 2;  // 2 hours
    const double volatility = 0.02;
    const double normalized_vol = volatility * 3;
    const double enter_price = 100.0;

    auto compute_effective_duration = [&](long long duration) {
        if (duration <= TRADE_MAX_TIME_MS) return duration;
        long long time_past_exit = duration - TRADE_MAX_TIME_MS;
        return TRADE_MAX_TIME_MS + time_past_exit * 2;
    };

    for (long long minutes = 0; minutes <= 300; minutes += 15) {
        long long duration = 1000 * 60 * minutes;
        long long effective = compute_effective_duration(duration);
        double time_remaining =
            1.0 - (static_cast<double>(effective) / TRADE_MAX_TIME_MS);
        double price_coeff =
            Core::Utils::calc_price_coeff_from_vol_and_trade_time_remaining(
                normalized_vol, time_remaining, true);
        double bid_price = enter_price * price_coeff;

        std::cout << "Duration: " << minutes << "min, "
                  << "price_coeff: " << price_coeff << ", "
                  << "bid_price: " << bid_price << std::endl;
    }
}

TEST_CASE("calc_price_coeff_from_simple_time_decay returns sensible values",
          "[strategy_utils]") {
    const long long TRADE_MAX_TIME_MS = 1000 * 60 * 60;  // 24 hours
    const double slope_coeff = 0.005;
    const double enter_price = 100.0;
    const bool is_bid = false;

    for (long long hours = 0; hours <= 24; hours += 1) {
        long long duration = 1000 * 60 * 60 * hours;
        double trade_time_elapsed_factor =
            static_cast<double>(duration) / TRADE_MAX_TIME_MS;
        double price_coeff =
            Core::Utils::calc_price_coeff_from_simple_time_decay(
                trade_time_elapsed_factor, slope_coeff, is_bid);
        double take_profit_price = enter_price * price_coeff;

        std::cout << "Duration: " << hours << "h, "
                  << "trade_time_elapsed_factor: " << trade_time_elapsed_factor
                  << ", " << "price_coeff: " << price_coeff << ", "
                  << "take_profit_price: " << take_profit_price << std::endl;
    }
}

TEST_CASE(
    "calc_price_coeff_v2 tiered multiplier computes correct effective duration",
    "[strategy_utils]") {
    const long long TRADE_MAX_TIME_MS = 1000LL * 60 * 60 * 4;  // 4 hours
    const double volatility = 0.02;
    const double normalized_vol = volatility * 3;

    SECTION("within first tier (0-4h) - multiplier 1") {
        long long duration_2h = 1000LL * 60 * 60 * 2;
        double coeff =
            Core::Utils::calc_price_coeff_from_vol_and_trade_time_remaining_v2(
                normalized_vol, duration_2h, TRADE_MAX_TIME_MS, true);
        double expected_time_factor = 1.0 - (2.0 / 4.0);
        double expected_coeff = 1.0 - normalized_vol * expected_time_factor;
        REQUIRE(coeff == Catch::Approx(expected_coeff));
    }

    SECTION("at boundary of first tier (4h) - multiplier 1") {
        double coeff =
            Core::Utils::calc_price_coeff_from_vol_and_trade_time_remaining_v2(
                normalized_vol, TRADE_MAX_TIME_MS, TRADE_MAX_TIME_MS, true);
        double expected_time_factor = 1.0 - 1.0;
        double expected_coeff = 1.0 - normalized_vol * expected_time_factor;
        REQUIRE(coeff == Catch::Approx(expected_coeff));
    }

    SECTION("second tier (4-8h) - multiplier 2") {
        long long duration_6h = 1000LL * 60 * 60 * 6;
        double coeff =
            Core::Utils::calc_price_coeff_from_vol_and_trade_time_remaining_v2(
                normalized_vol, duration_6h, TRADE_MAX_TIME_MS, true);
        double effective = 4.0 + 2.0 * 2;
        double expected_time_factor = 1.0 - (effective / 4.0);
        double expected_coeff = 1.0 - normalized_vol * expected_time_factor;
        REQUIRE(coeff == Catch::Approx(expected_coeff));
    }

    SECTION("at boundary of second tier (8h) - multipliers 1,2") {
        long long duration_8h = 1000LL * 60 * 60 * 8;
        double coeff =
            Core::Utils::calc_price_coeff_from_vol_and_trade_time_remaining_v2(
                normalized_vol, duration_8h, TRADE_MAX_TIME_MS, true);
        double effective = 4.0 + 4.0 * 2;
        double expected_time_factor = 1.0 - (effective / 4.0);
        double expected_coeff = 1.0 - normalized_vol * expected_time_factor;
        REQUIRE(coeff == Catch::Approx(expected_coeff));
    }

    SECTION("third tier (8-12h) - multiplier 3") {
        long long duration_10h = 1000LL * 60 * 60 * 10;
        double coeff =
            Core::Utils::calc_price_coeff_from_vol_and_trade_time_remaining_v2(
                normalized_vol, duration_10h, TRADE_MAX_TIME_MS, true);
        double effective = 4.0 + 4.0 * 2 + 2.0 * 3;
        double expected_time_factor = 1.0 - (effective / 4.0);
        double expected_coeff = 1.0 - normalized_vol * expected_time_factor;
        REQUIRE(coeff == Catch::Approx(expected_coeff));
    }

    SECTION("fourth tier (12-16h) - multiplier 4") {
        long long duration_14h = 1000LL * 60 * 60 * 14;
        double coeff =
            Core::Utils::calc_price_coeff_from_vol_and_trade_time_remaining_v2(
                normalized_vol, duration_14h, TRADE_MAX_TIME_MS, true);
        double effective = 4.0 + 4.0 * 2 + 4.0 * 3 + 2.0 * 4;
        double expected_time_factor = 1.0 - (effective / 4.0);
        double expected_coeff = 1.0 - normalized_vol * expected_time_factor;
        REQUIRE(coeff == Catch::Approx(expected_coeff));
    }

    SECTION("is_bid=false returns inverse coefficient") {
        long long duration_6h = 1000LL * 60 * 60 * 6;
        double coeff_bid =
            Core::Utils::calc_price_coeff_from_vol_and_trade_time_remaining_v2(
                normalized_vol, duration_6h, TRADE_MAX_TIME_MS, true);
        double coeff_ask =
            Core::Utils::calc_price_coeff_from_vol_and_trade_time_remaining_v2(
                normalized_vol, duration_6h, TRADE_MAX_TIME_MS, false);
        double bid_delta = 1.0 - coeff_bid;
        double ask_delta = coeff_ask - 1.0;
        REQUIRE(bid_delta == Catch::Approx(ask_delta));
    }
}

TEST_CASE("calc_price_coeff_v2 tiered vs old comparison",
          "[strategy_utils]") {
    const long long TRADE_MAX_TIME_MS = 1000LL * 60 * 60 * 4;
    const double volatility = 0.02;
    const double normalized_vol = volatility * 3;
    const double enter_price = 100.0;

    auto compute_old_effective_duration = [&](long long duration) {
        if (duration <= TRADE_MAX_TIME_MS) return duration;
        long long time_past_exit = duration - TRADE_MAX_TIME_MS;
        return TRADE_MAX_TIME_MS + time_past_exit * 2;
    };

    std::cout << "\n=== Tiered v2 vs Old comparison ===\n";
    for (long long hours = 0; hours <= 16; hours += 1) {
        long long duration = 1000LL * 60 * 60 * hours;

        long long old_effective = compute_old_effective_duration(duration);
        double old_time_factor =
            1.0 - (static_cast<double>(old_effective) / TRADE_MAX_TIME_MS);
        double old_coeff =
            Core::Utils::calc_price_coeff_from_vol_and_trade_time_remaining(
                normalized_vol, old_time_factor, true);

        double v2_coeff =
            Core::Utils::calc_price_coeff_from_vol_and_trade_time_remaining_v2(
                normalized_vol, duration, TRADE_MAX_TIME_MS, true);

        std::cout << "Duration: " << hours << "h | "
                  << "old_coeff: " << old_coeff << ", "
                  << "v2_coeff: " << v2_coeff << ", "
                  << "old_price: " << enter_price * old_coeff << ", "
                  << "v2_price: " << enter_price * v2_coeff << std::endl;
    }
}

TEST_CASE("calc_price_coeff_v2 five tiers output",
          "[test_full_five_tiers_output]") {
    const long long TRADE_MAX_TIME_MS = 1000LL * 60 * 60 * 4;  // 4 hours
    const double volatility = 0.02;
    const double normalized_vol = volatility * 3;
    const double enter_price = 100.0;
    const long long ONE_MINUTE_MS = 1000LL * 60;
    const long long FIVE_TIERS_MS = TRADE_MAX_TIME_MS * 5;  // 20 hours

    std::cout << "\n=== Minute-by-minute coefficient across 5 tiers ===\n";
    double prev_coeff = 0;
    for (long long duration = 0; duration <= FIVE_TIERS_MS;
         duration += ONE_MINUTE_MS) {
        double coeff =
            Core::Utils::calc_price_coeff_from_vol_and_trade_time_remaining_v2(
                normalized_vol, duration, TRADE_MAX_TIME_MS, true);

        long long minutes = duration / ONE_MINUTE_MS;
        int tier = static_cast<int>(duration / TRADE_MAX_TIME_MS) + 1;
        if (duration > 0 && duration % TRADE_MAX_TIME_MS == 0) tier--;

        std::cout << minutes << "min (tier " << tier
                  << "): " << "coeff=" << coeff << ", "
                  << "price=" << enter_price * coeff;
        if (duration > 0) {
            std::cout << ", delta=" << (coeff - prev_coeff);
        }
        std::cout << std::endl;

        prev_coeff = coeff;
    }
}

TEST_CASE("polymarket_dynamic_buffer bell curve", "[strategy_utils]") {
    const double min_buf = 0.025;
    const double max_buf = 0.07;

    SECTION("peak at midpoint") {
        REQUIRE(StrategyUtils::polymarket_dynamic_buffer(0.5, min_buf, max_buf) ==
                Catch::Approx(0.07));
    }

    SECTION("minimum at edges") {
        REQUIRE(StrategyUtils::polymarket_dynamic_buffer(0.0, min_buf, max_buf) ==
                Catch::Approx(0.025));
        REQUIRE(StrategyUtils::polymarket_dynamic_buffer(1.0, min_buf, max_buf) ==
                Catch::Approx(0.025));
    }

    SECTION("symmetry around 0.5") {
        REQUIRE(StrategyUtils::polymarket_dynamic_buffer(0.1, min_buf, max_buf) ==
                Catch::Approx(StrategyUtils::polymarket_dynamic_buffer(
                    0.9, min_buf, max_buf)));
        REQUIRE(StrategyUtils::polymarket_dynamic_buffer(0.3, min_buf, max_buf) ==
                Catch::Approx(StrategyUtils::polymarket_dynamic_buffer(
                    0.7, min_buf, max_buf)));
    }

    SECTION("near-edge value close to min") {
        double buf_001 =
            StrategyUtils::polymarket_dynamic_buffer(0.01, min_buf, max_buf);
        REQUIRE(buf_001 == Catch::Approx(0.02678).margin(0.001));
        REQUIRE(buf_001 > min_buf);
        REQUIRE(buf_001 < max_buf);
    }

    SECTION("monotonic from edge to center") {
        double buf_01 =
            StrategyUtils::polymarket_dynamic_buffer(0.1, min_buf, max_buf);
        double buf_03 =
            StrategyUtils::polymarket_dynamic_buffer(0.3, min_buf, max_buf);
        double buf_05 =
            StrategyUtils::polymarket_dynamic_buffer(0.5, min_buf, max_buf);
        REQUIRE(buf_05 > buf_03);
        REQUIRE(buf_03 > buf_01);
        REQUIRE(buf_01 > min_buf);
    }
}

TEST_CASE("polymarket buffer with linear time coefficient", "[strategy_utils]") {
    const double min_buf = 0.025;
    const double max_buf = 0.07;
    const double start_ms = 1000000.0;
    const double end_ms = 2000000.0;
    const double mid_ms = 1500000.0;

    SECTION("time coeff is 1.0 at event start") {
        double coeff = MathUtils::linear_mapping(
            start_ms, start_ms, 1.0, end_ms, 2.0, true);
        REQUIRE(coeff == Catch::Approx(1.0));
    }

    SECTION("time coeff is 1.5 at event midpoint") {
        double coeff = MathUtils::linear_mapping(
            mid_ms, start_ms, 1.0, end_ms, 2.0, true);
        REQUIRE(coeff == Catch::Approx(1.5));
    }

    SECTION("time coeff is 2.0 at event end") {
        double coeff = MathUtils::linear_mapping(
            end_ms, start_ms, 1.0, end_ms, 2.0, true);
        REQUIRE(coeff == Catch::Approx(2.0));
    }

    SECTION("combined buffer at midpoint fair value and event end doubles") {
        double base_buffer =
            StrategyUtils::polymarket_dynamic_buffer(0.5, min_buf, max_buf);
        double coeff = MathUtils::linear_mapping(
            end_ms, start_ms, 1.0, end_ms, 2.0, true);
        REQUIRE(base_buffer * coeff == Catch::Approx(0.14));
    }

    SECTION("time coeff clamps before event start") {
        double coeff = MathUtils::linear_mapping(
            start_ms - 100000, start_ms, 1.0, end_ms, 2.0, true);
        REQUIRE(coeff == Catch::Approx(1.0));
    }

    SECTION("time coeff clamps after event end") {
        double coeff = MathUtils::linear_mapping(
            end_ms + 100000, start_ms, 1.0, end_ms, 2.0, true);
        REQUIRE(coeff == Catch::Approx(2.0));
    }
}