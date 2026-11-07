#pragma once
#include "mpreal.h"

namespace MpfrUtils {
void round_to_nearest(mpfr::mpreal& alue, int decimal_places);
mpfr::mpreal str_to_fractional_mpreal(const std::string& str);
}  // namespace MpfrUtils
