/**
 * @file buehler_lsv_path_simulate.h
 * @brief LSV / pure-Bergomi fixing-bank MC on pure @e X.
 */

#ifndef BUEHLER_LSV_PATH_SIMULATE_H
#define BUEHLER_LSV_PATH_SIMULATE_H

#include "buehler_fixing_save_path.h"
#include "buehler_mc_settings.h"
#include <vector>

class BuehlerModel;

/**
 * @brief Save-path MC: LSV (Dupire + binned @f$E[e^{2\nu Y}\mid X]@f$) or pure Bergomi (@f$\sigma_{\mathrm{LV}}=\hat g=1@f$).
 * OpenMP team simulation via @c mcPathWorkers: team-parallel bins (per-thread partials merged
 * in thread order) and per-thread RNG in evolve; bank stored as a flat path-major buffer.
 */
BuehlerFixingSavePath simulateBuehlerFixingSavePathLsv(
    const BuehlerModel& buehler,
    const QuantLib::Date& horizonMax,
    const std::vector<QuantLib::Date>& evolutionDates,
    const std::vector<QuantLib::Date>& saveFixingDates,
    const BuehlerMcSettings& settings);

#endif
