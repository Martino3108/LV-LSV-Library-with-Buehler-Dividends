/**
 * @file buehler_fixing_path_simulate.cpp
 */

#include "buehler_fixing_path_simulate.h"
#include "buehler_fast_path_simulate.h"
#include "buehler_lsv_path_simulate.h"
#include "buehler_model.h"
#include "fd_buehler_x_fdm.h"
#include <ql/math/randomnumbers/rngtraits.hpp>
#include <ql/methods/montecarlo/pathgenerator.hpp>
#include <ql/timegrid.hpp>
#include <algorithm>
#include <cmath>
#include <iostream>

std::vector<QuantLib::Date> buehlerMcSimulationDatesEveryNCalendarDays(
    const BuehlerModel& buehler,
    const QuantLib::Date& horizonMax,
    const int calendarDayStep) {
    using namespace QuantLib;
    QL_REQUIRE(calendarDayStep > 0,
               "buehlerMcSimulationDatesEveryNCalendarDays: calendarDayStep must be positive");
    QL_REQUIRE(horizonMax > buehler.today(),
               "buehlerMcSimulationDatesEveryNCalendarDays: horizonMax must be after today");

    std::vector<Date> dates;
    Date d = buehler.today();
    while (true) {
        d += calendarDayStep;
        if (d > horizonMax)
            break;
        dates.push_back(d);
    }
    if (dates.empty() || dates.back() != horizonMax)
        dates.push_back(horizonMax);
    QL_REQUIRE(dates.size() >= 1,
               "buehlerMcSimulationDatesEveryNCalendarDays: empty simulation schedule");
    return dates;
}

std::vector<QuantLib::Date> normalizeSimulationDates(const BuehlerModel& buehler,
                                                     std::vector<QuantLib::Date> dates,
                                                     const QuantLib::Date& horizonMax) {
    using namespace QuantLib;
    QL_REQUIRE(!dates.empty(), "normalizeSimulationDates: simulation dates must be non-empty");
    QL_REQUIRE(horizonMax > buehler.today(),
               "normalizeSimulationDates: horizonMax must be after today");
    QL_REQUIRE(horizonMax <= buehler.maturity(),
               "normalizeSimulationDates: horizonMax must not exceed model maturity");

    std::sort(dates.begin(), dates.end());
    dates.erase(std::unique(dates.begin(), dates.end()), dates.end());
    for (const Date& d : dates) {
        QL_REQUIRE(d > buehler.today(), "normalizeSimulationDates: each date must be after today");
        QL_REQUIRE(d <= horizonMax, "normalizeSimulationDates: each date must be on or before horizonMax");
    }
    return dates;
}

std::vector<QuantLib::Date> resolveMcSavePathFixingDates(
    const std::vector<QuantLib::Date>& evolutionDates,
    const std::vector<QuantLib::Date>& requestedSaveDates) {
    using namespace QuantLib;
    QL_REQUIRE(!evolutionDates.empty(), "resolveMcSavePathFixingDates: empty evolution dates");
    if (requestedSaveDates.empty())
        return evolutionDates;

    std::vector<Date> saveDates = requestedSaveDates;
    std::sort(saveDates.begin(), saveDates.end());
    saveDates.erase(std::unique(saveDates.begin(), saveDates.end()), saveDates.end());
    for (const Date& d : saveDates) {
        QL_REQUIRE(std::binary_search(evolutionDates.begin(), evolutionDates.end(), d),
                   "resolveMcSavePathFixingDates: save date " << d
                                                              << " is not on the evolution grid");
    }
    return saveDates;
}

void buehlerMcCheckMartingaleDrift(const std::vector<QuantLib::Real>& xStateAtHorizon,
                                   const QuantLib::Real warnThreshold,
                                   const char* tag) {
    using namespace QuantLib;
    if (warnThreshold <= 0.0 || xStateAtHorizon.empty())
        return;
    Real sum = 0.0;
    for (const Real x : xStateAtHorizon)
        sum += x;
    const Real meanX = sum / static_cast<Real>(xStateAtHorizon.size());
    const Real drift = std::fabs(meanX - 1.0);
    if (drift > warnThreshold) {
        std::cerr << "[" << tag << " martingale warning] |E[X_T]-1| = " << drift
                  << " exceeds threshold " << warnThreshold << " (mean X_T = " << meanX
                  << ", paths = " << xStateAtHorizon.size()
                  << "); check bins/step-size or raise mcSamples\n";
    }
}

BuehlerFixingSavePath simulateBuehlerFixingSavePath(const BuehlerModel& buehler,
                                                    const QuantLib::Date& horizonMax,
                                                    const std::vector<QuantLib::Date>& simulationDates,
                                                    const BuehlerMcSettings& settings) {
    using namespace QuantLib;

    QL_REQUIRE(settings.mcSamples > 0, "simulateBuehlerFixingSavePath: mcSamples must be positive");
    QL_REQUIRE(!buehler.fixedPureLocalVolTs().empty(),
               "simulateBuehlerFixingSavePath: empty fixed pure-X local vol (run calibration)");

    const BuehlerMcDynamics dynamics =
        resolveBuehlerMcDynamics(settings.dynamics, buehler.hasLsvCalibration());

    const std::vector<Date> evolutionDates =
        normalizeSimulationDates(buehler, simulationDates, horizonMax);
    const std::vector<Date> saveFixingDates =
        resolveMcSavePathFixingDates(evolutionDates, settings.mcSavePathFixingDates);

    if (dynamics == BuehlerMcDynamics::Lsv || dynamics == BuehlerMcDynamics::Bergomi) {
        QL_REQUIRE(buehler.hasLsvCalibration(),
                   "simulateBuehlerFixingSavePath: LSV/Bergomi dynamics requires Bergomi params");
        return simulateBuehlerFixingSavePathLsv(buehler, horizonMax, evolutionDates,
                                                saveFixingDates, settings);
    }

    if (settings.useFastPathSimulator)
        return simulateBuehlerFixingSavePathFast(buehler, horizonMax, evolutionDates,
                                                 saveFixingDates, settings);

    const Size nSaveFix = saveFixingDates.size();
    const auto process = makeBuehlerPureXLocalVolProcess(buehler);

    std::vector<Time> evolutionTimes;
    evolutionTimes.reserve(evolutionDates.size());
    for (const Date& d : evolutionDates) {
        const Time t = process->time(d);
        QL_REQUIRE(t >= 0.0, "simulateBuehlerFixingSavePath: simulation dates must not be in the past");
        evolutionTimes.push_back(t);
    }

    std::vector<Time> saveFixingTimes;
    saveFixingTimes.reserve(nSaveFix);
    for (const Date& d : saveFixingDates) {
        const Time t = process->time(d);
        QL_REQUIRE(t >= 0.0, "simulateBuehlerFixingSavePath: save dates must not be in the past");
        saveFixingTimes.push_back(t);
    }

    const TimeGrid evolutionGrid(evolutionTimes.begin(), evolutionTimes.end());
    const Size mcBrownianSteps = evolutionGrid.size() - 1;
    QL_REQUIRE(mcBrownianSteps >= nSaveFix,
               "simulateBuehlerFixingSavePath: evolution grid must cover all save fixings");

    std::vector<Size> evolutionIndexAtSaveFixing;
    evolutionIndexAtSaveFixing.reserve(nSaveFix);
    for (const Time t : saveFixingTimes)
        evolutionIndexAtSaveFixing.push_back(evolutionGrid.index(t));

    std::vector<Real> dividendCarryAtFixing(nSaveFix);
    std::vector<Real> slopeGAtFixing(nSaveFix);
    for (Size i = 0; i < nSaveFix; ++i) {
        dividendCarryAtFixing[i] = buehler.dividendCarry0T(saveFixingDates[i]);
        slopeGAtFixing[i] = buehler.forward0T(saveFixingDates[i]) - dividendCarryAtFixing[i];
        QL_REQUIRE(slopeGAtFixing[i] > 0.0, "simulateBuehlerFixingSavePath: G(T_i) must be positive");
    }

    typedef PseudoRandom::rsg_type rsg_type;
    typedef PathGenerator<rsg_type> path_generator_type;
    const rsg_type generator = PseudoRandom::make_sequence_generator(
        process->factors() * (evolutionGrid.size() - 1), settings.seed);
    const path_generator_type pathGenerator(process, evolutionGrid, generator, false);

    std::vector<BuehlerBankReal> savedX(static_cast<Size>(settings.mcSamples) * nSaveFix,
                                        BuehlerBankReal(1));
    std::vector<Real> xAtHorizon(settings.mcSamples, 1.0);
    const Size lastEvolutionIndex = evolutionGrid.size() - 1;

    Size pathCount = 0;
    while (pathCount < settings.mcSamples) {
        const Path& fullPath = pathGenerator.next().value;
        for (Size i = 0; i < nSaveFix; ++i)
            savedX[pathCount * nSaveFix + i] =
                static_cast<BuehlerBankReal>(fullPath[evolutionIndexAtSaveFixing[i]]);
        xAtHorizon[pathCount] = fullPath[lastEvolutionIndex];
        ++pathCount;
        if (pathCount >= settings.mcSamples)
            break;

        const Path& antiPath = pathGenerator.antithetic().value;
        for (Size i = 0; i < nSaveFix; ++i)
            savedX[pathCount * nSaveFix + i] =
                static_cast<BuehlerBankReal>(antiPath[evolutionIndexAtSaveFixing[i]]);
        xAtHorizon[pathCount] = antiPath[lastEvolutionIndex];
        ++pathCount;
    }

    buehlerMcCheckMartingaleDrift(xAtHorizon, settings.mcMartingaleDriftWarnThreshold,
                                  "LV PathGenerator");

    return BuehlerFixingSavePath(saveFixingDates, settings.mcSamples, std::move(savedX),
                                 std::move(dividendCarryAtFixing), std::move(slopeGAtFixing),
                                 mcBrownianSteps);
}
