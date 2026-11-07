#include "mpfr_utils.h"

void MpfrUtils::round_to_nearest(mpfr::mpreal& value, int decimal_places) {
    mpfr::mpreal scale = mpfr::pow(mpfr::mpreal(10), decimal_places);
    value = round(value * scale) / scale;
}

mpfr::mpreal MpfrUtils::str_to_fractional_mpreal(const std::string& str) {
    size_t decimal_pos = str.find('.');

    if (decimal_pos == std::string::npos) {
        return mpfr::mpreal(str);
    }

    std::string integer_part = str;
    integer_part.erase(decimal_pos, 1);

    size_t decimal_places = str.length() - decimal_pos - 1;

    mpfr::mpreal numerator(integer_part);
    mpfr::mpreal denominator = mpfr::pow(mpfr::mpreal(10), decimal_places);
    mpfr::mpreal result = numerator / denominator;

    round_to_nearest(result, decimal_places);
    return result;
}
