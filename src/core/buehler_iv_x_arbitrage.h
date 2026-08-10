/**
 * @file buehler_iv_x_arbitrage.h
 * @brief Static arbitrage checks on bicubic σ_X from calibrated Buehler model.
 */

#ifndef BUEHLER_IV_X_ARBITRAGE_H
#define BUEHLER_IV_X_ARBITRAGE_H

#include "buehler_model.h"
#include <ql/quantlib.hpp>

struct BuehlerImpliedVolXArbitrageReport {
    QuantLib::Size nSamplesButterfly = 0;
    QuantLib::Size violationsButterfly = 0;
    double minButterfly = 0.0;

    QuantLib::Size nSamplesCalendar = 0;
    QuantLib::Size violationsCalendar = 0;
    double minCalendar = 0.0;

    /** Max allowed violation share of sampled cells (butterfly and calendar). */
    static constexpr double kMaxViolationFraction = 0.01;
    /** Hard floor on min ∂²C/∂k²; below this the gate fails regardless of count. */
    static constexpr double kMinButterflyFloor = -0.01;

    bool allPassed() const {
        const auto fractionOk = [](QuantLib::Size violations, QuantLib::Size nSamples) {
            if (nSamples == 0)
                return true;
            return static_cast<double>(violations) <=
                   kMaxViolationFraction * static_cast<double>(nSamples);
        };
        return fractionOk(violationsButterfly, nSamplesButterfly) &&
               fractionOk(violationsCalendar, nSamplesCalendar) &&
               minButterfly >= kMinButterflyFloor;
    }
};

/** @brief Butterfly + calendar static arbitrage on bicubic σ_X (pure-X call prices). */
BuehlerImpliedVolXArbitrageReport check_static_arbitrage(
    const BuehlerModel& buehler,
    QuantLib::Size nTimeSamples = 240,
    QuantLib::Size nStrikeSamples = 100,
    double tolButterfly = 0.0,
    double tolCalendar = 0.0,
    bool verbose = true);

#endif
