#include <iomanip>
#include <sstream>

#include "catch2/catch_all.hpp"
#include "mpreal.h"

// Expected values are what prices, quantities and P&L have always been
// formatted and rounded as; a change here changes orders, reports and
// stored rows.

using mpfr::mpreal;

namespace {

struct DefaultPrecision {
    explicit DefaultPrecision(mpfr_prec_t prec)
        : saved(mpreal::get_default_prec()) {
        mpreal::set_default_prec(prec);
    }
    ~DefaultPrecision() { mpreal::set_default_prec(saved); }
    mpfr_prec_t saved;
};

std::string streamed(const mpreal& x, int precision, bool fixed) {
    std::ostringstream out;
    if (fixed) out << std::fixed;
    out << std::setprecision(precision) << x;
    return out.str();
}

}  // namespace

TEST_CASE("mpreal toString prints round-trip significant digits",
          "[mpreal]") {
    {
        DefaultPrecision p(53);
        CHECK(mpreal("0.1").toString() == "0.10000000000000001");
        CHECK(mpreal("123.456").toString() == "123.456");
        CHECK(mpreal("0.00001234").toString() == "1.234e-05");
        CHECK(mpreal("1e20").toString() == "1e+20");
        CHECK(mpreal("65432.1").toString() == "65432.099999999999");
    }
    {
        DefaultPrecision p(128);
        CHECK(mpreal("0.1").toString() ==
              "0.1000000000000000000000000000000000000001");
        CHECK(mpreal("1e20").toString() == "100000000000000000000");
        CHECK(mpreal("65432.1").toString() ==
              "65432.10000000000000000000000000000000008");
    }
}

TEST_CASE("mpreal streams like a double", "[mpreal]") {
    DefaultPrecision p(128);
    CHECK(streamed(mpreal("123.456"), 6, false) == "123.456");
    CHECK(streamed(mpreal("1e20"), 6, false) == "1e+20");
    CHECK(streamed(mpreal("0.00001234"), 8, true) == "0.00001234");
    CHECK(streamed(mpreal("65432.1"), 8, true) == "65432.10000000");
}

TEST_CASE("mpreal result precision", "[mpreal]") {
    DefaultPrecision p(53);
    const mpreal high(1.5, 200);
    const mpreal low(1.5, 20);
    const long long ll = 3;
    const float f = 1.25f;

    CHECK((high + low).get_prec() == 200);
    CHECK((high + 3).get_prec() == 200);
    CHECK((3 + low).get_prec() == 20);
    CHECK((ll - high).get_prec() == 53);
    CHECK((ll / low).get_prec() == 53);
    CHECK((ll + high).get_prec() == 200);
    CHECK((high + f).get_prec() == 200);
    CHECK((low + f).get_prec() == 53);
    CHECK(pow(low, 0.5).get_prec() == 20);

    mpreal target(1, 20);
    target = 5LL;
    CHECK(target.get_prec() == 20);
    target = high;
    CHECK(target.get_prec() == 200);
}

TEST_CASE("mpreal string parsing", "[mpreal]") {
    DefaultPrecision p(64);
    CHECK(mpreal("abc").toString() == "0");
    CHECK(mpreal("12abc").toString() == "12");
    CHECK(mpfr::isnan(mpreal("")));

    mpreal kept(495, 64);
    kept = std::string("12abc");
    CHECK(kept.toString() == "495");
    kept = "";
    CHECK(kept.toString() == "495");
    kept = "0.3";
    CHECK(kept.toString() == "0.300000000000000000011");
}

TEST_CASE("mpreal comparisons with NaN are false except !=", "[mpreal]") {
    const mpreal nan(std::numeric_limits<double>::quiet_NaN());
    CHECK_FALSE(nan < 1);
    CHECK_FALSE(nan == nan);
    CHECK(nan != nan);
    CHECK_FALSE(nan > 0);
}
