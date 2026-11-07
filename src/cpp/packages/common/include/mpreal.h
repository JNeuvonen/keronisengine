#pragma once

// Arbitrary-precision real for prices, quantities and balances: an RAII value
// type over the MPFR C library, keeping the mpfr::mpreal interface the engine
// was written against.
//
// Precision rules:
//   - new values take the thread's default precision (set_default_prec);
//   - copies and assignment from another mpreal take the source's precision;
//   - assignment from a built-in number keeps the target's precision;
//   - a binary operation on two mpreals has the larger of their precisions,
//     one on an mpreal and a built-in number has the mpreal's precision.
// Arithmetic rounds to nearest; integer conversions truncate toward zero.

#include <cstdint>

// MPFR's function-like macros expand to the names mpfr_ptr / mpfr_srcptr,
// which inside mpreal resolve to its members of the same name.
#ifndef MPFR_USE_NO_MACRO
#define MPFR_USE_NO_MACRO
#endif
#ifndef MPFR_USE_INTMAX_T
#define MPFR_USE_INTMAX_T
#endif
#include <mpfr.h>

#include <algorithm>
#include <ostream>
#include <string>
#include <type_traits>

namespace mpfr {

class mpreal {
   public:
    mpreal() {
        mpfr_init2(value_, mpfr_get_default_prec());
        mpfr_set_zero(value_, 1);
    }
    mpreal(const mpreal& other) {
        mpfr_init2(value_, mpfr_get_prec(other.value_));
        mpfr_set(value_, other.value_, MPFR_RNDN);
    }
    // Leaves `other` a valid value: it may still be assigned to or destroyed.
    mpreal(mpreal&& other) noexcept {
        mpfr_init2(value_, mpfr_get_prec(other.value_));
        mpfr_swap(value_, other.value_);
    }
    mpreal(double v, mpfr_prec_t prec = mpfr_get_default_prec()) {
        mpfr_init2(value_, prec);
        mpfr_set_d(value_, v, MPFR_RNDN);
    }
    mpreal(float v, mpfr_prec_t prec = mpfr_get_default_prec())
        : mpreal(static_cast<double>(v), prec) {}
    mpreal(long double v, mpfr_prec_t prec = mpfr_get_default_prec()) {
        mpfr_init2(value_, prec);
        mpfr_set_ld(value_, v, MPFR_RNDN);
    }
    template <typename T,
              std::enable_if_t<std::is_integral_v<T> && std::is_signed_v<T>,
                               int> = 0>
    mpreal(T v, mpfr_prec_t prec = mpfr_get_default_prec()) {
        mpfr_init2(value_, prec);
        mpfr_set_sj(value_, static_cast<intmax_t>(v), MPFR_RNDN);
    }
    template <typename T,
              std::enable_if_t<std::is_integral_v<T> && std::is_unsigned_v<T>,
                               int> = 0>
    mpreal(T v, mpfr_prec_t prec = mpfr_get_default_prec()) {
        mpfr_init2(value_, prec);
        mpfr_set_uj(value_, static_cast<uintmax_t>(v), MPFR_RNDN);
    }
    // Decimal text. Text that does not parse is not an error: the value is
    // whatever mpfr_set_str leaves (0 for a fresh value).
    mpreal(const char* s, mpfr_prec_t prec = mpfr_get_default_prec()) {
        mpfr_init2(value_, prec);
        mpfr_set_str(value_, s, 10, MPFR_RNDN);
    }
    mpreal(const std::string& s, mpfr_prec_t prec = mpfr_get_default_prec())
        : mpreal(s.c_str(), prec) {}

    ~mpreal() { mpfr_clear(value_); }

    mpreal& operator=(const mpreal& other) {
        if (this != &other) {
            if (mpfr_get_prec(value_) != mpfr_get_prec(other.value_)) {
                mpfr_set_prec(value_, mpfr_get_prec(other.value_));
            }
            mpfr_set(value_, other.value_, MPFR_RNDN);
        }
        return *this;
    }
    mpreal& operator=(mpreal&& other) noexcept {
        mpfr_swap(value_, other.value_);
        return *this;
    }
    mpreal& operator=(double v) {
        mpfr_set_d(value_, v, MPFR_RNDN);
        return *this;
    }
    mpreal& operator=(float v) { return *this = static_cast<double>(v); }
    mpreal& operator=(long double v) {
        mpfr_set_ld(value_, v, MPFR_RNDN);
        return *this;
    }
    template <typename T, std::enable_if_t<std::is_integral_v<T>, int> = 0>
    mpreal& operator=(T v) {
        if constexpr (std::is_signed_v<T>) {
            mpfr_set_sj(value_, static_cast<intmax_t>(v), MPFR_RNDN);
        } else {
            mpfr_set_uj(value_, static_cast<uintmax_t>(v), MPFR_RNDN);
        }
        return *this;
    }
    // Text that does not parse in full leaves the value unchanged.
    mpreal& operator=(const char* s) {
        mpreal parsed(0, get_prec());
        if (mpfr_set_str(parsed.value_, s, 10, MPFR_RNDN) == 0) {
            mpfr_swap(value_, parsed.value_);
        }
        return *this;
    }
    mpreal& operator=(const std::string& s) { return *this = s.c_str(); }

    mpreal& operator+=(const mpreal& v) {
        mpfr_add(value_, value_, v.value_, MPFR_RNDN);
        return *this;
    }
    mpreal& operator-=(const mpreal& v) {
        mpfr_sub(value_, value_, v.value_, MPFR_RNDN);
        return *this;
    }
    mpreal& operator*=(const mpreal& v) {
        mpfr_mul(value_, value_, v.value_, MPFR_RNDN);
        return *this;
    }
    mpreal& operator/=(const mpreal& v) {
        mpfr_div(value_, value_, v.value_, MPFR_RNDN);
        return *this;
    }
    mpreal& operator+=(double v) {
        mpfr_add_d(value_, value_, v, MPFR_RNDN);
        return *this;
    }
    mpreal& operator-=(double v) {
        mpfr_sub_d(value_, value_, v, MPFR_RNDN);
        return *this;
    }
    mpreal& operator*=(double v) {
        mpfr_mul_d(value_, value_, v, MPFR_RNDN);
        return *this;
    }
    mpreal& operator/=(double v) {
        mpfr_div_d(value_, value_, v, MPFR_RNDN);
        return *this;
    }
    mpreal& operator+=(float v) { return *this += static_cast<double>(v); }
    mpreal& operator-=(float v) { return *this -= static_cast<double>(v); }
    mpreal& operator*=(float v) { return *this *= static_cast<double>(v); }
    mpreal& operator/=(float v) { return *this /= static_cast<double>(v); }
    mpreal& operator+=(long double v) { return *this += mpreal(v, get_prec()); }
    mpreal& operator-=(long double v) { return *this -= mpreal(v, get_prec()); }
    mpreal& operator*=(long double v) { return *this *= mpreal(v, get_prec()); }
    mpreal& operator/=(long double v) { return *this /= mpreal(v, get_prec()); }
    template <typename T, std::enable_if_t<std::is_integral_v<T>, int> = 0>
    mpreal& operator+=(T v) {
        mpfr_add_sj_or_uj(v, mpfr_add);
        return *this;
    }
    template <typename T, std::enable_if_t<std::is_integral_v<T>, int> = 0>
    mpreal& operator-=(T v) {
        mpfr_add_sj_or_uj(v, mpfr_sub);
        return *this;
    }
    template <typename T, std::enable_if_t<std::is_integral_v<T>, int> = 0>
    mpreal& operator*=(T v) {
        mpfr_add_sj_or_uj(v, mpfr_mul);
        return *this;
    }
    template <typename T, std::enable_if_t<std::is_integral_v<T>, int> = 0>
    mpreal& operator/=(T v) {
        mpfr_add_sj_or_uj(v, mpfr_div);
        return *this;
    }

    mpreal& operator++() { return *this += 1; }
    mpreal& operator--() { return *this -= 1; }
    mpreal operator++(int) {
        mpreal before(*this);
        *this += 1;
        return before;
    }
    mpreal operator--(int) {
        mpreal before(*this);
        *this -= 1;
        return before;
    }

    mpreal operator-() const {
        mpreal r(*this);
        mpfr_neg(r.value_, r.value_, MPFR_RNDN);
        return r;
    }
    mpreal operator+() const { return *this; }

    double toDouble() const { return mpfr_get_d(value_, MPFR_RNDN); }
    long double toLDouble() const { return mpfr_get_ld(value_, MPFR_RNDN); }
    long toLong() const { return mpfr_get_si(value_, MPFR_RNDZ); }
    unsigned long toULong() const { return mpfr_get_ui(value_, MPFR_RNDZ); }
    long long toLLong() const {
        return static_cast<long long>(mpfr_get_sj(value_, MPFR_RNDZ));
    }
    // %g with enough significant digits to read back the same value at this
    // precision (17 at 53 bits).
    std::string toString() const;

    mpfr_prec_t get_prec() const { return mpfr_get_prec(value_); }
    // Changes precision keeping the value, rounded to nearest.
    void set_prec(mpfr_prec_t prec) { mpfr_prec_round(value_, prec, MPFR_RNDN); }

    static void set_default_prec(mpfr_prec_t prec) {
        mpfr_set_default_prec(prec);
    }
    static mpfr_prec_t get_default_prec() { return mpfr_get_default_prec(); }

    ::mpfr_ptr mpfr_ptr() { return value_; }
    ::mpfr_srcptr mpfr_srcptr() const { return value_; }

   private:
    // Integers go through an exact temporary: MPFR has no intmax_t variants
    // of add/sub/mul/div.
    template <typename T, typename Fn>
    void mpfr_add_sj_or_uj(T v, Fn fn) {
        const mpfr_prec_t prec =
            std::max<mpfr_prec_t>(get_prec(), sizeof(T) * 8);
        mpreal exact(v, prec);
        fn(value_, value_, exact.value_, MPFR_RNDN);
    }

    mpfr_t value_;
};

namespace detail {
inline mpfr_prec_t max_prec(const mpreal& a, const mpreal& b) {
    return std::max(a.get_prec(), b.get_prec());
}
// Precision that holds a built-in number exactly.
template <typename T>
constexpr mpfr_prec_t exact_prec() {
    if constexpr (std::is_integral_v<T>) return sizeof(T) * 8;
    else return 64;
}
}  // namespace detail

// Result precision with a built-in operand follows the long-standing mpreal
// behavior the engine's numbers were produced with: the mpreal's precision,
// except a float operand on the right (the larger of the mpreal's and the
// default precision) and a 64-bit integer on the left of - or / (the default
// precision).
#define KERONIS_MPREAL_BINARY_OP(op, mpfr_fn, commutative)                  \
    inline mpreal operator op(const mpreal& a, const mpreal& b) {           \
        mpreal r(0, detail::max_prec(a, b));                                \
        mpfr_fn(r.mpfr_ptr(), a.mpfr_srcptr(), b.mpfr_srcptr(), MPFR_RNDN); \
        return r;                                                           \
    }                                                                       \
    template <typename T,                                                   \
              std::enable_if_t<std::is_arithmetic_v<T>, int> = 0>           \
    inline mpreal operator op(const mpreal& a, T b) {                       \
        if constexpr (std::is_same_v<T, float>) {                           \
            return a op mpreal(b);                                          \
        } else {                                                            \
            mpreal r(a);                                                    \
            r op## = b;                                                     \
            return r;                                                       \
        }                                                                   \
    }                                                                       \
    template <typename T,                                                   \
              std::enable_if_t<std::is_arithmetic_v<T>, int> = 0>           \
    inline mpreal operator op(T a, const mpreal& b) {                       \
        constexpr bool default_prec_result =                                \
            !(commutative) && (std::is_same_v<T, long long> ||              \
                               std::is_same_v<T, unsigned long long>);      \
        mpreal r(0, default_prec_result ? mpfr_get_default_prec()           \
                                        : b.get_prec());                    \
        mpfr_fn(r.mpfr_ptr(),                                               \
                mpreal(a, std::max(b.get_prec(), detail::exact_prec<T>()))  \
                    .mpfr_srcptr(),                                         \
                b.mpfr_srcptr(), MPFR_RNDN);                                \
        return r;                                                           \
    }

KERONIS_MPREAL_BINARY_OP(+, mpfr_add, true)
KERONIS_MPREAL_BINARY_OP(-, mpfr_sub, false)
KERONIS_MPREAL_BINARY_OP(*, mpfr_mul, true)
KERONIS_MPREAL_BINARY_OP(/, mpfr_div, false)
#undef KERONIS_MPREAL_BINARY_OP

// Comparisons involving NaN are false, except !=.
#define KERONIS_MPREAL_COMPARISON(op, mpfr_predicate)                         \
    inline bool operator op(const mpreal& a, const mpreal& b) {               \
        return mpfr_predicate(a.mpfr_srcptr(), b.mpfr_srcptr()) != 0;         \
    }                                                                         \
    template <typename T,                                                     \
              std::enable_if_t<std::is_arithmetic_v<T>, int> = 0>             \
    inline bool operator op(const mpreal& a, T b) {                           \
        return a op mpreal(b, detail::exact_prec<T>());                       \
    }                                                                         \
    template <typename T,                                                     \
              std::enable_if_t<std::is_arithmetic_v<T>, int> = 0>             \
    inline bool operator op(T a, const mpreal& b) {                           \
        return mpreal(a, detail::exact_prec<T>()) op b;                       \
    }

KERONIS_MPREAL_COMPARISON(==, mpfr_equal_p)
KERONIS_MPREAL_COMPARISON(<, mpfr_less_p)
KERONIS_MPREAL_COMPARISON(<=, mpfr_lessequal_p)
KERONIS_MPREAL_COMPARISON(>, mpfr_greater_p)
KERONIS_MPREAL_COMPARISON(>=, mpfr_greaterequal_p)
#undef KERONIS_MPREAL_COMPARISON

inline bool operator!=(const mpreal& a, const mpreal& b) { return !(a == b); }
template <typename T, std::enable_if_t<std::is_arithmetic_v<T>, int> = 0>
inline bool operator!=(const mpreal& a, T b) {
    return !(a == b);
}
template <typename T, std::enable_if_t<std::is_arithmetic_v<T>, int> = 0>
inline bool operator!=(T a, const mpreal& b) {
    return !(a == b);
}

#define KERONIS_MPREAL_UNARY_FN(name, mpfr_fn)             \
    inline mpreal name(const mpreal& x) {                  \
        mpreal r(0, x.get_prec());                         \
        mpfr_fn(r.mpfr_ptr(), x.mpfr_srcptr(), MPFR_RNDN); \
        return r;                                          \
    }

KERONIS_MPREAL_UNARY_FN(abs, mpfr_abs)
KERONIS_MPREAL_UNARY_FN(fabs, mpfr_abs)
KERONIS_MPREAL_UNARY_FN(sqrt, mpfr_sqrt)
KERONIS_MPREAL_UNARY_FN(log, mpfr_log)
KERONIS_MPREAL_UNARY_FN(log10, mpfr_log10)
KERONIS_MPREAL_UNARY_FN(exp, mpfr_exp)
#undef KERONIS_MPREAL_UNARY_FN

// MPFR's integer-rounding functions take no rounding mode.
#define KERONIS_MPREAL_INTEGER_FN(name, mpfr_fn)  \
    inline mpreal name(const mpreal& x) {         \
        mpreal r(0, x.get_prec());                \
        mpfr_fn(r.mpfr_ptr(), x.mpfr_srcptr());   \
        return r;                                 \
    }

KERONIS_MPREAL_INTEGER_FN(round, mpfr_round)
KERONIS_MPREAL_INTEGER_FN(floor, mpfr_floor)
KERONIS_MPREAL_INTEGER_FN(ceil, mpfr_ceil)
KERONIS_MPREAL_INTEGER_FN(trunc, mpfr_trunc)
#undef KERONIS_MPREAL_INTEGER_FN

inline mpreal pow(const mpreal& base, const mpreal& exponent) {
    mpreal r(0, detail::max_prec(base, exponent));
    mpfr_pow(r.mpfr_ptr(), base.mpfr_srcptr(), exponent.mpfr_srcptr(),
             MPFR_RNDN);
    return r;
}
template <typename T, std::enable_if_t<std::is_integral_v<T>, int> = 0>
inline mpreal pow(const mpreal& base, T exponent) {
    mpreal r(0, base.get_prec());
    mpfr_pow_sj(r.mpfr_ptr(), base.mpfr_srcptr(),
                static_cast<intmax_t>(exponent), MPFR_RNDN);
    return r;
}
inline mpreal pow(const mpreal& base, double exponent) {
    mpreal r(0, base.get_prec());
    mpfr_pow(r.mpfr_ptr(), base.mpfr_srcptr(),
             mpreal(exponent, 64).mpfr_srcptr(), MPFR_RNDN);
    return r;
}

inline mpreal fmod(const mpreal& x, const mpreal& y) {
    mpreal r(0, detail::max_prec(x, y));
    mpfr_fmod(r.mpfr_ptr(), x.mpfr_srcptr(), y.mpfr_srcptr(), MPFR_RNDN);
    return r;
}

inline const mpreal& max(const mpreal& a, const mpreal& b) {
    return a < b ? b : a;
}
inline const mpreal& min(const mpreal& a, const mpreal& b) {
    return b < a ? b : a;
}

inline bool isnan(const mpreal& x) { return mpfr_nan_p(x.mpfr_srcptr()) != 0; }
inline bool isinf(const mpreal& x) { return mpfr_inf_p(x.mpfr_srcptr()) != 0; }
inline bool isfinite(const mpreal& x) {
    return mpfr_number_p(x.mpfr_srcptr()) != 0;
}
inline bool iszero(const mpreal& x) { return mpfr_zero_p(x.mpfr_srcptr()) != 0; }
inline int sgn(const mpreal& x) { return mpfr_sgn(x.mpfr_srcptr()); }

// Honors the stream's precision and its fixed / scientific flags, like
// streaming a double.
std::ostream& operator<<(std::ostream& os, const mpreal& x);

inline std::string toString(const mpreal& x) { return x.toString(); }

}  // namespace mpfr
