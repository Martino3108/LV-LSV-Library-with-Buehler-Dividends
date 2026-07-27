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

    bool allPassed() const {
        return violationsButterfly == 0 && violationsCalendar == 0;
    }
};

/** @brief Butterfly + calendar static arbitrage on bicubic σ_X (pure-X call prices). */
BuehlerImpliedVolXArbitrageReport check_static_arbitrage(
    const BuehlerModel& buehler,
    QuantLib::Size nTimeSamples = 32,
    QuantLib::Size nStrikeSamples = 64,
    double tolButterfly = -5.0e-4,
    double tolCalendar = -5.0e-4,
    bool verbose = true);

#endif
