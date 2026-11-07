#include "str_math.h"

#include <iomanip>
#include <sstream>

#include "mpreal.h"

using mpfr::mpreal;

/* ─────────────────────────  helpers  ────────────────────────── */
namespace {
// Trim trailing zeroes (and the dangling dot, if any).
inline std::string tidy(std::string s) {
    auto dot = s.find('.');

    if (dot != std::string::npos) {
        // erase trailing 0-characters
        s.erase(s.find_last_not_of('0') + 1);
        // if the string now ends in '.', drop it as well
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return s.empty() ? "0" : s;
}
}  // namespace

/* ─────────────────────────  StrMath  ────────────────────────── */
std::string StrMath::sum(const std::string& a, const std::string& b,
                         int precision) {
    // plenty of internal bits so rounding is never a problem
    static bool prec_set = (mpreal::set_default_prec(256), true);

    const mpreal lhs(a);
    const mpreal rhs(b);
    const mpreal res = lhs + rhs;  // exact in MPFR

    std::ostringstream out;
    out << std::fixed << std::setprecision(precision) << res;
    return tidy(out.str());
}

std::string StrMath::minus(const std::string& a, const std::string& b,
                           int precision) {
    static bool prec_set = (mpreal::set_default_prec(256), true);

    const mpreal lhs(a);
    const mpreal rhs(b);
    const mpreal res = lhs - rhs;

    std::ostringstream out;
    out << std::fixed << std::setprecision(precision) << res;
    return tidy(out.str());
}

std::string StrMath::to_fixed_string(mpfr::mpreal float_value, int precision) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(precision) << float_value;
    return tidy(out.str());
}

int StrMath::step_size_to_precision(double step_size) {
    std::ostringstream tmp;
    tmp << std::fixed << std::setprecision(18) << step_size;
    std::string s = tmp.str();

    const auto dot = s.find('.');
    if (dot == std::string::npos) return 0;

    size_t first_nonzero = s.find_first_not_of('0', dot + 1);
    if (first_nonzero == std::string::npos) return 0;
    if (first_nonzero <= dot) return 0;

    int ret = static_cast<int>(first_nonzero - dot);
    return ret;
}