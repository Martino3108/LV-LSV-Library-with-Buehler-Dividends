/**
 * @file lookback_mc_buehler_option.cpp
 * @brief Discrete lookback payoffs (running max and min) on the Buehler model save path.
 */

#include "lookback_mc_buehler_option.h"
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

struct LookbackMcPricingSetup {
    std::vector<Size> obsIdx;
    Size expiryIdx = 0;
    bool isCall = true;
    Real discount = 1.0;
    Real strike = 0.0;
    LookbackMcStrikeStyle strikeStyle = LookbackMcStrikeStyle::Fixed;
};

LookbackMcPricingSetup buildLookbackMcPricingSetup(const BuehlerFixingSavePath& bank,
                                                   const OptionContractParams& params,
                                                   const BuehlerModel& buehler,
                                                   const LookbackMcStrikeStyle strikeStyle) {
    QL_REQUIRE(params.expiry > buehler.today(),
               "LookbackMcBuehlerOption: expiry must be after today");
    QL_REQUIRE(bank.hasFixingDate(params.expiry),
               "LookbackMcBuehlerOption: expiry not on save path");

    const std::vector<Date> fixingDates =
        resolveMcObservationDates(buehler, params, "LookbackMcBuehlerOption");
    QL_REQUIRE(!fixingDates.empty(), "LookbackMcBuehlerOption: empty observation schedule");

    LookbackMcPricingSetup setup;
    setup.obsIdx.reserve(fixingDates.size());
    for (const Date& d : fixingDates) {
        QL_REQUIRE(bank.hasFixingDate(d),
                   "LookbackMcBuehlerOption: observation " << d << " not on save path");
        setup.obsIdx.push_back(bank.fixingIndex(d));
    }
    setup.expiryIdx = bank.fixingIndex(params.expiry);
    setup.isCall = params.isCall;
    setup.discount = buehler.riskFreeTs()->discount(params.expiry);
    setup.strikeStyle = strikeStyle;

    if (strikeStyle == LookbackMcStrikeStyle::Fixed) {
        QL_REQUIRE(params.strike != Null<Real>(),
                   "LookbackMcBuehlerOption: fixed strike requires params.strike");
        setup.strike = params.strike;
    }
    return setup;
}

void pathExtremaSpot(const BuehlerFixingSavePath& bank, const Size pathIndex,
                     const std::vector<Size>& obsIdx, Real& runningMax, Real& runningMin) {
    runningMax = 0.0;
    runningMin = 0.0;
    for (const Size idx : obsIdx) {
        const Real spot = bank.sLevel(pathIndex, idx);
        QL_REQUIRE(spot > 0.0, "LookbackMcBuehlerOption: spot level must be positive");
        runningMax = std::max(runningMax, spot);
        runningMin = (runningMin == 0.0) ? spot : std::min(runningMin, spot);
    }
    QL_REQUIRE(runningMax > 0.0, "LookbackMcBuehlerOption: running max must be positive");
    QL_REQUIRE(runningMin > 0.0, "LookbackMcBuehlerOption: running min must be positive");
}

// Market-standard payoffs: the call reads the extremum that favours it (fixed
// call on the max, floating call buys at the min), the put the opposite one
// (fixed put on the min, floating put sells at the max).
Real lookbackPayoff(const Real runningMax, const Real runningMin, const Real terminal,
                    const LookbackMcStrikeStyle strikeStyle, const Real strike,
                    const bool isCall) {
    if (strikeStyle == LookbackMcStrikeStyle::Fixed)
        return isCall ? callPutPayoff(runningMax, strike, true)
                      : callPutPayoff(runningMin, strike, false);
    return isCall ? callPutPayoff(terminal, runningMin, true)
                  : callPutPayoff(runningMax, terminal, true);
}

Real pathPayoff(const BuehlerFixingSavePath& bank, const Size pathIndex,
                const LookbackMcPricingSetup& setup) {
    Real runningMax = 0.0;
    Real runningMin = 0.0;
    pathExtremaSpot(bank, pathIndex, setup.obsIdx, runningMax, runningMin);
    const Real terminal = bank.sLevel(pathIndex, setup.expiryIdx);
    QL_REQUIRE(terminal > 0.0, "LookbackMcBuehlerOption: terminal spot must be positive");

    return lookbackPayoff(runningMax, runningMin, terminal, setup.strikeStyle, setup.strike,
                          setup.isCall);
}

struct LookbackMcBatchSetup {
    std::vector<Size> obsIdx;
    Size expiryIdx = 0;
    bool isCall = true;
    Real discount = 1.0;
    Real strike = 0.0;
};

LookbackMcBatchSetup buildLookbackMcBatchSetup(const BuehlerFixingSavePath& bank,
                                               const OptionContractParams& params,
                                               const BuehlerModel& buehler) {
    QL_REQUIRE(params.expiry > buehler.today(),
               "LookbackMcBuehlerOption: expiry must be after today");
    QL_REQUIRE(params.strike != Null<Real>(),
               "LookbackMcBuehlerOption: priceAllPayoffs requires params.strike");
    QL_REQUIRE(bank.hasFixingDate(params.expiry),
               "LookbackMcBuehlerOption: expiry not on save path");

    const std::vector<Date> fixingDates =
        resolveMcObservationDates(buehler, params, "LookbackMcBuehlerOption");
    QL_REQUIRE(!fixingDates.empty(), "LookbackMcBuehlerOption: empty observation schedule");

    LookbackMcBatchSetup setup;
    setup.obsIdx.reserve(fixingDates.size());
    for (const Date& d : fixingDates) {
        QL_REQUIRE(bank.hasFixingDate(d),
                   "LookbackMcBuehlerOption: observation " << d << " not on save path");
        setup.obsIdx.push_back(bank.fixingIndex(d));
    }
    setup.expiryIdx = bank.fixingIndex(params.expiry);
    setup.isCall = params.isCall;
    setup.discount = buehler.riskFreeTs()->discount(params.expiry);
    setup.strike = params.strike;
    return setup;
}

void pathPayoffsAll(const BuehlerFixingSavePath& bank, const Size pathIndex,
                    const LookbackMcBatchSetup& setup, Real& fixedPayoff, Real& floatingPayoff) {
    Real runningMax = 0.0;
    Real runningMin = 0.0;
    pathExtremaSpot(bank, pathIndex, setup.obsIdx, runningMax, runningMin);

    const Real terminal = bank.sLevel(pathIndex, setup.expiryIdx);
    QL_REQUIRE(terminal > 0.0, "LookbackMcBuehlerOption: terminal spot must be positive");

    fixedPayoff = lookbackPayoff(runningMax, runningMin, terminal, LookbackMcStrikeStyle::Fixed,
                                 setup.strike, setup.isCall);
    floatingPayoff = lookbackPayoff(runningMax, runningMin, terminal,
                                    LookbackMcStrikeStyle::Floating, setup.strike, setup.isCall);
}

} // namespace

std::string LookbackMcBuehlerOption::scenarioExportBaseName() const {
    return std::string("training_set_lookback_mc_") + (params_.isCall ? "call" : "put") + "_" +
           (strikeStyle_ == LookbackMcStrikeStyle::Floating ? "float" : "fixed") + "_S";
}

LookbackMcBuehlerOption::LookbackMcBuehlerOption(OptionContractParams params,
                                                 const LookbackMcStrikeStyle strikeStyle)
: Option(std::move(params)), strikeStyle_(strikeStyle) {}

BuehlerMcPathPricingResult LookbackMcBuehlerOption::priceFromSavePath(
    const BuehlerFixingSavePath& savePath,
    const OptionContractParams& params,
    const BuehlerModel& buehler,
    const LookbackMcStrikeStyle strikeStyle) {
    const LookbackMcPricingSetup setup =
        buildLookbackMcPricingSetup(savePath, params, buehler, strikeStyle);
    BuehlerMcPayoffAccumulator stats;
    for (Size p = 0; p < savePath.numPaths(); ++p)
        stats.add(pathPayoff(savePath, p, setup));
    return stats.finish(setup.discount);
}

LookbackMcTwoPayoffs LookbackMcBuehlerOption::priceAllPayoffsFromSavePath(
    const BuehlerFixingSavePath& savePath,
    const OptionContractParams& params,
    const BuehlerModel& buehler) {
    const LookbackMcBatchSetup setup = buildLookbackMcBatchSetup(savePath, params, buehler);
    const Size nPaths = savePath.numPaths();

    BuehlerMcPayoffAccumulator statsFixed;
    BuehlerMcPayoffAccumulator statsFloating;
    for (Size p = 0; p < nPaths; ++p) {
        Real fixedPayoff = 0.0;
        Real floatingPayoff = 0.0;
        pathPayoffsAll(savePath, p, setup, fixedPayoff, floatingPayoff);
        statsFixed.add(fixedPayoff);
        statsFloating.add(floatingPayoff);
    }

    LookbackMcTwoPayoffs out;
    out.fixed = statsFixed.finish(setup.discount);
    out.floating = statsFloating.finish(setup.discount);
    return out;
}

BuehlerMcPathPricingResult LookbackMcBuehlerOption::priceWithStdError(
    const BuehlerModel& buehler) const {
    QL_REQUIRE(buehler.hasFixingSavePath(),
               "LookbackMcBuehlerOption: call simulateFixingPaths first");
    return priceFromSavePath(buehler.fixingSavePath(), params_, buehler, strikeStyle_);
}

QuantLib::Real LookbackMcBuehlerOption::price(const BuehlerModel& buehler) const {
    return priceWithStdError(buehler).value;
}

LookbackMcTwoPayoffs LookbackMcBuehlerOption::priceAllPayoffs(const BuehlerModel& buehler) const {
    QL_REQUIRE(buehler.hasFixingSavePath(),
               "LookbackMcBuehlerOption: call simulateFixingPaths first");
    return priceAllPayoffsFromSavePath(buehler.fixingSavePath(), params_, buehler);
}
