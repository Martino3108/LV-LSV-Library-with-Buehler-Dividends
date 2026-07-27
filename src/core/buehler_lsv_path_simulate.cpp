/**
 * @file buehler_lsv_path_simulate.cpp
 */

#include "buehler_lsv_path_simulate.h"
#include "buehler_fixing_path_simulate.h"
#include "buehler_mc_sigma_lookup.h"
#include "buehler_model.h"
#include <ql/math/randomnumbers/inversecumulativerng.hpp>
#include <ql/math/randomnumbers/mt19937uniformrng.hpp>
#include <ql/timegrid.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <vector>

#if defined(_OPENMP)
#include <omp.h>
#endif

#if defined(__APPLE__)
// vForce batched exp (Accelerate). Declared directly instead of including the
// Accelerate umbrella header, whose MacTypes.h `Size` typedef collides with
// QuantLib::Size under `using namespace QuantLib`.
extern "C" void vvexp(double* y, const double* x, const int* n);
#endif

namespace {

using namespace QuantLib;

/**
 * @brief expNuY[j] = exp(nu * y[j]) for j in [lo, hi).
 *
 * Batched through Accelerate's vForce (vvexp, in-place) on Apple; scalar libm elsewhere.
 * Uses expNuY itself as the argument buffer, so no extra allocation (RAM-neutral). The
 * per-path exp inside the Euler X-update stays scalar on purpose: it is interleaved with
 * the RNG draws and batching it would change the draw-to-path assignment.
 */
void expNuYRange(const Real nu,
                 const std::vector<Real>& y,
                 const Size lo,
                 const Size hi,
                 std::vector<Real>& expNuY) {
    for (Size j = lo; j < hi; ++j)
        expNuY[j] = nu * y[j];
#if defined(__APPLE__)
    const int n = static_cast<int>(hi - lo);
    if (n > 0)
        vvexp(expNuY.data() + lo, expNuY.data() + lo, &n);
#else
    for (Size j = lo; j < hi; ++j)
        expNuY[j] = std::exp(expNuY[j]);
#endif
}

/** @brief Equal-width bins in @e X; conditional mean via linear interp on bin midpoints.
 *  Writes into the caller-owned @p gHat scratch (resized on first use, reused across steps). */
void equidistantBinsConditionalMean(const std::vector<Real>& xAtStep,
                                    const std::vector<Real>& targetAtStep,
                                    const Size numBins,
                                    std::vector<Real>& gHat) {
    QL_REQUIRE(numBins > 0, "equidistantBinsConditionalMean: numBins must be positive");
    QL_REQUIRE(xAtStep.size() == targetAtStep.size(),
               "equidistantBinsConditionalMean: x/target size mismatch");
    const Size n = xAtStep.size();
    QL_REQUIRE(n > 0, "equidistantBinsConditionalMean: empty path cloud");

    Real xMin = xAtStep[0];
    Real xMax = xAtStep[0];
    for (Size j = 1; j < n; ++j) {
        xMin = std::min(xMin, xAtStep[j]);
        xMax = std::max(xMax, xAtStep[j]);
    }

    const Real width = xMax - xMin;
    if (width <= QL_EPSILON) {
        Real meanTarget = 0.0;
        for (Real v : targetAtStep)
            meanTarget += v;
        meanTarget /= static_cast<Real>(n);
        gHat.assign(n, std::max(meanTarget, QL_EPSILON));
        return;
    }

    std::vector<Real> sumTarget(numBins, 0.0);
    std::vector<Size> count(numBins, 0);

    const Real binWidth = width / static_cast<Real>(numBins);
    for (Size j = 0; j < n; ++j) {
        Real frac = (xAtStep[j] - xMin) / width;
        if (frac >= 1.0)
            frac = 1.0 - QL_EPSILON;
        Size k = static_cast<Size>(frac * static_cast<Real>(numBins));
        if (k >= numBins)
            k = numBins - 1;
        sumTarget[k] += targetAtStep[j];
        ++count[k];
    }

    Real globalMean = 0.0;
    for (Real v : targetAtStep)
        globalMean += v;
    globalMean = std::max(globalMean / static_cast<Real>(n), QL_EPSILON);

    std::vector<Real> binMean(numBins, globalMean);
    for (Size k = 0; k < numBins; ++k) {
        if (count[k] > 0)
            binMean[k] = std::max(sumTarget[k] / static_cast<Real>(count[k]), QL_EPSILON);
    }

    const auto gAtX = [&](const Real x) -> Real {
        const Real xLeftMid = xMin + 0.5 * binWidth;
        const Real xRightMid = xMin + (static_cast<Real>(numBins) - 0.5) * binWidth;
        if (x <= xLeftMid)
            return binMean[0];
        if (x >= xRightMid)
            return binMean[numBins - 1];
        if (numBins == 1)
            return binMean[0];
        Size k = static_cast<Size>((x - xMin) / binWidth - 0.5);
        if (k >= numBins - 1)
            k = numBins - 2;
        const Real xMidK = xMin + (static_cast<Real>(k) + 0.5) * binWidth;
        const Real t = (x - xMidK) / binWidth;
        return std::max(binMean[k] + t * (binMean[k + 1] - binMean[k]), QL_EPSILON);
    };

    gHat.resize(n);
    for (Size j = 0; j < n; ++j)
        gHat[j] = gAtX(xAtStep[j]);
}

/** @brief Adaptive bins with lockstep pooling: min/max and sum/count over path chunks at one step. */
void equidistantBinsConditionalMeanPooled(const std::vector<Real>& xAtStep,
                                          const std::vector<Real>& targetAtStep,
                                          const Size numBins,
                                          const Size chunkSamples,
                                          std::vector<Real>& gHat) {
    QL_REQUIRE(numBins > 0, "equidistantBinsConditionalMeanPooled: numBins must be positive");
    QL_REQUIRE(xAtStep.size() == targetAtStep.size(),
               "equidistantBinsConditionalMeanPooled: x/target size mismatch");
    const Size n = xAtStep.size();
    QL_REQUIRE(n > 0, "equidistantBinsConditionalMeanPooled: empty path cloud");
    if (chunkSamples == 0 || chunkSamples >= n) {
        equidistantBinsConditionalMean(xAtStep, targetAtStep, numBins, gHat);
        return;
    }

    Real xMin = xAtStep[0];
    Real xMax = xAtStep[0];
    for (Size off = 0; off < n; off += chunkSamples) {
        const Size end = std::min(off + chunkSamples, n);
        for (Size j = off; j < end; ++j) {
            xMin = std::min(xMin, xAtStep[j]);
            xMax = std::max(xMax, xAtStep[j]);
        }
    }

    const Real width = xMax - xMin;
    if (width <= QL_EPSILON) {
        Real meanTarget = 0.0;
        for (Real v : targetAtStep)
            meanTarget += v;
        meanTarget /= static_cast<Real>(n);
        gHat.assign(n, std::max(meanTarget, QL_EPSILON));
        return;
    }

    std::vector<Real> sumTarget(numBins, 0.0);
    std::vector<Size> count(numBins, 0);
    const Real binWidth = width / static_cast<Real>(numBins);
    for (Size off = 0; off < n; off += chunkSamples) {
        const Size end = std::min(off + chunkSamples, n);
        for (Size j = off; j < end; ++j) {
            Real frac = (xAtStep[j] - xMin) / width;
            if (frac >= 1.0)
                frac = 1.0 - QL_EPSILON;
            Size k = static_cast<Size>(frac * static_cast<Real>(numBins));
            if (k >= numBins)
                k = numBins - 1;
            sumTarget[k] += targetAtStep[j];
            ++count[k];
        }
    }

    Real globalMean = 0.0;
    for (Real v : targetAtStep)
        globalMean += v;
    globalMean = std::max(globalMean / static_cast<Real>(n), QL_EPSILON);

    std::vector<Real> binMean(numBins, globalMean);
    for (Size k = 0; k < numBins; ++k) {
        if (count[k] > 0)
            binMean[k] = std::max(sumTarget[k] / static_cast<Real>(count[k]), QL_EPSILON);
    }

    const auto gAtX = [&](const Real x) -> Real {
        const Real xLeftMid = xMin + 0.5 * binWidth;
        const Real xRightMid = xMin + (static_cast<Real>(numBins) - 0.5) * binWidth;
        if (x <= xLeftMid)
            return binMean[0];
        if (x >= xRightMid)
            return binMean[numBins - 1];
        if (numBins == 1)
            return binMean[0];
        Size k = static_cast<Size>((x - xMin) / binWidth - 0.5);
        if (k >= numBins - 1)
            k = numBins - 2;
        const Real xMidK = xMin + (static_cast<Real>(k) + 0.5) * binWidth;
        const Real t = (x - xMidK) / binWidth;
        return std::max(binMean[k] + t * (binMean[k + 1] - binMean[k]), QL_EPSILON);
    };

    gHat.resize(n);
    for (Size j = 0; j < n; ++j)
        gHat[j] = gAtX(xAtStep[j]);
}

struct LsvBinStepContext {
    Size pathChunkSamples = 0;
};

void computeLsvGHatAtStep(const LsvBinStepContext& ctx,
                          const std::vector<Real>& xState,
                          const std::vector<Real>& exp2NuY,
                          const Size lsvBins,
                          std::vector<Real>& gHat) {
    if (ctx.pathChunkSamples > 0 && ctx.pathChunkSamples < xState.size())
        equidistantBinsConditionalMeanPooled(xState, exp2NuY, lsvBins, ctx.pathChunkSamples, gHat);
    else
        equidistantBinsConditionalMean(xState, exp2NuY, lsvBins, gHat);
}

/** Var(Y_t) of the driftless OU driver started at Y_0 = 0. */
Real bergomiOuVariance(const Real k, const Time t) {
    if (t <= 0.0)
        return 0.0;
    if (k <= QL_EPSILON)
        return t;
    return (1.0 - std::exp(-2.0 * k * t)) / (2.0 * k);
}

/**
 * Lognormal drift correction for the pure-driver vol factor: with
 * sigma_t = exp(nu*Y_t - c_t) and E[e^{2 nu Y_t}] = e^{2 nu^2 Var(Y_t)},
 * E[sigma_t^2] = 1 requires c_t = nu^2 * Var(Y_t) (the nu^2 factor matters
 * whenever nu != 1).
 */
Real bergomiForwardVarianceOffset(const Real nu, const Real k, const Time t) {
    return nu * nu * bergomiOuVariance(k, t);
}

/** @brief Per-step constants hoisted out of the per-path Euler kernel (identical for all paths). */
struct LsvStepConstants {
    Real dt = 0.0;
    Real sqrtDt = 0.0;
    bool ouMeanReverting = false;
    Real expNegKDt = 1.0; ///< exp(-k*dt), used when mean-reverting
    Real ouStd = 0.0;     ///< std-dev of the exact OU shock over dt
    Real cT = 0.0;        ///< Bergomi forward-variance offset at tEval (pure-driver mode only)
};

LsvStepConstants makeLsvStepConstants(const BuehlerBergomiParams& bergomi,
                                      const Time tEval,
                                      const Real dt,
                                      const bool pureDriverOnX) {
    LsvStepConstants c;
    c.dt = dt;
    c.sqrtDt = std::sqrt(dt);
    c.ouMeanReverting = bergomi.k > QL_EPSILON;
    if (c.ouMeanReverting) {
        c.expNegKDt = std::exp(-bergomi.k * dt);
        c.ouStd = std::sqrt((1.0 - std::exp(-2.0 * bergomi.k * dt)) / (2.0 * bergomi.k));
    } else {
        c.ouStd = c.sqrtDt;
    }
    if (pureDriverOnX)
        c.cT = bergomiForwardVarianceOffset(bergomi.nu, bergomi.k, tEval);
    return c;
}

void evolveLsvEulerStep(const BuehlerBergomiParams& bergomi,
                        const LsvStepConstants& c,
                        const Real zX,
                        const Real zY,
                        const Real sigmaLv,
                        const Real expNuY,
                        const Real gHat,
                        const bool pureDriverOnX,
                        Real& x,
                        Real& y) {
    QL_REQUIRE(gHat > 0.0, "evolveLsvEulerStep: conditional mean must be positive");
    QL_REQUIRE(sigmaLv > 0.0, "evolveLsvEulerStep: local vol must be positive");

    Real volX;
    if (pureDriverOnX) {
        volX = std::exp(bergomi.nu * y - c.cT);
    } else {
        volX = sigmaLv * expNuY / std::sqrt(gHat);
    }
    x = x * std::exp((-0.5 * volX * volX) * c.dt + volX * c.sqrtDt * zX);

    if (c.ouMeanReverting)
        y = y * c.expNegKDt + c.ouStd * zY;
    else
        y += c.ouStd * zY;
}

inline Real correlatedStandardNormal(const Real z1, const Real rho, const Real z2Indep) {
    const Real rhoClamped = std::max(-1.0 + QL_EPSILON, std::min(1.0 - QL_EPSILON, rho));
    return rhoClamped * z1 + std::sqrt(1.0 - rhoClamped * rhoClamped) * z2Indep;
}

} // namespace

BuehlerFixingSavePath simulateBuehlerFixingSavePathLsv(const BuehlerModel& buehler,
                                                         const Date& horizonMax,
                                                         const std::vector<Date>& evolutionDates,
                                                         const std::vector<Date>& saveFixingDates,
                                                         const BuehlerMcSettings& settings) {
    QL_REQUIRE(settings.mcSamples > 0,
               "simulateBuehlerFixingSavePathLsv: mcSamples must be positive");
    QL_REQUIRE(buehler.hasLsvCalibration(),
               "simulateBuehlerFixingSavePathLsv: Bergomi params not set on model");
    QL_REQUIRE(!buehler.fixedPureLocalVolTs().empty(),
               "simulateBuehlerFixingSavePathLsv: empty fixed pure-X local vol (run calibration)");
    QL_REQUIRE(settings.lsvBins > 0,
               "simulateBuehlerFixingSavePathLsv: lsvBins must be positive");
    QL_REQUIRE(!buehler.denseExpiries().empty(),
               "simulateBuehlerFixingSavePathLsv: empty denseExpiries (run calibration)");
    QL_REQUIRE(!buehler.denseXStrikes().empty(),
               "simulateBuehlerFixingSavePathLsv: empty denseXStrikes in BuehlerModel");
    QL_REQUIRE(!evolutionDates.empty(),
               "simulateBuehlerFixingSavePathLsv: evolution dates must be non-empty");
    QL_REQUIRE(!saveFixingDates.empty(),
               "simulateBuehlerFixingSavePathLsv: save fixing dates must be non-empty");
    (void)horizonMax;

    const bool pureBergomiOnX = settings.dynamics == BuehlerMcDynamics::Bergomi;
    const bool unitLocalVol = pureBergomiOnX || settings.lsvUnitLocalVol;
    const bool unitLeverage = pureBergomiOnX || settings.lsvForceUnitLeverage;
    const bool pureDriverOnX = unitLocalVol && unitLeverage;

    const BuehlerMcTimeGridSigmaLookup* sigma = unitLocalVol ? nullptr : &buehler.mcSigmaLookup();

    const BuehlerBergomiParams bergomi = buehler.bergomiParams();

    const Size nSaveFix = saveFixingDates.size();

    const DayCounter& dc = buehler.dayCounter();
    const Date today = buehler.today();
    std::vector<Time> evolutionTimes;
    evolutionTimes.reserve(evolutionDates.size());
    for (const Date& d : evolutionDates) {
        const Time t = dc.yearFraction(today, d);
        QL_REQUIRE(t >= 0.0, "simulateBuehlerFixingSavePathLsv: dates must not be in the past");
        evolutionTimes.push_back(t);
    }

    std::vector<Time> saveFixingTimes;
    saveFixingTimes.reserve(nSaveFix);
    for (const Date& d : saveFixingDates) {
        const Time t = dc.yearFraction(today, d);
        QL_REQUIRE(t >= 0.0, "simulateBuehlerFixingSavePathLsv: save dates must not be in the past");
        saveFixingTimes.push_back(t);
    }

    const TimeGrid evolutionGrid(evolutionTimes.begin(), evolutionTimes.end());
    const Size mcBrownianSteps = evolutionGrid.size() - 1;
    QL_REQUIRE(mcBrownianSteps >= nSaveFix,
               "simulateBuehlerFixingSavePathLsv: evolution grid must cover all save fixings");

    constexpr Size kNoSaveSlot = std::numeric_limits<Size>::max();
    std::vector<Size> saveSlotAtEvolutionIndex(evolutionGrid.size(), kNoSaveSlot);
    for (Size i = 0; i < nSaveFix; ++i) {
        const Size evIdx = evolutionGrid.index(saveFixingTimes[i]);
        QL_REQUIRE(saveSlotAtEvolutionIndex[evIdx] == kNoSaveSlot,
                   "simulateBuehlerFixingSavePathLsv: duplicate save fixing on evolution grid");
        saveSlotAtEvolutionIndex[evIdx] = i;
    }

    std::vector<Real> dividendCarryAtFixing(nSaveFix);
    std::vector<Real> slopeGAtFixing(nSaveFix);
    for (Size i = 0; i < nSaveFix; ++i) {
        dividendCarryAtFixing[i] = buehler.dividendCarry0T(saveFixingDates[i]);
        slopeGAtFixing[i] = buehler.forward0T(saveFixingDates[i]) - dividendCarryAtFixing[i];
        QL_REQUIRE(slopeGAtFixing[i] > 0.0,
                   "simulateBuehlerFixingSavePathLsv: G(T_i) must be positive");
    }

    const Size nPaths = settings.mcSamples;
    const Size nSteps = mcBrownianSteps;
    if (settings.lsvPathChunkSamples > 0)
        QL_REQUIRE(settings.lsvPathChunkSamples <= nPaths,
                   "simulateBuehlerFixingSavePathLsv: lsvPathChunkSamples exceeds mcSamples");

    LsvBinStepContext binCtx;
    binCtx.pathChunkSamples = settings.lsvPathChunkSamples;

    const Size nPairs = (nPaths + 1) / 2;
    const unsigned lsvWorkers =
        settings.lsvUseParallelSimulator
            ? buehlerMcEffectivePathWorkers(settings.mcPathWorkers, nPairs)
            : 1u;
    const bool useOpenMpWorkers =
        settings.lsvUseParallelSimulator && buehlerMcUseOpenMpWorkers(settings.mcPathWorkers);

    std::vector<Real> xState(nPaths, 1.0);
    std::vector<Real> yState(nPaths, 0.0);
    std::vector<BuehlerBankReal> savedX(nPaths * nSaveFix, BuehlerBankReal(1));
    std::vector<Real> gHat(nPaths, 1.0);
    // expNuY = exp(nu*y) per path, refreshed each step before evolve and squared for the
    // bins target; initial value 1.0 matches y=0 at step 0 (bins are skipped there).
    std::vector<Real> expNuY(nPaths, 1.0);
    std::vector<Real> exp2NuY(nPaths);

    const auto recordAtEvolutionIndex = [&](const Size path, const Size evIdx) {
        const Size slot = saveSlotAtEvolutionIndex[evIdx];
        if (slot != kNoSaveSlot)
            savedX[path * nSaveFix + slot] = static_cast<BuehlerBankReal>(xState[path]);
    };

    using clock = std::chrono::steady_clock;
    double binsMs = 0.0;
    double evolveMs = 0.0;

    const auto evolvePairAtStep = [&](const Size pair,
                                    const Real zX,
                                    const Real zY,
                                    const LsvStepConstants& stepC,
                                    const Size step,
                                    const BuehlerMcSigmaTimeSlice* sigmaSlice) {
        const Size p0 = pair * 2;
        const Real sigmaLv0 = unitLocalVol ? 1.0 : sigmaSlice->atX(xState[p0]);
        evolveLsvEulerStep(bergomi, stepC, zX, zY, sigmaLv0, expNuY[p0], gHat[p0], pureDriverOnX,
                           xState[p0], yState[p0]);
        recordAtEvolutionIndex(p0, step + 1);

        const Size p1 = p0 + 1;
        if (p1 < nPaths) {
            const Real sigmaLv1 = unitLocalVol ? 1.0 : sigmaSlice->atX(xState[p1]);
            evolveLsvEulerStep(bergomi, stepC, -zX, -zY, sigmaLv1, expNuY[p1], gHat[p1],
                               pureDriverOnX, xState[p1], yState[p1]);
            recordAtEvolutionIndex(p1, step + 1);
        }
    };

#if defined(_OPENMP)
    const bool useParallelTeam = lsvWorkers > 1 && useOpenMpWorkers;

    if (useParallelTeam) {
        // Team-parallel bins scratch: per-thread partials merged in thread order
        // (deterministic for a fixed worker count; replaces the serial `omp single` pass).
        const Size numBins = settings.lsvBins;
        std::vector<Real> threadMinX(lsvWorkers), threadMaxX(lsvWorkers),
            threadTargetSum(lsvWorkers);
        std::vector<Real> threadBinSum(static_cast<Size>(lsvWorkers) * numBins);
        std::vector<long long> threadBinCount(static_cast<Size>(lsvWorkers) * numBins);
        std::vector<Real> teamBinMean(numBins);
        struct BinsShared {
            Real xMin = 0.0;
            Real xMax = 0.0;
            Real binWidth = 0.0;
            Real globalMean = 0.0;
            bool degenerate = false;
        } binsShared;

        /** Executed by every thread of the team; barriers keep phases in lockstep. */
        const auto teamBinsAtStep = [&](const int threadId, const int nThreads) {
            const Size n = nPaths;
            const Size lo = (n * static_cast<Size>(threadId)) / static_cast<Size>(nThreads);
            const Size hi = (n * (static_cast<Size>(threadId) + 1)) / static_cast<Size>(nThreads);

            // One exp shared between bins target (e^2) and evolve vol (e), batched per chunk.
            expNuYRange(bergomi.nu, yState, lo, hi, expNuY);
            Real mn = std::numeric_limits<Real>::max();
            Real mx = std::numeric_limits<Real>::lowest();
            Real tsum = 0.0;
            for (Size j = lo; j < hi; ++j) {
                const Real e = expNuY[j];
                exp2NuY[j] = e * e;
                mn = std::min(mn, xState[j]);
                mx = std::max(mx, xState[j]);
                tsum += exp2NuY[j];
            }
            threadMinX[threadId] = mn;
            threadMaxX[threadId] = mx;
            threadTargetSum[threadId] = tsum;
#pragma omp barrier
#pragma omp single
            {
                Real xMin = std::numeric_limits<Real>::max();
                Real xMax = std::numeric_limits<Real>::lowest();
                Real gsum = 0.0;
                for (int t = 0; t < nThreads; ++t) {
                    xMin = std::min(xMin, threadMinX[t]);
                    xMax = std::max(xMax, threadMaxX[t]);
                    gsum += threadTargetSum[t];
                }
                binsShared.xMin = xMin;
                binsShared.xMax = xMax;
                const Real width = xMax - xMin;
                binsShared.degenerate = width <= QL_EPSILON;
                binsShared.globalMean = std::max(gsum / static_cast<Real>(n), QL_EPSILON);
                binsShared.binWidth =
                    binsShared.degenerate ? 0.0 : width / static_cast<Real>(numBins);
            } // implicit barrier

            if (binsShared.degenerate) {
                for (Size j = lo; j < hi; ++j)
                    gHat[j] = binsShared.globalMean;
                return;
            }

            Real* binSum = threadBinSum.data() + static_cast<Size>(threadId) * numBins;
            long long* binCount = threadBinCount.data() + static_cast<Size>(threadId) * numBins;
            std::fill_n(binSum, numBins, 0.0);
            std::fill_n(binCount, numBins, 0);
            const Real width = binsShared.xMax - binsShared.xMin;
            for (Size j = lo; j < hi; ++j) {
                Real frac = (xState[j] - binsShared.xMin) / width;
                if (frac >= 1.0)
                    frac = 1.0 - QL_EPSILON;
                Size k = static_cast<Size>(frac * static_cast<Real>(numBins));
                if (k >= numBins)
                    k = numBins - 1;
                binSum[k] += exp2NuY[j];
                ++binCount[k];
            }
#pragma omp barrier
#pragma omp for schedule(static)
            for (long k = 0; k < static_cast<long>(numBins); ++k) {
                Real sum = 0.0;
                long long cnt = 0;
                for (int t = 0; t < nThreads; ++t) {
                    sum += threadBinSum[static_cast<Size>(t) * numBins + static_cast<Size>(k)];
                    cnt += threadBinCount[static_cast<Size>(t) * numBins + static_cast<Size>(k)];
                }
                teamBinMean[static_cast<Size>(k)] =
                    cnt > 0 ? std::max(sum / static_cast<Real>(cnt), QL_EPSILON)
                            : binsShared.globalMean;
            } // implicit barrier

            const Real xMin = binsShared.xMin;
            const Real binWidth = binsShared.binWidth;
            const Real xLeftMid = xMin + 0.5 * binWidth;
            const Real xRightMid = xMin + (static_cast<Real>(numBins) - 0.5) * binWidth;
            for (Size j = lo; j < hi; ++j) {
                const Real x = xState[j];
                if (x <= xLeftMid || numBins == 1) {
                    gHat[j] = teamBinMean[0];
                    continue;
                }
                if (x >= xRightMid) {
                    gHat[j] = teamBinMean[numBins - 1];
                    continue;
                }
                Size k = static_cast<Size>((x - xMin) / binWidth - 0.5);
                if (k >= numBins - 1)
                    k = numBins - 2;
                const Real xMidK = xMin + (static_cast<Real>(k) + 0.5) * binWidth;
                const Real t = (x - xMidK) / binWidth;
                gHat[j] =
                    std::max(teamBinMean[k] + t * (teamBinMean[k + 1] - teamBinMean[k]), QL_EPSILON);
            }
        };

        const auto runBinsAtStep = [&](const Size step) {
            if (unitLeverage) {
                std::fill(gHat.begin(), gHat.end(), 1.0);
                if (!pureDriverOnX && step > 0)
                    expNuYRange(bergomi.nu, yState, 0, nPaths, expNuY);
                return;
            }
            if (step > 0) {
                expNuYRange(bergomi.nu, yState, 0, nPaths, expNuY);
                for (Size j = 0; j < nPaths; ++j)
                    exp2NuY[j] = expNuY[j] * expNuY[j];
                const clock::time_point t0Bins = clock::now();
                computeLsvGHatAtStep(binCtx, xState, exp2NuY, settings.lsvBins, gHat);
                const double stepBinsMs =
                    std::chrono::duration<double, std::milli>(clock::now() - t0Bins).count();
#pragma omp atomic
                binsMs += stepBinsMs;
            } else {
                std::fill(gHat.begin(), gHat.end(), 1.0);
            }
        };

        clock::time_point t0EvolveStep;
        clock::time_point t0BinsStep;
        buehlerMcApplyOpenMpTuning();
        omp_set_num_threads(static_cast<int>(lsvWorkers));
#pragma omp parallel num_threads(lsvWorkers)
        {
            const int threadId = omp_get_thread_num();
            const int nThreads = omp_get_num_threads();
            MersenneTwisterUniformRng uniform(static_cast<unsigned long>(
                buehlerMcThreadRngSeed(settings.seed, static_cast<unsigned>(threadId))));
            InverseCumulativeRng<MersenneTwisterUniformRng, InverseCumulativeNormal> threadRng(
                uniform);

            for (Size step = 0; step < nSteps; ++step) {
                const Time tEval = evolutionGrid[step];
                const Real dt = evolutionGrid.dt(step);
                const LsvStepConstants stepC =
                    makeLsvStepConstants(bergomi, tEval, dt, pureDriverOnX);
                BuehlerMcSigmaTimeSlice sigmaSlice;
                if (!unitLocalVol)
                    sigmaSlice = sigma->sliceAtTime(tEval);

                if (unitLeverage || step == 0 || binCtx.pathChunkSamples > 0) {
#pragma omp single
                    { runBinsAtStep(step); }
#pragma omp barrier
                } else {
#pragma omp single
                    { t0BinsStep = clock::now(); } // implicit barrier
                    teamBinsAtStep(threadId, nThreads);
#pragma omp barrier
#pragma omp single
                    {
                        binsMs += std::chrono::duration<double, std::milli>(clock::now() -
                                                                            t0BinsStep)
                                      .count();
                    } // implicit barrier
                }

#pragma omp single
                { t0EvolveStep = clock::now(); }
#pragma omp barrier

#pragma omp for schedule(static)
                for (long pair = 0; pair < static_cast<long>(nPairs); ++pair) {
                    const Real z1 = threadRng.next().value;
                    const Real z2 = threadRng.next().value;
                    const Real zY = z1;
                    const Real zX = correlatedStandardNormal(z1, bergomi.rho, z2);
                    evolvePairAtStep(static_cast<Size>(pair), zX, zY, stepC, step,
                                     &sigmaSlice);
                }

#pragma omp single
                {
                    evolveMs += std::chrono::duration<double, std::milli>(clock::now() - t0EvolveStep)
                                    .count();
                }
            }
        }
    } else
#endif
    {
        MersenneTwisterUniformRng uniform(static_cast<unsigned long>(settings.seed));
        InverseCumulativeRng<MersenneTwisterUniformRng, InverseCumulativeNormal> rng(uniform);

        for (Size step = 0; step < nSteps; ++step) {
            const Time tEval = evolutionGrid[step];
            const Real dt = evolutionGrid.dt(step);
            const LsvStepConstants stepC = makeLsvStepConstants(bergomi, tEval, dt, pureDriverOnX);
            BuehlerMcSigmaTimeSlice sigmaSlice;
            if (!unitLocalVol)
                sigmaSlice = sigma->sliceAtTime(tEval);

            if (unitLeverage) {
                std::fill(gHat.begin(), gHat.end(), 1.0);
                if (!pureDriverOnX && step > 0)
                    expNuYRange(bergomi.nu, yState, 0, nPaths, expNuY);
            } else if (step > 0) {
                expNuYRange(bergomi.nu, yState, 0, nPaths, expNuY);
                for (Size j = 0; j < nPaths; ++j)
                    exp2NuY[j] = expNuY[j] * expNuY[j];

                const clock::time_point t0Bins = clock::now();
                computeLsvGHatAtStep(binCtx, xState, exp2NuY, settings.lsvBins, gHat);
                binsMs += std::chrono::duration<double, std::milli>(clock::now() - t0Bins).count();
            } else {
                std::fill(gHat.begin(), gHat.end(), 1.0);
            }

            const clock::time_point t0Evolve = clock::now();
            for (Size pair = 0; pair < nPairs; ++pair) {
                const Real z1 = rng.next().value;
                const Real z2 = rng.next().value;
                const Real zY = z1;
                const Real zX = correlatedStandardNormal(z1, bergomi.rho, z2);
                evolvePairAtStep(pair, zX, zY, stepC, step, &sigmaSlice);
            }
            evolveMs += std::chrono::duration<double, std::milli>(clock::now() - t0Evolve).count();
        }
    }

    buehlerMcCheckMartingaleDrift(xState, settings.mcMartingaleDriftWarnThreshold,
                                  pureDriverOnX ? "Bergomi-on-X" : "LSV");

    const Size nBinStepsProfile = nSteps > 0 ? nSteps - 1 : 0;
    const double totalMs = binsMs + evolveMs;
    if (settings.mcLogSimulatorProfile) {
        std::cout << std::fixed << std::setprecision(3);
        if (pureDriverOnX) {
            std::cout << "  [Bergomi-on-X profile] sigma_LV=1, gHat=1; evolve(2D step): " << evolveMs
                      << " ms\n"
                      << "  [Bergomi-on-X profile] ms/evolve-step="
                      << (evolveMs / std::max<Size>(nSteps, 1)) << '\n';
        } else {
            const char* binMode = settings.lsvPathChunkSamples > 0
                                      ? "adaptive cloud min/max, lockstep chunks"
                                      : "adaptive cloud min/max";
            std::cout << "  [LSV profile] bins(" << binMode << ", linear midpoints): " << binsMs
                      << " ms (" << (100.0 * binsMs / std::max(totalMs, 1e-9)) << "%), "
                      << "evolve(2D step): " << evolveMs << " ms ("
                      << (100.0 * evolveMs / std::max(totalMs, 1e-9)) << "%)\n"
                      << "  [LSV profile] workers=" << lsvWorkers
                      << " (OpenMP=" << (useOpenMpWorkers ? "on" : "off") << ")";
            std::cout << '\n'
                      << "  [LSV profile] ms/bin-step="
                      << (binsMs / std::max<Size>(nBinStepsProfile, 1))
                      << ", ms/evolve-step=" << (evolveMs / std::max<Size>(nSteps, 1)) << '\n';
        }
    }

    return BuehlerFixingSavePath(saveFixingDates, nPaths, std::move(savedX),
                                 std::move(dividendCarryAtFixing), std::move(slopeGAtFixing),
                                 mcBrownianSteps);
}
