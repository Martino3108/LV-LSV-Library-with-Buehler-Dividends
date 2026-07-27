/**
 * @file benchmark_forward_start.h
 * @brief Forward-start smile sweep (LV vs LSV MC).
 */

#ifndef BENCHMARK_FORWARD_START_H
#define BENCHMARK_FORWARD_START_H

#include "buehler_mc_settings.h"
#include <ql/quantlib.hpp>

class MarketData;

/**
 * @brief T1 sweep at fixed T2: simulate LV/LSV once to T2=@p expiryYears, then price FS smiles
 * for T1 = 0 .. @p t1MaxYears on @p md. Logs to @c out_forward_start_sweep.txt.
 */
void forward_start_smile_t1_sweep(const MarketData& md,
                                  int expiryYears = 7,
                                  int t1MaxYears = 6,
                                  QuantLib::Size mcSamples = kDefaultMcSamples,
                                  QuantLib::Size lsvBins = kDefaultLsvBins,
                                  QuantLib::BigNatural lvSeed = kDefaultMcSeed);

#endif
