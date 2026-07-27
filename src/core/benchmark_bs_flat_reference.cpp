/**
 * @file benchmark_bs_flat_reference.cpp
 */

#include "benchmark_bs_flat_reference.h"
#include "autocall_mc_buehler_option.h"
#include "buehler_mc_path_pricing.h"
#include "benchmark_numeric.h"
#include "bs_flat_mc_save_path.h"
#include "buehler_mc_path_pricing.h"
#include "market_data.h"
#include <ql/instruments/asianoption.hpp>
#include <ql/instruments/payoffs.hpp>
#include <ql/instruments/vanillaoption.hpp>
#include <ql/pricingengines/asian/analytic_discr_geom_av_price.hpp>
#include <ql/pricingengines/asian/turnbullwakemanasianengine.hpp>
#include <ql/pricingengines/blackformula.hpp>
#include <ql/pricingengines/vanilla/analyticeuropeanengine.hpp>
#include <ql/termstructures/volatility/equityfx/blackconstantvol.hpp>
#include <algorithm>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>
#include <ql/quantlib.hpp>

namespace bs_flat_reference {

const std::vector<int> kAsianSanityTenorMonths = {6, 12, 18, 24};
QuantLib::BigNatural gBsSeed = kDefaultBsSeed;
QuantLib::Size gPipelineSanityMcSamples = kPipelineSanityMcSamples;
QuantLib::Real qlEquityForward(const MarketData& md, const QuantLib::Date& t) {
    return md.spotValue() * md.repoTs()->discount(t) / md.riskFreeTs()->discount(t);
}

QuantLib::ext::shared_ptr<QuantLib::BlackScholesMertonProcess> makeQlRepoDividendBsProcess(
    const MarketData& md, const QuantLib::Date& expiry, QuantLib::Real strikeS) {
    using namespace QuantLib;
    const Volatility sigma = md.blackVolTs()->blackVol(expiry, strikeS, true);
    const auto vol = QuantLib::ext::make_shared<BlackConstantVol>(md.today(), md.calendar(), sigma,
                                                                  md.dayCounter());
    return QuantLib::ext::make_shared<BlackScholesMertonProcess>(
        md.spot(), md.repoTs(), md.riskFreeTs(), Handle<BlackVolTermStructure>(vol));
}

std::string maturityLabel(int months) {
    const int y = months / 12;
    const int m = months % 12;
    if (m == 0)
        return std::to_string(y) + "Y";
    if (y > 0)
        return std::to_string(y) + "Y" + std::to_string(m) + "M";
    return std::to_string(months) + "M";
}

QuantLib::Real priceBsEuropean(
    const QuantLib::ext::shared_ptr<QuantLib::BlackScholesMertonProcess>& process,
    const QuantLib::Date& expiry, QuantLib::Real strikeS) {
    using namespace QuantLib;
    const auto payoff =
        QuantLib::ext::make_shared<PlainVanillaPayoff>(QuantLib::Option::Call, strikeS);
    VanillaOption option(payoff, ext::make_shared<EuropeanExercise>(expiry));
    option.setPricingEngine(ext::make_shared<AnalyticEuropeanEngine>(process));
    return option.NPV();
}

QuantLib::Real priceBsCashDigitalCall(const MarketData& md, const QuantLib::Date& expiry,
                                      QuantLib::Real strikeS) {
    using namespace QuantLib;
    const Time t = md.dayCounter().yearFraction(md.today(), expiry);
    const Volatility sigma = md.blackVolTs()->blackVol(expiry, strikeS, true);
    const Real forwardS = qlEquityForward(md, expiry);
    const Real discountS = md.riskFreeTs()->discount(expiry);
    const Real stdDev = sigma * std::sqrt(t);
    const Real d2 = (std::log(forwardS / strikeS) - 0.5 * sigma * sigma * t) / stdDev;
    const CumulativeNormalDistribution phi;
    return discountS * phi(d2);
}

QuantLib::Real priceBsCashDigitalPut(const MarketData& md, const QuantLib::Date& expiry,
                                     QuantLib::Real strikeS) {
    using namespace QuantLib;
    const Time t = md.dayCounter().yearFraction(md.today(), expiry);
    const Volatility sigma = md.blackVolTs()->blackVol(expiry, strikeS, true);
    const Real forwardS = qlEquityForward(md, expiry);
    const Real discountS = md.riskFreeTs()->discount(expiry);
    const Real stdDev = sigma * std::sqrt(t);
    const Real d2 = (std::log(forwardS / strikeS) - 0.5 * sigma * sigma * t) / stdDev;
    const CumulativeNormalDistribution phi;
    return discountS * phi(-d2);
}

QuantLib::Real priceBsIndependentCouponDigitalStrip(
    const MarketData& md,
    const std::vector<QuantLib::Date>& obsDates,
    const QuantLib::Real couponAmount,
    const QuantLib::Real barrierS) {
    QuantLib::Real sum = 0.0;
    for (const QuantLib::Date& d : obsDates)
        sum += couponAmount * priceBsCashDigitalPut(md, d, barrierS);
    return sum;
}

BuehlerMcPathPricingResult priceAutocallKnockOutFromBsFlatSavePath(
    const BsFlatMcSavePath& savePath,
    const MarketData& md,
    const std::vector<QuantLib::Date>& obsDates,
    const QuantLib::Real couponAmount,
    const QuantLib::Real barrierS,
    const QuantLib::Real notional,
    const AutocallCouponStyle couponStyle) {
    using namespace QuantLib;
    QL_REQUIRE(!obsDates.empty(), "priceAutocallKnockOutFromBsFlatSavePath: empty schedule");
    QL_REQUIRE(couponAmount >= 0.0, "priceAutocallKnockOutFromBsFlatSavePath: negative coupon");
    QL_REQUIRE(notional > 0.0, "priceAutocallKnockOutFromBsFlatSavePath: notional must be positive");

    std::vector<Size> obsIdx;
    std::vector<Real> legDiscount;
    obsIdx.reserve(obsDates.size());
    legDiscount.reserve(obsDates.size());
    for (const Date& d : obsDates) {
        QL_REQUIRE(savePath.hasFixingDate(d),
                   "priceAutocallKnockOutFromBsFlatSavePath: " << d << " not on bank");
        obsIdx.push_back(savePath.fixingIndex(d));
        legDiscount.push_back(md.riskFreeTs()->discount(d));
    }

    const Size nPaths = savePath.numPaths();
    QL_REQUIRE(nPaths > 0, "priceAutocallKnockOutFromBsFlatSavePath: empty bank");
    std::vector<Real> payoffs;
    payoffs.reserve(nPaths);
    for (Size path = 0; path < nPaths; ++path) {
        Real pv = 0.0;
        Real accruedCoupons = 0.0;
        const Size nObs = obsIdx.size();
        for (Size leg = 0; leg < nObs; ++leg) {
            const Real spot = savePath.sLevel(path, obsIdx[leg]);
            const bool isLastLeg = leg + 1 == nObs;
            if (couponStyle == AutocallCouponStyle::Athena) {
                accruedCoupons += couponAmount;
                if (spot >= barrierS || isLastLeg) {
                    pv = legDiscount[leg] * (accruedCoupons + notional);
                    break;
                }
            } else {
                pv += legDiscount[leg] * couponAmount;
                if (spot >= barrierS || isLastLeg) {
                    pv += legDiscount[leg] * notional;
                    break;
                }
            }
        }
        payoffs.push_back(pv);
    }
    return buehlerMcStatsFromPayoffs(payoffs, 1.0);
}

BuehlerMcPathPricingResult priceBsAutocallFromSavePath(
    const BsFlatMcSavePath& savePath,
    const OptionContractParams& params,
    const AutocallMcTerms& terms,
    const MarketData& md) {
    using namespace QuantLib;

    QL_REQUIRE(terms.couponRatePerPeriod != Null<Real>(),
               "priceBsAutocallFromSavePath: couponRatePerPeriod required");
    QL_REQUIRE(terms.barrierFractionOfSpot != Null<Real>(),
               "priceBsAutocallFromSavePath: barrierFractionOfSpot required");
    QL_REQUIRE(terms.couponRatePerPeriod >= 0.0,
               "priceBsAutocallFromSavePath: couponRatePerPeriod must be non-negative");
    QL_REQUIRE(terms.barrierFractionOfSpot > 0.0,
               "priceBsAutocallFromSavePath: barrierFractionOfSpot must be positive");

    std::vector<Date> fixingDates = params.observationDates;
    std::sort(fixingDates.begin(), fixingDates.end());
    fixingDates.erase(std::unique(fixingDates.begin(), fixingDates.end()), fixingDates.end());
    QL_REQUIRE(!fixingDates.empty(), "priceBsAutocallFromSavePath: observationDates required");

    const Real referenceNotional =
        terms.referenceNotional != Null<Real>() ? terms.referenceNotional : md.spotValue();
    QL_REQUIRE(referenceNotional > 0.0, "priceBsAutocallFromSavePath: reference notional must be positive");

    const Real couponAmount = terms.couponRatePerPeriod * referenceNotional;
    const Real barrierS = terms.barrierFractionOfSpot * referenceNotional;
    return priceAutocallKnockOutFromBsFlatSavePath(savePath, md, fixingDates, couponAmount,
                                                   barrierS, referenceNotional,
                                                   parseAutocallCouponStyle(terms.couponStyle));
}

QuantLib::Real priceBsStockDigital(
    const QuantLib::ext::shared_ptr<QuantLib::BlackScholesMertonProcess>& process,
    const QuantLib::Date& expiry, QuantLib::Real strikeS) {
    using namespace QuantLib;
    const auto payoff =
        QuantLib::ext::make_shared<AssetOrNothingPayoff>(QuantLib::Option::Call, strikeS);
    VanillaOption option(payoff, ext::make_shared<EuropeanExercise>(expiry));
    option.setPricingEngine(ext::make_shared<AnalyticEuropeanEngine>(process));
    return option.NPV();
}

/** @brief Flat repo/dividend BS geometric Asian (same @p fixingDates as MC observation schedule). */
QuantLib::Real priceBsGeometricAsianCallFlat(const MarketData& md, const QuantLib::Date& expiry,
                                             QuantLib::Real strikeS,
                                             const std::vector<QuantLib::Date>& fixingDates) {
    using namespace QuantLib;
    QL_REQUIRE(!fixingDates.empty(), "priceBsGeometricAsianCallFlat: empty fixing schedule");
    const auto process = makeQlRepoDividendBsProcess(md, expiry, strikeS);
    const auto payoff =
        ext::make_shared<PlainVanillaPayoff>(QuantLib::Option::Call, strikeS);
    const auto asian = ext::make_shared<DiscreteAveragingAsianOption>(
        Average::Geometric, fixingDates, payoff, ext::make_shared<EuropeanExercise>(expiry));
    asian->setPricingEngine(
        ext::make_shared<AnalyticDiscreteGeometricAveragePriceAsianEngine>(process));
    // QL Asian engines return discounted NPV (BlackCalculator uses riskFreeDiscount).
    return asian->NPV();
}

/** @brief Flat repo/dividend BS arithmetic Asian (same @p fixingDates as MC observation schedule). */
QuantLib::Real priceBsArithmeticAsianCallFlat(const MarketData& md, const QuantLib::Date& expiry,
                                              QuantLib::Real strikeS,
                                              const std::vector<QuantLib::Date>& fixingDates) {
    using namespace QuantLib;
    QL_REQUIRE(!fixingDates.empty(), "priceBsArithmeticAsianCallFlat: empty fixing schedule");
    const auto process = makeQlRepoDividendBsProcess(md, expiry, strikeS);
    const auto payoff =
        ext::make_shared<PlainVanillaPayoff>(QuantLib::Option::Call, strikeS);
    const auto asian = ext::make_shared<DiscreteAveragingAsianOption>(
        Average::Arithmetic, fixingDates, payoff, ext::make_shared<EuropeanExercise>(expiry));
    asian->setPricingEngine(ext::make_shared<TurnbullWakemanAsianEngine>(process));
    return asian->NPV();
}

namespace {

std::vector<QuantLib::Date> sortedUniqueDates(std::vector<QuantLib::Date> dates) {
    std::sort(dates.begin(), dates.end());
    dates.erase(std::unique(dates.begin(), dates.end()), dates.end());
    return dates;
}

QuantLib::Real callPutPayoff(const QuantLib::Real underlying, const QuantLib::Real strike,
                             const bool isCall) {
    if (isCall)
        return std::max(underlying - strike, 0.0);
    return std::max(strike - underlying, 0.0);
}

struct AsianObsSetup {
    std::vector<QuantLib::Size> obsIdx;
    QuantLib::Size nObs = 0;
    QuantLib::Size terminalIdx = 0;
    bool isCall = true;
    QuantLib::Real discount = 1.0;
    QuantLib::Real strike = 0.0;
};

AsianObsSetup buildAsianObsSetup(const BsFlatMcSavePath& bank, const OptionContractParams& params,
                                 const MarketData& md) {
    using namespace QuantLib;
    AsianObsSetup setup;
    const std::vector<Date> fixingDates = sortedUniqueDates(params.observationDates);
    QL_REQUIRE(!fixingDates.empty(), "priceBsAsianAllPayoffsFromSavePath: empty observation schedule");
    setup.obsIdx.reserve(fixingDates.size());
    for (const Date& d : fixingDates) {
        QL_REQUIRE(bank.hasFixingDate(d),
                   "priceBsAsianAllPayoffsFromSavePath: observation " << d << " not on save path");
        setup.obsIdx.push_back(bank.fixingIndex(d));
    }
    setup.nObs = setup.obsIdx.size();
    QL_REQUIRE(bank.hasFixingDate(params.expiry),
               "priceBsAsianAllPayoffsFromSavePath: expiry not on save path");
    setup.terminalIdx = bank.fixingIndex(params.expiry);
    setup.isCall = params.isCall;
    setup.discount = md.riskFreeTs()->discount(params.expiry);
    QL_REQUIRE(params.expiry > md.today(), "priceBsAsianAllPayoffsFromSavePath: expiry after today");
    if (params.strike != Null<Real>())
        setup.strike = params.strike;
    return setup;
}

QuantLib::Real asianPathPayoff(const BsFlatMcSavePath& bank, const QuantLib::Size pathIndex,
                               const AsianMcPayoffKind payoffKind, const AsianObsSetup& setup) {
    using namespace QuantLib;
    const Real invN = 1.0 / static_cast<Real>(setup.nObs);
    Real logSum = 0.0;
    Real sumArith = 0.0;

    for (QuantLib::Size i = 0; i < setup.nObs; ++i) {
        const QuantLib::Size k = setup.obsIdx[i];
        const Real u = bank.sLevel(pathIndex, k);
        QL_REQUIRE(u > 0.0, "priceBsAsianAllPayoffsFromSavePath: spot level must be positive");
        logSum += std::log(u);
        sumArith += u;
    }

    const Real terminal = bank.sLevel(pathIndex, setup.terminalIdx);
    const Real avgGeom = std::exp(logSum * invN);
    const Real avgArith = sumArith * invN;

    switch (payoffKind) {
      case AsianMcPayoffKind::GeometricFixed:
          QL_REQUIRE(setup.strike != Null<Real>(),
                     "priceBsAsianAllPayoffsFromSavePath: fixed strike requires params.strike");
          return callPutPayoff(avgGeom, setup.strike, setup.isCall);
      case AsianMcPayoffKind::GeometricFloating:
          return callPutPayoff(avgGeom, terminal, setup.isCall);
      case AsianMcPayoffKind::ArithmeticFixed:
          QL_REQUIRE(setup.strike != Null<Real>(),
                     "priceBsAsianAllPayoffsFromSavePath: fixed strike requires params.strike");
          return callPutPayoff(avgArith, setup.strike, setup.isCall);
      case AsianMcPayoffKind::ArithmeticFloating:
          return callPutPayoff(avgArith, terminal, setup.isCall);
    }
    QL_FAIL("priceBsAsianAllPayoffsFromSavePath: unknown payoff kind");
}

struct BarrierObsSetup {
    std::vector<QuantLib::Size> obsIdx;
    QuantLib::Size expiryIdx = 0;
    bool isCall = true;
    QuantLib::Real discount = 1.0;
    QuantLib::Real strike = 0.0;
    QuantLib::Real barrierDown = 0.0;
    QuantLib::Real barrierUp = 0.0;
};

BarrierObsSetup buildBarrierObsSetup(const BsFlatMcSavePath& bank,
                                   const OptionContractParams& params, const MarketData& md) {
    using namespace QuantLib;
    QL_REQUIRE(params.expiry > md.today(), "priceBsBarrierAllPayoffsFromSavePath: expiry after today");
    QL_REQUIRE(params.strike != Null<Real>(), "priceBsBarrierAllPayoffsFromSavePath: strike required");
    QL_REQUIRE(params.barrierDown.has_value() && params.barrierUp.has_value(),
               "priceBsBarrierAllPayoffsFromSavePath: barrierDown and barrierUp required");
    QL_REQUIRE(bank.hasFixingDate(params.expiry),
               "priceBsBarrierAllPayoffsFromSavePath: expiry not on save path");

    const std::vector<Date> fixingDates = sortedUniqueDates(params.observationDates);
    QL_REQUIRE(!fixingDates.empty(),
               "priceBsBarrierAllPayoffsFromSavePath: observationDates must be non-empty");

    BarrierObsSetup setup;
    setup.obsIdx.reserve(fixingDates.size());
    for (const Date& d : fixingDates) {
        QL_REQUIRE(bank.hasFixingDate(d),
                   "priceBsBarrierAllPayoffsFromSavePath: observation " << d << " not on save path");
        setup.obsIdx.push_back(bank.fixingIndex(d));
    }
    setup.expiryIdx = bank.fixingIndex(params.expiry);
    setup.isCall = params.isCall;
    setup.discount = md.riskFreeTs()->discount(params.expiry);
    setup.strike = params.strike;
    setup.barrierDown = *params.barrierDown;
    setup.barrierUp = *params.barrierUp;
    return setup;
}

void barrierPathPayoffsAll(const BsFlatMcSavePath& bank, const QuantLib::Size pathIndex,
                           const BarrierObsSetup& setup, QuantLib::Real& downOut,
                           QuantLib::Real& downIn, QuantLib::Real& upOut, QuantLib::Real& upIn) {
    bool triggeredDown = false;
    bool triggeredUp = false;
    for (const QuantLib::Size obsIdx : setup.obsIdx) {
        const QuantLib::Real spot = bank.sLevel(pathIndex, obsIdx);
        QL_REQUIRE(spot > 0.0, "priceBsBarrierAllPayoffsFromSavePath: spot level must be positive");
        if (spot <= setup.barrierDown)
            triggeredDown = true;
        if (spot >= setup.barrierUp)
            triggeredUp = true;
    }

    const QuantLib::Real terminal = bank.sLevel(pathIndex, setup.expiryIdx);
    QL_REQUIRE(terminal > 0.0, "priceBsBarrierAllPayoffsFromSavePath: terminal spot must be positive");
    const QuantLib::Real vanilla = callPutPayoff(terminal, setup.strike, setup.isCall);

    downOut = triggeredDown ? 0.0 : vanilla;
    downIn = triggeredDown ? vanilla : 0.0;
    upOut = triggeredUp ? 0.0 : vanilla;
    upIn = triggeredUp ? vanilla : 0.0;
}

struct LookbackObsSetup {
    std::vector<QuantLib::Size> obsIdx;
    QuantLib::Size expiryIdx = 0;
    bool isCall = true;
    QuantLib::Real discount = 1.0;
    QuantLib::Real strike = 0.0;
};

LookbackObsSetup buildLookbackObsSetup(const BsFlatMcSavePath& bank,
                                       const OptionContractParams& params,
                                       const MarketData& md) {
    using namespace QuantLib;
    QL_REQUIRE(params.expiry > md.today(), "priceBsLookbackAllPayoffsFromSavePath: expiry after today");
    QL_REQUIRE(params.strike != Null<Real>(), "priceBsLookbackAllPayoffsFromSavePath: strike required");
    QL_REQUIRE(bank.hasFixingDate(params.expiry),
               "priceBsLookbackAllPayoffsFromSavePath: expiry not on save path");

    const std::vector<Date> fixingDates = sortedUniqueDates(params.observationDates);
    QL_REQUIRE(!fixingDates.empty(),
               "priceBsLookbackAllPayoffsFromSavePath: observationDates must be non-empty");

    LookbackObsSetup setup;
    setup.obsIdx.reserve(fixingDates.size());
    for (const Date& d : fixingDates) {
        QL_REQUIRE(bank.hasFixingDate(d),
                   "priceBsLookbackAllPayoffsFromSavePath: observation " << d << " not on save path");
        setup.obsIdx.push_back(bank.fixingIndex(d));
    }
    setup.expiryIdx = bank.fixingIndex(params.expiry);
    setup.isCall = params.isCall;
    setup.discount = md.riskFreeTs()->discount(params.expiry);
    setup.strike = params.strike;
    return setup;
}

void lookbackPathPayoffsAll(const BsFlatMcSavePath& bank, const QuantLib::Size pathIndex,
                            const LookbackObsSetup& setup, QuantLib::Real& fixedPayoff,
                            QuantLib::Real& floatingPayoff) {
    QuantLib::Real runningMax = 0.0;
    QuantLib::Real runningMin = 0.0;
    for (const QuantLib::Size obsIdx : setup.obsIdx) {
        const QuantLib::Real spot = bank.sLevel(pathIndex, obsIdx);
        QL_REQUIRE(spot > 0.0, "priceBsLookbackAllPayoffsFromSavePath: spot level must be positive");
        runningMax = std::max(runningMax, spot);
        runningMin = (runningMin == 0.0) ? spot : std::min(runningMin, spot);
    }
    QL_REQUIRE(runningMax > 0.0, "priceBsLookbackAllPayoffsFromSavePath: running max must be positive");
    QL_REQUIRE(runningMin > 0.0, "priceBsLookbackAllPayoffsFromSavePath: running min must be positive");

    const QuantLib::Real terminal = bank.sLevel(pathIndex, setup.expiryIdx);
    QL_REQUIRE(terminal > 0.0, "priceBsLookbackAllPayoffsFromSavePath: terminal spot must be positive");

    // Market-standard payoffs, mirroring LookbackMcBuehlerOption: fixed call on
    // the max / put on the min; floating call buys at the min / put sells at the max.
    fixedPayoff = setup.isCall ? callPutPayoff(runningMax, setup.strike, true)
                               : callPutPayoff(runningMin, setup.strike, false);
    floatingPayoff = setup.isCall ? callPutPayoff(terminal, runningMin, true)
                                  : callPutPayoff(runningMax, terminal, true);
}

} // namespace

AsianMcFourPayoffs priceBsAsianAllPayoffsFromSavePath(const BsFlatMcSavePath& savePath,
                                                      const OptionContractParams& params,
                                                      const MarketData& md) {
    const AsianObsSetup setup = buildAsianObsSetup(savePath, params, md);
    const QuantLib::Size nPaths = savePath.numPaths();

    std::vector<QuantLib::Real> payGeomFixed;
    std::vector<QuantLib::Real> payGeomFloating;
    std::vector<QuantLib::Real> payArithFixed;
    std::vector<QuantLib::Real> payArithFloating;
    payGeomFixed.reserve(nPaths);
    payGeomFloating.reserve(nPaths);
    payArithFixed.reserve(nPaths);
    payArithFloating.reserve(nPaths);

    for (QuantLib::Size p = 0; p < nPaths; ++p) {
        payGeomFixed.push_back(
            asianPathPayoff(savePath, p, AsianMcPayoffKind::GeometricFixed, setup));
        payGeomFloating.push_back(
            asianPathPayoff(savePath, p, AsianMcPayoffKind::GeometricFloating, setup));
        payArithFixed.push_back(
            asianPathPayoff(savePath, p, AsianMcPayoffKind::ArithmeticFixed, setup));
        payArithFloating.push_back(
            asianPathPayoff(savePath, p, AsianMcPayoffKind::ArithmeticFloating, setup));
    }

    AsianMcFourPayoffs out;
    out.geometricFixed = buehlerMcStatsFromPayoffs(payGeomFixed, setup.discount);
    out.geometricFloating = buehlerMcStatsFromPayoffs(payGeomFloating, setup.discount);
    out.arithmeticFixed = buehlerMcStatsFromPayoffs(payArithFixed, setup.discount);
    out.arithmeticFloating = buehlerMcStatsFromPayoffs(payArithFloating, setup.discount);
    return out;
}

BarrierMcFourPayoffs priceBsBarrierAllPayoffsFromSavePath(const BsFlatMcSavePath& savePath,
                                                          const OptionContractParams& params,
                                                          const MarketData& md) {
    const BarrierObsSetup setup = buildBarrierObsSetup(savePath, params, md);
    const QuantLib::Size nPaths = savePath.numPaths();

    std::vector<QuantLib::Real> payDownOut;
    std::vector<QuantLib::Real> payDownIn;
    std::vector<QuantLib::Real> payUpOut;
    std::vector<QuantLib::Real> payUpIn;
    payDownOut.reserve(nPaths);
    payDownIn.reserve(nPaths);
    payUpOut.reserve(nPaths);
    payUpIn.reserve(nPaths);

    for (QuantLib::Size p = 0; p < nPaths; ++p) {
        QuantLib::Real downOut = 0.0;
        QuantLib::Real downIn = 0.0;
        QuantLib::Real upOut = 0.0;
        QuantLib::Real upIn = 0.0;
        barrierPathPayoffsAll(savePath, p, setup, downOut, downIn, upOut, upIn);
        payDownOut.push_back(downOut);
        payDownIn.push_back(downIn);
        payUpOut.push_back(upOut);
        payUpIn.push_back(upIn);
    }

    BarrierMcFourPayoffs out;
    out.downOut = buehlerMcStatsFromPayoffs(payDownOut, setup.discount);
    out.downIn = buehlerMcStatsFromPayoffs(payDownIn, setup.discount);
    out.upOut = buehlerMcStatsFromPayoffs(payUpOut, setup.discount);
    out.upIn = buehlerMcStatsFromPayoffs(payUpIn, setup.discount);
    return out;
}

LookbackMcTwoPayoffs priceBsLookbackAllPayoffsFromSavePath(const BsFlatMcSavePath& savePath,
                                                            const OptionContractParams& params,
                                                            const MarketData& md) {
    const LookbackObsSetup setup = buildLookbackObsSetup(savePath, params, md);
    const QuantLib::Size nPaths = savePath.numPaths();

    std::vector<QuantLib::Real> payFixed;
    std::vector<QuantLib::Real> payFloating;
    payFixed.reserve(nPaths);
    payFloating.reserve(nPaths);

    for (QuantLib::Size p = 0; p < nPaths; ++p) {
        QuantLib::Real fixedPayoff = 0.0;
        QuantLib::Real floatingPayoff = 0.0;
        lookbackPathPayoffsAll(savePath, p, setup, fixedPayoff, floatingPayoff);
        payFixed.push_back(fixedPayoff);
        payFloating.push_back(floatingPayoff);
    }

    LookbackMcTwoPayoffs out;
    out.fixed = buehlerMcStatsFromPayoffs(payFixed, setup.discount);
    out.floating = buehlerMcStatsFromPayoffs(payFloating, setup.discount);
    return out;
}

QuantLib::Real priceBsRangeAccrual(const MarketData& md,
                                  const std::vector<QuantLib::Date>& observationDates,
                                  QuantLib::Real strikeLowS, QuantLib::Real strikeUpS) {
    QuantLib::Real sum = 0.0;
    for (const QuantLib::Date& d : observationDates) {
        sum += priceBsCashDigitalCall(md, d, strikeLowS)
             - priceBsCashDigitalCall(md, d, strikeUpS);
    }
    return sum / static_cast<QuantLib::Real>(observationDates.size());
}

std::vector<QuantLib::Date> monthlyObservationDates(const MarketData& md,
                                                    const QuantLib::Date& horizon) {
    using namespace QuantLib;
    std::vector<Date> dates;
    Date d = md.today();
    while (true) {
        d = md.calendar().advance(d, 1, Months, Following);
        if (d > horizon)
            break;
        dates.push_back(d);
    }
    if (dates.empty() || dates.back() != horizon)
        dates.push_back(horizon);
    QL_REQUIRE(!dates.empty(), "monthlyObservationDates: empty schedule");
    return dates;
}

void printTenorBanner(const std::string& maturity) {
    std::cout << "\n======== " << maturity << " ========\n" << std::flush;
}

void printFdTableHeader() {
    std::cout << "product                 BS_analytic   Buehler       |diff|\n" << std::flush;
}

void printAsianTableHeader() {
    std::cout << "product                 MC_NPV        MC_stderr   BS_ref       |diff|\n"
              << std::flush;
}

void printMcCompareRow(const std::string& product, QuantLib::Real mcValue,
                       QuantLib::Real mcStderr, QuantLib::Real bsRef, double& sumAbsErr) {
    const double absErr = std::fabs(benchmarkToDouble(mcValue) - benchmarkToDouble(bsRef));
    sumAbsErr += absErr;
    std::cout << std::fixed << std::setprecision(8);
    std::cout << std::setw(22) << std::left << product << std::right << ' ' << mcValue << ' ';
    if (mcStderr != QuantLib::Null<QuantLib::Real>())
        std::cout << mcStderr;
    else
        std::cout << '-';
    std::cout << ' ' << bsRef << ' ' << absErr << '\n' << std::flush;
}

void printFdRow(const std::string& product, QuantLib::Real npvBs, QuantLib::Real npvBuehler,
                double& sumAbsErr) {
    const double absErr = std::fabs(benchmarkToDouble(npvBuehler) - benchmarkToDouble(npvBs));
    sumAbsErr += absErr;
    std::cout << std::fixed << std::setprecision(8);
    std::cout << std::setw(22) << std::left << product << std::right << ' ' << npvBs << ' '
              << npvBuehler << ' ' << absErr
              << '\n'
              << std::flush;
}

void printAsianRow(const std::string& product, const BuehlerMcPathPricingResult& mc,
                   QuantLib::Real bsRef, double& sumAbsErr) {
    printMcCompareRow(product, mc.value, mc.errorEstimate, bsRef, sumAbsErr);
}

void printBarrierRow(const std::string& product, const BuehlerMcPathPricingResult& mc,
                     QuantLib::Real bsRef, double& sumAbsErr) {
    printMcCompareRow(product, mc.value, mc.errorEstimate, bsRef, sumAbsErr);
}

void printLookbackRow(const std::string& product, const BuehlerMcPathPricingResult& mc,
                      QuantLib::Real bsRef, double& sumAbsErr) {
    printMcCompareRow(product, mc.value, mc.errorEstimate, bsRef, sumAbsErr);
}

void printAutocallRow(const std::string& product, const BuehlerMcPathPricingResult& mc,
                      QuantLib::Real bsRef, double& sumAbsErr) {
    printMcCompareRow(product, mc.value, mc.errorEstimate, bsRef, sumAbsErr);
}

void printSanityLegend(const QuantLib::BigNatural buehlerMcSeed,
                       const QuantLib::BigNatural bsSeed, const QuantLib::Size nSubbanks,
                       const QuantLib::Size subbankSamples) {
    const QuantLib::Size totalPaths = nSubbanks * subbankSamples;
    std::cout << "BS flat MC: " << nSubbanks << " independent sub-banks x " << subbankSamples
              << " paths (" << totalPaths << " total), base seed " << bsSeed
              << "; Buehler LV MC (fast path): " << nSubbanks << " sub-banks x " << subbankSamples
              << " paths, workers " << kDefaultMcPathWorkers << ", base seed " << buehlerMcSeed
              << "; horizon " << maturityLabel(kAsianSavePathTenorMonths)
              << "; price = mean over sub-banks (stderr = stddev across sub-bank means); "
                 "Asian geom fixed = analytic; arith fixed monthly = TW if nFix<=31 else BS bank MC; "
                 "lookback = discrete MC on same banks.\n"
              << std::flush;
}

} // namespace bs_flat_reference
