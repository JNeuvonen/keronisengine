#include "uuid.h"

#include <atomic>
#include <chrono>
#include <random>
#include <thread>

namespace Common {
// Original simple counter-based UUID (kept for backward compatibility)
long long generate_uuid() {
    static std::atomic<long long> uuid_counter(0);
    return uuid_counter.fetch_add(1);
}

unsigned long long generate_timestamp_uuid() {
    auto now = std::chrono::system_clock::now();
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now.time_since_epoch())
                      .count();

    static thread_local std::mt19937_64 rng(
        std::chrono::steady_clock::now().time_since_epoch().count() +
        std::hash<std::thread::id>{}(std::this_thread::get_id()));
    static thread_local std::uniform_int_distribution<unsigned long long> dist(
        0, (1ULL << 23) - 1);

    return (static_cast<unsigned long long>(millis) << 23) | dist(rng);
}

std::string hash_to_32bit(unsigned long long value) {
    value = ((value >> 16) ^ value) * 0x45d9f3b;
    value = ((value >> 16) ^ value) * 0x45d9f3b;
    value = (value >> 16) ^ value;
    uint32_t hash = static_cast<uint32_t>(value);

    std::string result;
    result.reserve(32);
    for (int i = 31; i >= 0; --i) {
        result += ((hash >> i) & 1) ? '1' : '0';
    }
    return result;
}

std::string hash_to_base62(unsigned long long value, size_t length) {
    static const char charset[] = "abcdefghijklmnopqrsuvwxyz0123456789";
    static const size_t base = sizeof(charset) - 1;

    static thread_local std::mt19937_64 rng(
        std::chrono::steady_clock::now().time_since_epoch().count() +
        std::hash<std::thread::id>{}(std::this_thread::get_id()));
    static thread_local std::uniform_int_distribution<unsigned long long>
        dist64(0, UINT64_MAX);

    unsigned long long mixed = value ^ dist64(rng);
    mixed = ((mixed >> 16) ^ mixed) * 0x45d9f3b;
    mixed = ((mixed >> 16) ^ mixed) * 0x45d9f3b;
    mixed = (mixed >> 16) ^ mixed;

    std::string result;
    result.reserve(length);

    unsigned long long current = mixed;
    for (size_t i = 0; i < length; ++i) {
        result += charset[current % base];
        current = current / base + dist64(rng);
    }
    return result;
}

std::string hash_to_26char(unsigned long long value) {
    return hash_to_base62(value, 26);
}
}  // namespace Common
