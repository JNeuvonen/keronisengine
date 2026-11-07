#pragma once

#include <string>

#include "mpreal.h"

namespace StrMath {
std::string sum(const std::string& a, const std::string& b, int precision);
std::string minus(const std::string& a, const std::string& b, int precision);
std::string to_fixed_string(mpfr::mpreal float_value, int precision);
int step_size_to_precision(double step_size);
}  // namespace StrMath