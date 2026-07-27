/**
 * @file buehler_fixing_path_simulate.h
 * @brief MC save-path simulation on pure-X LV or LSV (Bergomi 1F + bins).
 */

#ifndef BUEHLER_FIXING_PATH_SIMULATE_H
#define BUEHLER_FIXING_PATH_SIMULATE_H

#include "buehler_fixing_save_path.h"
#include "buehler_mc_settings.h"
#include <vector>

class BuehlerModel;

std::vector<QuantLib::Date> normalizeSimulationDates(const BuehlerModel& buehler,
                                                     std::vector<QuantLib::Date> dates,
                                                     const QuantLib::Date& horizonMax);

std::vector<QuantLib::Date> buehlerMcSimulationDatesEveryNBusinessDays(
    const BuehlerModel& buehler,
    const QuantLib::Date& horizonMax,
    int businessDayStep = kDefaultMcBusinessDayStep);

/** @brief Resolve stored fixings; @p requestedSaveDates empty → all @p evolutionDates. */
std::vector<QuantLib::Date> resolveMcSavePathFixingDates(
    const std::vector<QuantLib::Date>& evolutionDates,
    const std::vector<QuantLib::Date>& requestedSaveDates);

/**
 * @brief Martingale gate on the terminal path cloud: warn on stderr when
 * @f$|\bar X_T - 1|@f$ exceeds @p warnThreshold (0 disables). @p tag names the simulator.
 */
void buehlerMcCheckMartingaleDrift(const std::vector<QuantLib::Real>& xStateAtHorizon,
                                   QuantLib::Real warnThreshold,
                                   const char* tag);

/** @brief LV (@c PathGenerator / fast tabulated σ) or LSV (Bergomi 1F + bins) per @c settings.dynamics. */
BuehlerFixingSavePath simulateBuehlerFixingSavePath(const BuehlerModel& buehler,
                                                    const QuantLib::Date& horizonMax,
                                                    const std::vector<QuantLib::Date>& simulationDates,
                                                    const BuehlerMcSettings& settings = BuehlerMcSettings{});

#endif
