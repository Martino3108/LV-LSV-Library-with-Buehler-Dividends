/**
 * @file buehler_fast_path_simulate.cpp
 */

#include "buehler_fast_path_simulate.h"
#include "buehler_fixing_path_simulate.h"
#include "buehler_mc_sigma_lookup.h"
#include "buehler_model.h"
#include <ql/math/randomnumbers/inversecumulativerng.hpp>
#include <ql/math/randomnumbers/mt19937uniformrng.hpp>
#include <ql/timegrid.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#if defined(_OPENMP)
#include <omp.h>
#endif

namespace {

using namespace QuantLib;

constexpr Size kNoSaveSlot = std::numeric_limits<Size>::max();

inline Real evolvePureXEulerStep(const Real x, const Real dt, const Real sqrtDt, const Real z,
                                 const Real vol) {
    return x * std::exp((-0.5 * vol * vol) * dt + vol * sqrtDt * z);
}

inline BigNatural threadRngSeed(const BigNatural baseSeed, const int threadId) {
    return buehlerMcThreadRngSeed(baseSeed, static_cast<unsigned>(threadId));
}

unsigned effectiveMcPathWorkers(const BuehlerMcSettings& settings, const Size nPairs) {
    return buehlerMcEffectivePathWorkers(settings.mcPathWorkers, nPairs);
}

void simulateFastLvScalarByStep(const Size nPaths,
                                const Size nSteps,
                                const Size nSaveFix,
                                const TimeGrid& evolutionGrid,
                                const BuehlerMcTimeGridSigmaLookup& sigma,
                                std::vector<BuehlerBankReal>& savedX,
                                std::vector<Real>& xState,
                                const std::vector<Size>& saveSlotAtEvolutionIndex,
                                const BigNatural seed,
                                const unsigned nWorkers,
                                const bool useOpenMpWorkers) {
    xState.assign(nPaths, 1.0);
    const Size nPairs = (nPaths + 1) / 2;

    const auto recordAtEvolutionIndex = [&](const Size path, const Size evIdx) {
        const Size slot = saveSlotAtEvolutionIndex[evIdx];
        if (slot != kNoSaveSlot)
            savedX[path * nSaveFix + slot] = static_cast<BuehlerBankReal>(xState[path]);
    };

    const auto evolvePair = [&](const Size pair,
                                InverseCumulativeRng<MersenneTwisterUniformRng, InverseCumulativeNormal>& rng,
                                const BuehlerMcSigmaTimeSlice& sigmaSlice, const Real dt,
                                const Real sqrtDt, const Size step) {
        const Real z = rng.next().value;
        const Size p0 = pair * 2;
        const Real vol0 = sigmaSlice.atX(xState[p0]);
        xState[p0] = evolvePureXEulerStep(xState[p0], dt, sqrtDt, z, vol0);
        recordAtEvolutionIndex(p0, step + 1);

        const Size p1 = p0 + 1;
        if (p1 < nPaths) {
            const Real vol1 = sigmaSlice.atX(xState[p1]);
            xState[p1] = evolvePureXEulerStep(xState[p1], dt, sqrtDt, -z, vol1);
            recordAtEvolutionIndex(p1, step + 1);
        }
    };

#if defined(_OPENMP)
    if (nWorkers > 1 && useOpenMpWorkers) {
        buehlerMcApplyOpenMpTuning();
        omp_set_num_threads(static_cast<int>(nWorkers));
#pragma omp parallel num_threads(nWorkers)
        {
            const int threadId = omp_get_thread_num();
            MersenneTwisterUniformRng uniform(
                static_cast<unsigned long>(threadRngSeed(seed, static_cast<unsigned>(threadId))));
            InverseCumulativeRng<MersenneTwisterUniformRng, InverseCumulativeNormal> rng(uniform);
            for (Size step = 0; step < nSteps; ++step) {
                const BuehlerMcSigmaTimeSlice sigmaSlice = sigma.sliceAtTime(evolutionGrid[step]);
                const Real dt = evolutionGrid.dt(step);
                const Real sqrtDt = std::sqrt(dt);
#pragma omp for schedule(static)
                for (long pair = 0; pair < static_cast<long>(nPairs); ++pair)
                    evolvePair(static_cast<Size>(pair), rng, sigmaSlice, dt, sqrtDt, step);
            }
        }
        return;
    }
#endif

    MersenneTwisterUniformRng uniform(static_cast<unsigned long>(seed));
    InverseCumulativeRng<MersenneTwisterUniformRng, InverseCumulativeNormal> rng(uniform);
    for (Size step = 0; step < nSteps; ++step) {
        const BuehlerMcSigmaTimeSlice sigmaSlice = sigma.sliceAtTime(evolutionGrid[step]);
        const Real dt = evolutionGrid.dt(step);
        const Real sqrtDt = std::sqrt(dt);
        for (Size pair = 0; pair < nPairs; ++pair)
            evolvePair(pair, rng, sigmaSlice, dt, sqrtDt, step);
    }
}

} // namespace

BuehlerFastSigmaErrorReport measureDenseFixedLvSigmaError(const BuehlerModel& buehler) {
    const ext::shared_ptr<LocalVolTermStructure> lv =
        buehler.fixedPureLocalVolTs().currentLink();
    QL_REQUIRE(!buehler.denseXStrikes().empty(),
               "measureDenseFixedLvSigmaError: empty denseXStrikes in BuehlerModel");
    const BuehlerMcTimeGridSigmaLookup lookup(lv, buehler);
    const DayCounter& dc = buehler.dayCounter();
    const Date today = buehler.today();

    BuehlerFastSigmaErrorReport report;
    Real sumErr = 0.0;
    for (const Date& d : buehler.denseExpiries()) {
        const Time t = dc.yearFraction(today, d);
        for (const Real x : buehler.denseXStrikes()) {
            const Real ql = lv->localVol(t, x, true);
            const Real fast = lookup.atTime(t, x);
            const Real err = std::fabs(fast - ql);
            report.maxAbsError = std::max(report.maxAbsError, err);
            sumErr += err;
            ++report.sampleCount;
        }
    }
    if (report.sampleCount > 0)
        report.meanAbsError = sumErr / static_cast<Real>(report.sampleCount);
    return report;
}

BuehlerFixingSavePath simulateBuehlerFixingSavePathFast(const BuehlerModel& buehler,
                                                        const Date& horizonMax,
                                                        const std::vector<Date>& evolutionDates,
                                                        const std::vector<Date>& saveFixingDates,
                                                        const BuehlerMcSettings& settings) {
    QL_REQUIRE(settings.mcSamples > 0, "simulateBuehlerFixingSavePathFast: mcSamples must be positive");
    QL_REQUIRE(!buehler.fixedPureLocalVolTs().empty(),
               "simulateBuehlerFixingSavePathFast: empty fixed pure-X local vol");
    QL_REQUIRE(!evolutionDates.empty(),
               "simulateBuehlerFixingSavePathFast: evolution dates must be non-empty");
    QL_REQUIRE(!saveFixingDates.empty(),
               "simulateBuehlerFixingSavePathFast: save fixing dates must be non-empty");
    (void)horizonMax;

    const Size nSaveFix = saveFixingDates.size();

    const DayCounter& dc = buehler.dayCounter();
    const Date today = buehler.today();

    std::vector<Time> evolutionTimes;
    evolutionTimes.reserve(evolutionDates.size());
    for (const Date& d : evolutionDates) {
        const Time t = dc.yearFraction(today, d);
        QL_REQUIRE(t >= 0.0, "simulateBuehlerFixingSavePathFast: evolution dates must not be in the past");
        evolutionTimes.push_back(t);
    }

    std::vector<Time> saveFixingTimes;
    saveFixingTimes.reserve(nSaveFix);
    for (const Date& d : saveFixingDates) {
        const Time t = dc.yearFraction(today, d);
        QL_REQUIRE(t >= 0.0, "simulateBuehlerFixingSavePathFast: save dates must not be in the past");
        saveFixingTimes.push_back(t);
    }

    const TimeGrid evolutionGrid(evolutionTimes.begin(), evolutionTimes.end());
    const Size nSteps = evolutionGrid.size() - 1;
    QL_REQUIRE(nSteps >= nSaveFix,
               "simulateBuehlerFixingSavePathFast: evolution grid must cover all save fixings");

    std::vector<Size> saveSlotAtEvolutionIndex(evolutionGrid.size(), kNoSaveSlot);
    for (Size i = 0; i < nSaveFix; ++i) {
        const Size evIdx = evolutionGrid.index(saveFixingTimes[i]);
        QL_REQUIRE(saveSlotAtEvolutionIndex[evIdx] == kNoSaveSlot,
                   "simulateBuehlerFixingSavePathFast: duplicate save fixing on evolution grid");
        saveSlotAtEvolutionIndex[evIdx] = i;
    }

    std::vector<Real> dividendCarryAtFixing(nSaveFix);
    std::vector<Real> slopeGAtFixing(nSaveFix);
    for (Size i = 0; i < nSaveFix; ++i) {
        dividendCarryAtFixing[i] = buehler.dividendCarry0T(saveFixingDates[i]);
        slopeGAtFixing[i] = buehler.forward0T(saveFixingDates[i]) - dividendCarryAtFixing[i];
        QL_REQUIRE(slopeGAtFixing[i] > 0.0, "simulateBuehlerFixingSavePathFast: G(T_i) must be positive");
    }

    QL_REQUIRE(!buehler.denseExpiries().empty(),
               "simulateBuehlerFixingSavePathFast: empty denseExpiries (run calibration)");
    QL_REQUIRE(!buehler.denseXStrikes().empty(),
               "simulateBuehlerFixingSavePathFast: empty denseXStrikes in BuehlerModel");
    const BuehlerMcTimeGridSigmaLookup& sigma = buehler.mcSigmaLookup();
    const Size nPaths = settings.mcSamples;

    std::vector<BuehlerBankReal> savedX(nPaths * nSaveFix, BuehlerBankReal(1));

    const Size nPairs = (nPaths + 1) / 2;
    const unsigned nWorkers = effectiveMcPathWorkers(settings, nPairs);

    std::vector<Real> xState;
    simulateFastLvScalarByStep(nPaths, nSteps, nSaveFix, evolutionGrid, sigma, savedX, xState,
                               saveSlotAtEvolutionIndex, settings.seed, nWorkers,
                               buehlerMcUseOpenMpWorkers(settings.mcPathWorkers));

    buehlerMcCheckMartingaleDrift(xState, settings.mcMartingaleDriftWarnThreshold, "LV fast path");

    return BuehlerFixingSavePath(saveFixingDates, nPaths, std::move(savedX),
                                 std::move(dividendCarryAtFixing), std::move(slopeGAtFixing),
                                 nSteps);
}
