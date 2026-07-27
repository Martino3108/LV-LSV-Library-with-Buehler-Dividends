/**
 * @file mc_observation_schedule.h
 * @brief Shared observation schedules for path-dependent MC pricers (quoted in S).
 */

#ifndef MC_OBSERVATION_SCHEDULE_H
#define MC_OBSERVATION_SCHEDULE_H

#include "option.h"
#include <vector>
#include <ql/quantlib.hpp>

class BuehlerModel;

/** @brief Monthly @c Following grid from today through @p expiry (appends @p expiry if needed). */
std::vector<QuantLib::Date> mcObservationDatesMonthlyThroughExpiry(const BuehlerModel& buehler,
                                                                     const QuantLib::Date& expiry);

/** @brief Bank fixings with date <= expiry. */
std::vector<QuantLib::Date> bankFixingsThroughExpiry(
    const std::vector<QuantLib::Date>& bankFixings, const QuantLib::Date& expiry);

/** @brief Last bank date in each calendar month on or before @p expiry. */
std::vector<QuantLib::Date> bankFixingsLastDatePerMonth(
    const std::vector<QuantLib::Date>& bankFixings, const QuantLib::Date& expiry);

/** @brief Last bank date in each calendar year on or before @p expiry. */
std::vector<QuantLib::Date> bankFixingsLastDatePerYear(
    const std::vector<QuantLib::Date>& bankFixings, const QuantLib::Date& expiry);

/**
 * @brief Observation dates for MC monitoring.
 * Uses @c params.observationDates when non-empty; otherwise falls back per
 * @c params.observationFrequency: monthly @c Following through @c params.expiry (default),
 * or every save-path fixing up to @c params.expiry (daily; requires a simulated bank).
 */
std::vector<QuantLib::Date> resolveMcObservationDates(const BuehlerModel& buehler,
                                                      const OptionContractParams& params,
                                                      const char* productLabel);

#endif
