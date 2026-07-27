/**
 * @file main.cpp
 * @brief Path-generation timing: LV vs LSV, 2Y horizon, 100k paths.
 */

#include "buehler_fixing_save_path.h"
#include "buehler_mc_settings.h"
#include "buehler_model.h"
#include "market_data.h"
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>

namespace {

using namespace QuantLib;

Date expiryNearYears(const MarketData& md, const double years) {
    const auto& expiries = md.expiries();
    QL_REQUIRE(!expiries.empty(), "expiryNearYears: no expiries on market data");
    Size best = 0;
    Real bestDist = std::numeric_limits<Real>::max();
    for (Size i = 0; i < expiries.size(); ++i) {
        const Real t = md.dayCounter().yearFraction(md.today(), expiries[i]);
        const Real dist = std::fabs(t - years);
        if (dist < bestDist) {
            bestDist = dist;
            best = i;
        }
    }
    return expiries[best];
}

/** Mean of X at the final fixing: regression checksum for simulator changes. */
double bankChecksum(const BuehlerFixingSavePath& bank) {
    const Size last = bank.numFixings() - 1;
    long double sum = 0.0L;
    for (Size p = 0; p < bank.numPaths(); ++p)
        sum += bank.xLevel(p, last);
    return static_cast<double>(sum / static_cast<long double>(bank.numPaths()));
}

double simulateAndTimeMs(BuehlerModel& model, const Date& horizonMax,
                         const BuehlerMcDynamics dynamics, const Size mcSamples,
                         const bool logProfile, double& checksum) {
    BuehlerMcSettings mc;
    mc.dynamics = dynamics;
    mc.mcSamples = mcSamples;
    mc.seed = kDefaultMcSeed;
    mc.mcPathWorkers = buehlerMcPathWorkersFromEnvironment();
    mc.mcLogSimulatorProfile = logProfile;

    using clock = std::chrono::steady_clock;
    const clock::time_point t0 = clock::now();
    model.simulateFixingPaths(horizonMax, {}, mc);
    const clock::time_point t1 = clock::now();

    checksum = bankChecksum(model.fixingSavePath());
    (void)model.takeFixingSavePath();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

} // namespace

// usage: pricing_engine [lv|lsv|both] [repeats] [profile]
int main(int argc, char** argv) {
    try {
        constexpr Size kMcSamples = 100000;
        constexpr double kHorizonYears = 2.0;

        const std::string mode = argc > 1 ? argv[1] : "both";
        const int repeats = argc > 2 ? std::atoi(argv[2]) : 1;
        const bool logProfile = argc > 3 && std::string(argv[3]) == "profile";
        const bool runLv = mode == "lv" || mode == "both";
        const bool runLsv = mode == "lsv" || mode == "both";
        QL_REQUIRE(runLv || runLsv, "mode must be lv, lsv or both");
        QL_REQUIRE(repeats > 0, "repeats must be positive");

        std::cout << "\nPath-generation timing (LV vs LSV)\n";
        std::cout << "Dataset: data/sample_market_snapshot (loadSampleMarketSnapshot)\n";

        MarketData md;
        md.loadSampleMarketSnapshot();
        BuehlerModel model(md);

        model.preprocessing();
        model.calibration(true);

        const Date horizon = expiryNearYears(md, kHorizonYears);
        const Time horizonT = md.dayCounter().yearFraction(md.today(), horizon);
        const Size workers = buehlerMcPathWorkersFromEnvironment();

        std::cout << std::fixed << std::setprecision(4);
        std::cout << "horizon = " << horizon << "  (T = " << horizonT << " years)\n"
                  << "paths   = " << kMcSamples << " (antithetic)\n"
                  << "workers = " << workers << " (OMP_NUM_THREADS or default)\n"
                  << "mode    = " << mode << "  repeats = " << repeats << "\n\n";

        double lvMs = 0.0;
        double lsvMs = 0.0;
        double lvChecksum = 0.0;
        double lsvChecksum = 0.0;
        for (int r = 0; r < repeats; ++r) {
            if (runLv)
                lvMs += simulateAndTimeMs(model, horizon, BuehlerMcDynamics::Lv, kMcSamples,
                                          logProfile, lvChecksum);
            if (runLsv)
                lsvMs += simulateAndTimeMs(model, horizon, BuehlerMcDynamics::Lsv, kMcSamples,
                                           logProfile, lsvChecksum);
        }
        lvMs /= repeats;
        lsvMs /= repeats;

        std::cout << std::setprecision(1);
        if (runLv)
            std::cout << "LV  simulateFixingPaths: " << std::setw(9) << lvMs << " ms (avg of "
                      << repeats << ")  checksum(mean X_T) = " << std::setprecision(12)
                      << lvChecksum << std::setprecision(1) << '\n';
        if (runLsv)
            std::cout << "LSV simulateFixingPaths: " << std::setw(9) << lsvMs << " ms (avg of "
                      << repeats << ")  checksum(mean X_T) = " << std::setprecision(12)
                      << lsvChecksum << std::setprecision(1) << '\n';
        if (runLv && runLsv)
            std::cout << std::setprecision(3)
                      << "LSV / LV: " << (lvMs > 0.0 ? lsvMs / lvMs : 0.0) << "x\n";

        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "\nFAIL: " << ex.what() << '\n';
        return 1;
    }
}
