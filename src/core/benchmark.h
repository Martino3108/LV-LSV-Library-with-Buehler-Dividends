/**
 * @file benchmark.h
 * @brief Benchmark entry points; verify_fit and helpers live in dedicated headers.
 */

#ifndef BENCHMARK_H
#define BENCHMARK_H

#include "benchmark_forward_start.h"
#include "buehler_iv_x_arbitrage.h"
#include "buehler_mc_settings.h"
#include "benchmark_bs_flat_reference.h"
#include "verify_fit.h"
#include <ql/quantlib.hpp>

class MarketData;

/** @brief Sanity-check loaded tables against built QuantLib term structures (no file I/O). */
void checkImportedData(const MarketData& md);

/**
 * @brief FD + Asian vs flat BS references on @c loadConstantMock() (flat Black–Scholes collapse test).
 * @return Mean absolute error across FD, Asian, and barrier rows.
 */
double pipeline_sanity_check_BS_fallback(QuantLib::BigNatural buehlerMcSeed = 701,
                                         QuantLib::BigNatural bsSeed = 1701,
                                         QuantLib::Size nSubbanks =
                                             bs_flat_reference::kDefaultPipelineSanitySubbanks,
                                         QuantLib::Size subbankSamples = kDefaultMcSamples);

/**
 * @brief 2Y arithmetic-fixed Asian: QuantLib vs fast MC (@c useFastPathSimulator).
 * @param md Caller-loaded market data.
 * Single LV calibration (with validate_calibration once); 50 seeds × 3 engines (MC + pricing only).
 */
void benchmark_asian_fast_seed_sweep(MarketData& md,
                                    QuantLib::Size fastWorkers = 6,
                                    QuantLib::Size mcSamples = kDefaultMcSamples);

#endif
