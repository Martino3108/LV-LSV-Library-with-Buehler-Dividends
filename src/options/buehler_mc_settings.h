/**
 * @file buehler_mc_settings.h
 * @brief Defaults for @c BuehlerModel::simulateFixingPaths.
 */

#ifndef BUEHLER_MC_SETTINGS_H
#define BUEHLER_MC_SETTINGS_H

#include <cstdlib>
#include <ql/quantlib.hpp>

#if defined(_OPENMP)
#include <omp.h>
#endif

constexpr QuantLib::Size kDefaultMcSamples = 100000;
constexpr QuantLib::Size kDefaultMcPathWorkers = 6;
/** @brief Default MC evolution step in calendar days (ACT/365 quote clock). */
constexpr int kDefaultMcCalendarDayStep = 1;
/** @brief Default OpenMP spin-before-sleep window (ms) applied when KMP_BLOCKTIME is unset. */
constexpr int kDefaultMcBlockTimeMs = 50;
constexpr QuantLib::BigNatural kDefaultMcSeed = 42;
constexpr QuantLib::Size kDefaultLsvBins = 200;
constexpr QuantLib::Size kDefaultLsvSubbankCount = 1;

/** @brief Pure-X path dynamics for the fixing bank. */
enum class BuehlerMcDynamics {
    /** @brief @c Lsv if @c BuehlerModel::hasLsvCalibration(), else @c Lv. */
    Auto,
    Lv,
    Lsv,
    /** @brief Same LSV integrator with @f$\sigma_{\mathrm{LV}}\equiv 1@f$, @f$\hat g\equiv 1@f$ (pure Bergomi on @e X). */
    Bergomi
};

/** @brief Resolve @c Auto from model LSV state. */
inline BuehlerMcDynamics resolveBuehlerMcDynamics(BuehlerMcDynamics choice,
                                                  bool hasLsvCalibration) {
    if (choice != BuehlerMcDynamics::Auto)
        return choice;
    return hasLsvCalibration ? BuehlerMcDynamics::Lsv : BuehlerMcDynamics::Lv;
}

/** @brief MC bank simulation settings (antithetic pairs count toward @c mcSamples). */
struct BuehlerMcSettings {
    QuantLib::Size mcSamples = kDefaultMcSamples;
    QuantLib::BigNatural seed = kDefaultMcSeed;
    /** Path simulation always evolves @e X; MC pricers read @c xLevel or @c sLevel (no @f$P\,a\,c_X@f$ map). */
    /** @brief LV Dupire vs LSV (Bergomi 1F on @e X + bins). @c Auto resolves to LSV when Bergomi params are set. */
    BuehlerMcDynamics dynamics = BuehlerMcDynamics::Auto;
    /** @brief Number of equal-width LSV bins in @e X. */
    QuantLib::Size lsvBins = kDefaultLsvBins;
    /** @brief If true, use @f$\sigma_{\mathrm{LV}}\equiv 1@f$ in LSV/Bergomi MC (pure Bergomi on @e X). */
    bool lsvUnitLocalVol = false;
    /** @brief If true, use @f$\hat g\equiv 1@f$ (unit leverage) instead of binned conditional variance. */
    bool lsvForceUnitLeverage = false;
    /** @brief If true, parallelize LSV pair evolution with OpenMP (bins stay serial). */
    bool lsvUseParallelSimulator = true;
    /** @brief If true, use tabulated-σ fast LV simulator instead of QuantLib @c PathGenerator. */
    bool useFastPathSimulator = true;
    /** @brief OpenMP workers for LV fast path and LSV pair evolution (0 -> default). */
    QuantLib::Size mcPathWorkers = kDefaultMcPathWorkers;
    /** @brief If true, print per-simulation timing breakdown (LSV bins/evolve). */
    bool mcLogSimulatorProfile = false;
    /**
     * @brief Lockstep path-chunk size for pooled adaptive bins within one simulate (0 = one pool).
     */
    QuantLib::Size lsvPathChunkSamples = 0;
    /**
     * @brief Dates stored in the fixing bank after simulation.
     * Empty = store every evolution date (default). Otherwise a subset of the evolution
     * grid (default evolution is every calendar day to the horizon).
     */
    std::vector<QuantLib::Date> mcSavePathFixingDates;
    /**
     * @brief Martingale gate: warn (stderr) when @f$|\bar X_{T} - 1|@f$ at the simulation
     * horizon exceeds this threshold (binned leverage + Euler can drift the mean of the
     * unit-forward @e X). 0 disables the check.
     */
    QuantLib::Real mcMartingaleDriftWarnThreshold = 0.005;
};

inline QuantLib::BigNatural buehlerMcThreadRngSeed(const QuantLib::BigNatural baseSeed,
                                                   const unsigned threadId) {
    return baseSeed + static_cast<QuantLib::BigNatural>(threadId) * 1315423911ULL;
}

inline unsigned buehlerMcEffectivePathWorkers(const QuantLib::Size requestedWorkers,
                                              const QuantLib::Size nPairs) {
    unsigned n = requestedWorkers > 0
                     ? static_cast<unsigned>(requestedWorkers)
                     : static_cast<unsigned>(kDefaultMcPathWorkers);
    if (nPairs > 0)
        n = std::min(n, static_cast<unsigned>(nPairs));
    return std::max(1u, n);
}

inline bool buehlerMcUseOpenMpWorkers(const QuantLib::Size mcPathWorkers) {
    return mcPathWorkers > 0;
}

/**
 * @brief Apply a default OpenMP blocktime (spin-before-sleep) once, unless KMP_BLOCKTIME is set.
 *
 * macOS libomp puts worker threads to sleep in the kernel at barriers by default; over the
 * ~500 per-step barriers of a simulation this adds a fixed ~120 ms floor that dominates small
 * runs. A short spin removes it. The window is finite (not infinite) so idle threads still
 * sleep between calls, which avoids a permanent busy-wait that backfires under CPU contention.
 * An explicit @c KMP_BLOCKTIME in the environment is always respected. No-op on OpenMP runtimes
 * without @c kmp_set_blocktime (e.g. MSVC vcomp, GCC libgomp).
 */
inline void buehlerMcApplyOpenMpTuning() {
#if defined(_OPENMP) && \
    (defined(__clang__) || defined(__INTEL_COMPILER) || defined(__INTEL_LLVM_COMPILER))
    static const bool applied = [] {
        if (std::getenv("KMP_BLOCKTIME") == nullptr)
            kmp_set_blocktime(kDefaultMcBlockTimeMs);
        return true;
    }();
    (void)applied;
#endif
}

/** @brief OpenMP team size: @c OMP_NUM_THREADS if set and positive, else @c kDefaultMcPathWorkers. */
inline QuantLib::Size buehlerMcPathWorkersFromEnvironment() {
    if (const char* env = std::getenv("OMP_NUM_THREADS")) {
        char* end = nullptr;
        const long parsed = std::strtol(env, &end, 10);
        if (end != env && parsed > 0)
            return static_cast<QuantLib::Size>(parsed);
    }
    return kDefaultMcPathWorkers;
}

#endif
