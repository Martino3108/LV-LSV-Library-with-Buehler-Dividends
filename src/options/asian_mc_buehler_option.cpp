/**
 * @file asian_mc_buehler_option.cpp
 * @brief Discrete Asian payoffs on the Buehler model save path (`BuehlerModel::simulateFixingPaths`).
 */

#include "asian_mc_buehler_option.h"
#include "buehler_mc_path_pricing.h"
#include "buehler_model.h"
#include "mc_observation_schedule.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace {

using namespace QuantLib;

Real callPutPayoff(const Real underlying, const Real strike, const bool isCall) {
    if (isCall)
        return std::max(underlying - strike, 0.0);
    return std::max(strike - underlying, 0.0);
}

struct AsianMcPricingSetup {
    std::vector<Size> obsIdx;
    Size nObs = 0;
    Size terminalIdx = 0;
    bool isCall = true;
    Real discount = 1.0;
    Real strike = 0.0;
};

AsianMcPricingSetup buildAsianMcPricingSetup(const BuehlerFixingSavePath& bank,
                                             const OptionContractParams& params,
                                             const BuehlerModel& buehler) {
    AsianMcPricingSetup setup;
    const std::vector<Date> fixingDates =
        resolveMcObservationDates(buehler, params, "AsianMcBuehlerOption");
    QL_REQUIRE(!fixingDates.empty(), "AsianMcBuehlerOption: empty observation schedule");
    setup.obsIdx.reserve(fixingDates.size());
    for (const Date& d : fixingDates)
        setup.obsIdx.push_back(bank.fixingIndex(d));
    setup.nObs = setup.obsIdx.size();
    setup.terminalIdx = setup.obsIdx.back();
    setup.isCall = params.isCall;
    setup.discount = buehler.riskFreeTs()->discount(params.expiry);

    QL_REQUIRE(params.expiry > buehler.today(), "AsianMcBuehlerOption: expiry must be after today");

    if (params.strike != Null<Real>())
        setup.strike = params.strike;
    return setup;
}

/** Geometric average only: the per-observation log dominates the Asian pricing cost,
 *  so arithmetic payoffs must not pay for it. */
Real pathAverageGeometric(const BuehlerFixingSavePath& bank, const Size pathIndex,
                          const AsianMcPricingSetup& setup) {
    Real logSum = 0.0;
    for (Size i = 0; i < setup.nObs; ++i) {
        const Real u = bank.sLevel(pathIndex, setup.obsIdx[i]);
        QL_REQUIRE(u > 0.0, "AsianMcBuehlerOption: spot level must be positive");
        logSum += std::log(u);
    }
    // logSum * (1/n), not logSum / n: keeps results bit-identical to the historical kernel.
    return std::exp(logSum * (1.0 / static_cast<Real>(setup.nObs)));
}

Real pathAverageArithmetic(const BuehlerFixingSavePath& bank, const Size pathIndex,
                           const AsianMcPricingSetup& setup) {
    Real sumArith = 0.0;
    for (Size i = 0; i < setup.nObs; ++i)
        sumArith += bank.sLevel(pathIndex, setup.obsIdx[i]);
    return sumArith * (1.0 / static_cast<Real>(setup.nObs));
}

Real pathPayoff(const BuehlerFixingSavePath& bank, const Size pathIndex,
                const AsianMcPayoffKind payoffKind, const AsianMcPricingSetup& setup) {
    switch (payoffKind) {
      case AsianMcPayoffKind::GeometricFixed:
          QL_REQUIRE(setup.strike != Null<Real>(), "AsianMcBuehlerOption: fixed strike requires params.strike");
          return callPutPayoff(pathAverageGeometric(bank, pathIndex, setup), setup.strike,
                               setup.isCall);
      case AsianMcPayoffKind::GeometricFloating:
          return callPutPayoff(pathAverageGeometric(bank, pathIndex, setup),
                               bank.sLevel(pathIndex, setup.terminalIdx), setup.isCall);
      case AsianMcPayoffKind::ArithmeticFixed:
          QL_REQUIRE(setup.strike != Null<Real>(), "AsianMcBuehlerOption: fixed strike requires params.strike");
          return callPutPayoff(pathAverageArithmetic(bank, pathIndex, setup), setup.strike,
                               setup.isCall);
      case AsianMcPayoffKind::ArithmeticFloating:
          return callPutPayoff(pathAverageArithmetic(bank, pathIndex, setup),
                               bank.sLevel(pathIndex, setup.terminalIdx), setup.isCall);
    }
    QL_FAIL("AsianMcBuehlerOption: unknown payoff kind");
}

} // namespace

AsianMcPayoffKind asianMcPayoffKind(const AsianMcAverageType averageType,
                                    const AsianMcStrikeStyle strikeStyle) {
    if (averageType == AsianMcAverageType::Geometric) {
        return strikeStyle == AsianMcStrikeStyle::Floating ? AsianMcPayoffKind::GeometricFloating
                                                           : AsianMcPayoffKind::GeometricFixed;
    }
    return strikeStyle == AsianMcStrikeStyle::Floating ? AsianMcPayoffKind::ArithmeticFloating
                                                      : AsianMcPayoffKind::ArithmeticFixed;
}

std::string AsianMcBuehlerOption::scenarioExportBaseName() const {
    std::string s = std::string("training_set_asian_mc_") + (params_.isCall ? "call" : "put") + "_";
    s += (averageType_ == AsianMcAverageType::Geometric ? "geom" : "arith");
    s += (strikeStyle_ == AsianMcStrikeStyle::Floating ? "_float" : "_fixed");
    s += "_S";
    return s;
}

AsianMcBuehlerOption::AsianMcBuehlerOption(OptionContractParams params, AsianMcAverageType averageType,
                                         AsianMcStrikeStyle strikeStyle)
: Option(std::move(params)), averageType_(averageType), strikeStyle_(strikeStyle) {}

BuehlerMcPathPricingResult AsianMcBuehlerOption::priceFromSavePath(const BuehlerFixingSavePath& bank,
                                                                 const AsianMcPayoffKind payoffKind,
                                                                 const OptionContractParams& params,
                                                                 const BuehlerModel& buehler) {
    const AsianMcPricingSetup setup = buildAsianMcPricingSetup(bank, params, buehler);
    BuehlerMcPayoffAccumulator stats;
    for (Size p = 0; p < bank.numPaths(); ++p)
        stats.add(pathPayoff(bank, p, payoffKind, setup));
    return stats.finish(setup.discount);
}

AsianMcFourPayoffs AsianMcBuehlerOption::priceAllPayoffsFromSavePath(const BuehlerFixingSavePath& bank,
                                                                     const OptionContractParams& params,
                                                                     const BuehlerModel& buehler) {
    const AsianMcPricingSetup setup = buildAsianMcPricingSetup(bank, params, buehler);
    const Size nPaths = bank.numPaths();

    BuehlerMcPayoffAccumulator statsGeomFixed;
    BuehlerMcPayoffAccumulator statsGeomFloating;
    BuehlerMcPayoffAccumulator statsArithFixed;
    BuehlerMcPayoffAccumulator statsArithFloating;

    QL_REQUIRE(setup.strike != Null<Real>(),
               "AsianMcBuehlerOption: fixed strike requires params.strike");
    for (Size p = 0; p < nPaths; ++p) {
        // Single scan per path: both averages accumulated together (4 payoffs share them).
        Real logSum = 0.0;
        Real sumArith = 0.0;
        for (Size i = 0; i < setup.nObs; ++i) {
            const Real u = bank.sLevel(p, setup.obsIdx[i]);
            QL_REQUIRE(u > 0.0, "AsianMcBuehlerOption: spot level must be positive");
            logSum += std::log(u);
            sumArith += u;
        }
        const Real invN = 1.0 / static_cast<Real>(setup.nObs);
        const Real avgGeom = std::exp(logSum * invN);
        const Real avgArith = sumArith * invN;
        const Real terminal = bank.sLevel(p, setup.terminalIdx);

        statsGeomFixed.add(callPutPayoff(avgGeom, setup.strike, setup.isCall));
        statsGeomFloating.add(callPutPayoff(avgGeom, terminal, setup.isCall));
        statsArithFixed.add(callPutPayoff(avgArith, setup.strike, setup.isCall));
        statsArithFloating.add(callPutPayoff(avgArith, terminal, setup.isCall));
    }

    AsianMcFourPayoffs out;
    out.geometricFixed = statsGeomFixed.finish(setup.discount);
    out.geometricFloating = statsGeomFloating.finish(setup.discount);
    out.arithmeticFixed = statsArithFixed.finish(setup.discount);
    out.arithmeticFloating = statsArithFloating.finish(setup.discount);
    return out;
}

BuehlerMcPathPricingResult AsianMcBuehlerOption::priceWithStdError(const BuehlerModel& buehler) const {
    QL_REQUIRE(buehler.hasFixingSavePath(),
               "AsianMcBuehlerOption: call simulateFixingPaths first");
    return priceFromSavePath(buehler.fixingSavePath(), asianMcPayoffKind(averageType_, strikeStyle_), params_,
                             buehler);
}

QuantLib::Real AsianMcBuehlerOption::price(const BuehlerModel& buehler) const {
    return priceWithStdError(buehler).value;
}

AsianMcFourPayoffs AsianMcBuehlerOption::priceAllPayoffs(const BuehlerModel& buehler) const {
    return priceAllPayoffsFromSavePath(buehler.fixingSavePath(), params_, buehler);
}
