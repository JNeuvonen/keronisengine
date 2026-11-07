#pragma once

#include "mpreal.h"

constexpr size_t BACKTEST_CHUNK_SIZE = 2500;

const mpfr::mpreal MPREAL_ZERO = mpfr::mpreal("0.0");
const mpfr::mpreal MIN_SIZE_TO_ASSUME_TRADE_CLOSED = mpfr::mpreal("20.0");
const mpfr::mpreal MPREAL_EPSILON_E_10 = mpfr::mpreal("1e-10");
const mpfr::mpreal MPREAL_EPSILON_E_6 = mpfr::mpreal("1e-6");
const mpfr::mpreal MPREAL_EPSILON_E_3 = mpfr::mpreal("1e-3");