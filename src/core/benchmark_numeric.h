/**
 * @file benchmark_numeric.h
 * @brief Small numeric helpers for benchmarks and verify_fit.
 */

#ifndef BENCHMARK_NUMERIC_H
#define BENCHMARK_NUMERIC_H

#include <ql/quantlib.hpp>

inline double benchmarkToDouble(const QuantLib::Real& x) {
    return static_cast<double>(x);
}

#endif
