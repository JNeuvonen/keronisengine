#include <generic_utils.h>
#include <mpreal.h>

#include <algorithm>
#include <atomic>
#include <iostream>
#include <string>

#include "datetime_utils.h"

using namespace mpfr;

void CommonUtils::to_lowercase(std::string &str) {
    std::transform(str.begin(), str.end(), str.begin(), ::tolower);
}

void CommonUtils::to_uppercase(std::string &str) {
    std::transform(str.begin(), str.end(), str.begin(), ::toupper);
}

std::string CommonUtils::generate_unique_id() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> distrib(100000000, 999999999);

    uint32_t random_id = distrib(gen);

    std::ostringstream oss;
    return oss.str();
}

std::string CommonUtils::generate_unique_id_v2() {
    static std::atomic<long long> counter = 0;
    long long count = counter++;

    std::ostringstream oss;
    oss << std::hex << DatetimeUtils::get_current_time_ms() << std::setfill('0')
        << std::setw(4) << count;
    return oss.str();
}

std::string CommonUtils::generate_random_id(size_t num_bits) {
    static thread_local std::mt19937_64 gen(std::random_device{}());
    static std::uniform_int_distribution<uint64_t> distrib(0, UINT64_MAX);

    if (num_bits == 0) return "0";

    size_t num_bytes = (num_bits + 7) / 8;
    uint64_t random_value = distrib(gen);

    if (num_bits < 64) {
        random_value &= ((1ULL << num_bits) - 1);
    }

    std::ostringstream oss;
    oss << std::hex << random_value;
    return oss.str();
}

std::string CommonUtils::truncate_to_precision(double value, int precision) {
    if (precision < 0) return std::to_string(value);

    double factor = std::pow(10.0, precision);
    double truncatedValue = std::trunc(value * factor) / factor;

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(precision) << truncatedValue;
    return oss.str();
}
