#pragma once

#include <string>
namespace Common {
long long generate_uuid();
unsigned long long generate_timestamp_uuid();
std::string hash_to_32bit(unsigned long long value);
std::string hash_to_base62(unsigned long long value, size_t length);
std::string hash_to_26char(unsigned long long value);
}  // namespace Common
