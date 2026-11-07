#include "mpreal.h"

#include <stdexcept>

namespace {

std::string format(const char* fmt, int digits, mpfr_srcptr x) {
    char* out = nullptr;
    if (mpfr_asprintf(&out, fmt, digits, x) < 0) {
        throw std::runtime_error("mpfr_asprintf failed");
    }
    std::string s(out);
    mpfr_free_str(out);
    return s;
}

}  // namespace

std::string mpfr::mpreal::toString() const {
    const int digits =
        static_cast<int>(mpfr_get_str_ndigits(10, mpfr_get_prec(value_)));
    return format("%.*Rg", digits, value_);
}

std::ostream& mpfr::operator<<(std::ostream& os, const mpreal& x) {
    const auto floatfield = os.flags() & std::ios_base::floatfield;
    const char* fmt = floatfield == std::ios_base::fixed        ? "%.*Rf"
                      : floatfield == std::ios_base::scientific ? "%.*Re"
                                                                : "%.*Rg";
    return os << format(fmt, static_cast<int>(os.precision()),
                        x.mpfr_srcptr());
}
