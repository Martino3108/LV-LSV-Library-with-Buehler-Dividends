/**
 * @file benchmark_forward_start.cpp
 */

#include "benchmark_forward_start.h"
#include "benchmark_log.h"
#include "buehler_fixing_save_path.h"
#include "buehler_model.h"
#include "buehler_mc_settings.h"
#include "market_data.h"
#include <chrono>
#include <iomanip>
#include <iostream>
#include <string>
#include <ql/pricingengines/blackformula.hpp>
#include <ql/quantlib.hpp>

namespace forward_start_internal {

using namespace QuantLib;
using clock = std::chrono::steady_clock;

struct ForwardStartPrice {
    Real priceS = 0.0;
};

double simulateToHorizon(BuehlerModel& model, const Date& horizonMax, BuehlerMcDynamics dynamics,
                         const BigNatural seed, const Size mcSamples,
                         const Size lsvBins) {
    model.preprocessing();
    model.calibration();

    BuehlerMcSettings mcSettings;
    mcSettings.mcSamples = mcSamples;
    mcSettings.seed = seed;
    mcSettings.dynamics = dynamics;
    mcSettings.lsvBins = lsvBins;

    const clock::time_point t0 = clock::now();
    // Empty dates → every calendar day to horizon (ACT/365-aligned MC grid).
    model.simulateFixingPaths(horizonMax, {}, mcSettings);
    return std::chrono::duration<double, std::milli>(clock::now() - t0).count();
}

ForwardStartPrice mcForwardStartCallPriceS(const BuehlerFixingSavePath& bank,
                                           const BuehlerModel& model, const Date& startDate,
                                           const Date& expiryDate, const Real moneyness) {
    QL_REQUIRE(bank.hasFixingDate(expiryDate), "forward-start: expiry not on bank");
    const bool spotStart = startDate <= model.today();
    Size iStart = 0;
    if (!spotStart) {
        QL_REQUIRE(bank.hasFixingDate(startDate), "forward-start: start date not on bank");
        iStart = bank.fixingIndex(startDate);
    }
    const Size iExp = bank.fixingIndex(expiryDate);
    const Size nPaths = bank.numPaths();
    QL_REQUIRE(nPaths > 0, "forward-start: empty path bank");

    const Real sStartSpot = model.mapXtoS(model.today(), 1.0);
    const Real discount = model.riskFreeTs()->discount(expiryDate);
    long double sum = 0.0L;
    for (Size path = 0; path < nPaths; ++path) {
        const Real sStart = spotStart ? sStartSpot : bank.sLevel(path, iStart);
        const Real sExp = bank.sLevel(path, iExp);
        const Real strike = moneyness * sStart;
        sum += static_cast<long double>(std::max(sExp - strike, 0.0));
    }
    ForwardStartPrice out;
    out.priceS = static_cast<Real>(discount * sum / static_cast<long double>(nPaths));
    return out;
}

Real impliedForwardStartVolS(const BuehlerModel& model, const DayCounter& dc, const Date& startDate,
                             const Date& expiryDate, const Real moneyness, const Real priceS) {
    const Time tau = dc.yearFraction(startDate, expiryDate);
    QL_REQUIRE(tau > 0.0, "forward-start IV: non-positive forward tenor");
    constexpr Real kMinPrice = 1.0e-9;
    if (!(priceS > kMinPrice))
        return Null<Real>();

    const Real forwardT1 = model.forward0T(startDate);
    const Real forwardT2 = model.forward0T(expiryDate);
    const Real strikeS = moneyness * forwardT1;
    const Real discountT2 = model.riskFreeTs()->discount(expiryDate);
    try {
        const Real stdDevGuess = 0.2 * std::sqrt(tau);
        const Real stdDevImp = blackFormulaImpliedStdDev(
            QuantLib::Option::Call, strikeS, forwardT2, priceS, discountT2, 0.0, stdDevGuess, 1.0e-8,
            200);
        const Real sigma = stdDevImp / std::sqrt(tau);
        if (std::isfinite(sigma) && sigma > 0.0)
            return sigma;
    } catch (...) {
    }
    return Null<Real>();
}

Date resolveForwardStartDate(const MarketData& md, const BuehlerFixingSavePath& bank,
                             const int startYears) {
    if (startYears <= 0) {
        return md.today();
    }
    const Date target =
        dateFromAct365YearFraction(md.today(), static_cast<Real>(startYears), md.calendar());
    QL_REQUIRE(bank.hasFixingDate(target),
               "resolveForwardStartDate: ACT/365 T1 " << target
               << " not on MC bank (horizon must cover that calendar date)");
    return target;
}

std::string formatT1Label(const int startYears) {
    if (startYears <= 0) {
        return "0 (today)";
    }
    return std::to_string(startYears) + "Y";
}

void printForwardStartSmileHeader(const MarketData& md, const int startYears,
                                  const Date& startDate, const int expiryYears,
                                  const Date& expiryDate, const QuantLib::Size mcSamples,
                                  const QuantLib::Size lsvBins, const BigNatural lvSeed,
                                  const bool sharedSimulation) {
    using namespace QuantLib;
    const Time forwardTenor = md.dayCounter().yearFraction(startDate, expiryDate);
    std::cout << std::fixed;
    std::cout << "\n=== forward_start_smile_comparison (LV vs LSV MC) ===\n"
              << "Pipeline: simulate paths in X -> map to S via D(T), G(T) -> forward-start payoff in S\n"
              << "T1=" << formatT1Label(startYears) << " (" << startDate << "), T2=" << expiryYears
              << "Y (" << expiryDate << "), tau=" << std::setprecision(4) << forwardTenor
              << "Y, K = kx * S(T1)  (kx = denseXStrikes from BuehlerModel)\n"
              << "Payoff: D(T2) E[max(S(T2) - kx S(T1), 0)]; IV: RR FS with Buehler F(T1), F(T2), D(T2)\n"
              << "Dynamics: LV Dupire | LSV (Dupire + bins)\n"
              << "mcSamples=" << mcSamples << ", lsvBins=" << lsvBins << ", seed=" << lvSeed;
    if (sharedSimulation) {
        std::cout << " (single LV/LSV simulation reused across T1)";
    }
    std::cout << '\n';
}

void printForwardStartSmileRows(const MarketData& md, const BuehlerModel& lvModel,
                                const BuehlerModel& lsvModel, const Date& startDate,
                                const Date& expiryDate, const double lvSimMs,
                                const double lsvSimMs) {
    using namespace QuantLib;
    const std::vector<Real>& moneynessGrid = lvModel.denseXStrikes();
    QL_REQUIRE(!moneynessGrid.empty(),
               "forward_start_smile: empty denseXStrikes after calibration");

    std::cout << std::setprecision(4);
    std::cout << "kx grid: n=" << moneynessGrid.size() << ", [" << moneynessGrid.front() << ", "
              << moneynessGrid.back() << "]\n";
    std::cout << std::setprecision(1);
    std::cout << "LV sim: " << lvSimMs << " ms | LSV sim: " << lsvSimMs << " ms\n\n";
    std::cout << std::setprecision(4);
    std::cout << "     kx   sigma_LV   sigma_LSV\n";

    for (Size i = 0; i < moneynessGrid.size(); ++i) {
        const Real m = moneynessGrid[i];
        const ForwardStartPrice lvPx =
            mcForwardStartCallPriceS(lvModel.fixingSavePath(), lvModel, startDate, expiryDate, m);
        const ForwardStartPrice lsvPx =
            mcForwardStartCallPriceS(lsvModel.fixingSavePath(), lsvModel, startDate, expiryDate, m);

        const Real ivLv =
            impliedForwardStartVolS(lvModel, md.dayCounter(), startDate, expiryDate, m, lvPx.priceS);
        const Real ivLsv =
            impliedForwardStartVolS(lsvModel, md.dayCounter(), startDate, expiryDate, m,
                                    lsvPx.priceS);

        std::cout << std::setw(7) << m << ' ';
        if (ivLv == Null<Real>())
            std::cout << std::setw(10) << "n/a";
        else
            std::cout << std::setw(10) << ivLv;
        std::cout << ' ';
        if (ivLsv == Null<Real>())
            std::cout << std::setw(11) << "n/a";
        else
            std::cout << std::setw(11) << ivLsv;
        std::cout << '\n';
    }
}

} // namespace forward_start_internal

void forward_start_smile_t1_sweep(const MarketData& md, const int expiryYears,
                                  const int t1MaxYears, const QuantLib::Size mcSamples,
                                  const QuantLib::Size lsvBins, const QuantLib::BigNatural lvSeed) {
    ScopedTeeLog logCapture("out_forward_start_sweep.txt");
    using namespace QuantLib;
    using namespace forward_start_internal;

    QL_REQUIRE(expiryYears > 0, "forward_start_smile_t1_sweep: expiryYears must be positive");
    QL_REQUIRE(t1MaxYears >= 0, "forward_start_smile_t1_sweep: t1MaxYears must be non-negative");
    QL_REQUIRE(t1MaxYears < expiryYears,
               "forward_start_smile_t1_sweep: require t1MaxYears < expiryYears");

    const Date expiryDate =
        dateFromAct365YearFraction(md.today(), static_cast<Real>(expiryYears), md.calendar());

    std::cout << std::fixed;
    std::cout << "\n=== forward_start_smile_t1_sweep ===\n"
              << "Fixed T2=" << expiryYears << "Y (" << expiryDate << "), T1 from 0 to "
              << t1MaxYears << "Y (single LV + single LSV simulation to T2)\n"
              << std::flush;

    BuehlerModel lvModel(md);
    const double lvSimMs =
        simulateToHorizon(lvModel, expiryDate, BuehlerMcDynamics::Lv, lvSeed, mcSamples,
                          lsvBins);

    BuehlerModel lsvModel(md);
    const double lsvSimMs =
        simulateToHorizon(lsvModel, expiryDate, BuehlerMcDynamics::Lsv, lvSeed, mcSamples,
                          lsvBins);

    QL_REQUIRE(lvModel.fixingSavePath().hasFixingDate(expiryDate),
               "forward_start_smile_t1_sweep: T2 not on MC bank");

    for (int startYears = 0; startYears <= t1MaxYears; ++startYears) {
        const Date startDate =
            resolveForwardStartDate(md, lvModel.fixingSavePath(), startYears);
        QL_REQUIRE(expiryDate > startDate,
                   "forward_start_smile_t1_sweep: T2 must be after T1 for startYears="
                       + std::to_string(startYears));

        printForwardStartSmileHeader(md, startYears, startDate, expiryYears, expiryDate,
                                     mcSamples, lsvBins, lvSeed, true);
        printForwardStartSmileRows(md, lvModel, lsvModel, startDate, expiryDate, lvSimMs,
                                   lsvSimMs);
    }
}
