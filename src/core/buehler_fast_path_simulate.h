/**
 * @file buehler_fast_path_simulate.h
 * @brief Fast Euler MC bank (σ lookup on calib FixedLocalVol grid, optional OpenMP).
 */

#ifndef BUEHLER_FAST_PATH_SIMULATE_H
#define BUEHLER_FAST_PATH_SIMULATE_H

#include "buehler_fixing_save_path.h"
#include "buehler_mc_settings.h"
#include <vector>

class BuehlerModel;

/** @brief Fast LV simulate on @p evolutionDates; bank stores @p saveFixingDates (subset allowed). */
BuehlerFixingSavePath simulateBuehlerFixingSavePathFast(const BuehlerModel& buehler,
                                                        const QuantLib::Date& horizonMax,
                                                        const std::vector<QuantLib::Date>& evolutionDates,
                                                        const std::vector<QuantLib::Date>& saveFixingDates,
                                                        const BuehlerMcSettings& settings);

struct BuehlerFastSigmaErrorReport {
    QuantLib::Real maxAbsError = 0.0;
    QuantLib::Real meanAbsError = 0.0;
    QuantLib::Size sampleCount = 0;
};

/** @brief Compare tabulated σ on the calib grid to @c localVol. */
BuehlerFastSigmaErrorReport measureDenseFixedLvSigmaError(const BuehlerModel& buehler);

#endif
